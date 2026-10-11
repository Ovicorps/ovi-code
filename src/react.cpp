#include "ovicode/react.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <stop_token>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ovicode/context.hpp"
#include "ovicode/metrics.hpp"
#include "ovicode/tools.hpp"
#include "ovicode/utf8.hpp"

namespace ovicode {

namespace fs = std::filesystem;

namespace {

bool emit(const TranscriptSink& sink, TranscriptEntry::Kind kind,
          std::string title, std::string body) {
    if (!sink) return true;
    return sink({kind, std::move(title), std::move(body)});
}

// Produce a short human-readable description of a tool invocation that we
// can show in the transcript. The full JSON args still go to the LLM via the
// assistant message; this string is just for the user.
std::string humanizeToolCall(const ToolCall& tc) {
    auto get = [&](const char* k) -> std::string {
        if (tc.input.contains(k) && tc.input[k].is_string())
            return tc.input[k].get<std::string>();
        return {};
    };
    if (tc.name == "read_file") {
        auto p = get("path");
        return p.empty() ? "read file" : "Reading " + p;
    }
    if (tc.name == "list_files") {
        auto p = get("path");
        return "Listing " + (p.empty() ? std::string(".") : p);
    }
    if (tc.name == "edit_file") {
        auto p = get("path");
        return p.empty() ? "edit file" : "Editing " + p;
    }
    return tc.name + " " + tc.input.dump();
}

// Format a tool's result for the transcript. Edit diffs are passed through
// verbatim (the renderer colorizes `+`/`-` lines); read/list results are
// trimmed to a short preview so the panel doesn't get drowned by file dumps.
std::string humanizeToolResult(const ToolCall& tc, const ToolResult& r) {
    if (r.is_error) return r.content;
    if (tc.name == "edit_file") return r.content;  // already a diff

    auto truncated = [](const std::string& s, size_t lines_cap,
                        size_t bytes_cap) -> std::string {
        if (s.size() <= bytes_cap) {
            size_t newlines = 0;
            for (char c : s) if (c == '\n') ++newlines;
            if (newlines <= lines_cap) return s;
        }
        std::string out;
        size_t lines = 0;
        for (size_t i = 0; i < s.size() && i < bytes_cap; ++i) {
            out.push_back(s[i]);
            if (s[i] == '\n' && ++lines >= lines_cap) break;
        }
        out += "... (truncated, " + std::to_string(s.size()) + " bytes total)";
        return out;
    };

    if (tc.name == "read_file") {
        size_t lines = 0;
        for (char c : r.content) if (c == '\n') ++lines;
        std::string header =
            "(" + std::to_string(r.content.size()) + " bytes, " +
            std::to_string(lines) + " lines)\n";
        return header + truncated(r.content, 20, 2000);
    }
    if (tc.name == "list_files") {
        return truncated(r.content, 40, 4000);
    }
    return truncated(r.content, 20, 2000);
}

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        } else {
            if (cur.size() >= 3) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= 3) out.push_back(cur);
    return out;
}

struct RagDocument {
    fs::path path;
    std::uintmax_t size = 0;
    fs::file_time_type modified;
    std::unordered_map<std::string, double> tf;
    std::string excerpt;
};

struct RagCorpus {
    std::unordered_map<std::string, RagDocument> documents;
};

struct RagResult {
    std::string context;
    bool cancelled = false;
};

std::mutex rag_cache_mutex;
std::unordered_map<std::string, RagCorpus> rag_caches;

struct FileDescriptor {
    int value = -1;
    ~FileDescriptor() {
        if (value >= 0) ::close(value);
    }
    explicit FileDescriptor(int fd = -1) : value(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value(other.value) {
        other.value = -1;
    }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (value >= 0) ::close(value);
            value = other.value;
            other.value = -1;
        }
        return *this;
    }
};

bool readWorkspaceFile(int workspace_fd, const fs::path& relative,
                       std::size_t max_bytes, std::string& body) {
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& part : relative) {
        if (part == "..") return false;
    }
    FileDescriptor current(::dup(workspace_fd));
    if (current.value < 0) return false;
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        auto next = part;
        ++next;
        const int flags = next == relative.end()
                              ? O_RDONLY | O_NOFOLLOW | O_CLOEXEC
                              : O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        FileDescriptor child(::openat(current.value, part->c_str(), flags));
        if (child.value < 0) return false;
        current = std::move(child);
    }
    struct stat status {};
    if (::fstat(current.value, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > max_bytes) {
        return false;
    }
    body.assign(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < body.size()) {
        const ssize_t read_bytes =
            ::read(current.value, body.data() + offset, body.size() - offset);
        if (read_bytes <= 0) return false;
        offset += static_cast<std::size_t>(read_bytes);
    }
    return true;
}

bool ignoredRagDirectory(const fs::path& path) {
    const std::string name = path.filename().string();
    return name == ".git" || name == "build" || name == "site" ||
           name == ".cache" || name.starts_with("build-");
}

bool likelyBinary(const std::string& body) {
    const std::size_t sample = std::min<std::size_t>(body.size(), 4096);
    return body.find('\0') < sample;
}

RagResult ragContext(const fs::path& workspace, const std::string& query,
                     size_t max_files, size_t max_bytes_per_file,
                     std::stop_token stop, int trusted_workspace_fd) {
    metrics::ScopedTimer timer(metrics::Distribution::RagDurationUs);
    const auto query_tokens = tokenize(query);
    if (query_tokens.empty()) return {};

    constexpr std::size_t kMaxIndexedFiles = 5000;
    constexpr std::size_t kMaxVisitedEntries = 20000;
    constexpr std::uintmax_t kMaxFileBytes = 1024 * 1024;
    constexpr std::uintmax_t kMaxCorpusBytes = 32 * 1024 * 1024;
    std::error_code error;
    const std::string cache_key = fs::weakly_canonical(workspace, error).string();
    if (error) return {};
    FileDescriptor workspace_fd(
        trusted_workspace_fd >= 0
            ? ::dup(trusted_workspace_fd)
            : ::open(cache_key.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (workspace_fd.value < 0) return {};

    std::lock_guard<std::mutex> cache_lock(rag_cache_mutex);
    RagCorpus& corpus = rag_caches[cache_key];
    std::unordered_set<std::string> live_paths;
    std::uintmax_t indexed_bytes = 0;
    std::size_t visited_entries = 0;

    for (fs::recursive_directory_iterator it(
             workspace, fs::directory_options::skip_permission_denied, error),
         end;
         it != end && !error; it.increment(error)) {
        if (stop.stop_requested()) return {{}, true};
        if (++visited_entries > kMaxVisitedEntries) {
            break;
        }
        if (it->is_directory(error) && ignoredRagDirectory(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_symlink(error)) continue;
        if (!it->is_regular_file(error)) continue;
        const auto size = it->file_size(error);
        if (error || size == 0 || size > kMaxFileBytes) {
            error.clear();
            continue;
        }
        if (indexed_bytes + size > kMaxCorpusBytes ||
            live_paths.size() >= kMaxIndexedFiles) {
            break;
        }
        const auto modified = it->last_write_time(error);
        if (error) {
            error.clear();
            continue;
        }
        const fs::path relative = fs::relative(it->path(), cache_key, error);
        if (error || relative.empty() || relative.is_absolute()) {
            error.clear();
            continue;
        }
        const std::string key = relative.lexically_normal().string();
        live_paths.insert(key);
        indexed_bytes += size;
        const auto cached = corpus.documents.find(key);
        if (cached != corpus.documents.end() && cached->second.size == size &&
            cached->second.modified == modified) {
            metrics::add(metrics::Counter::RagCacheHits);
            continue;
        }

        std::string body;
        if (!readWorkspaceFile(workspace_fd.value, relative, kMaxFileBytes,
                               body)) {
            corpus.documents.erase(key);
            continue;
        }
        if (stop.stop_requested()) return {{}, true};
        if (likelyBinary(body) || !utf8::isValid(body)) {
            corpus.documents.erase(key);
            continue;
        }
        RagDocument document;
        document.path = fs::path(cache_key) / relative;
        document.size = size;
        document.modified = modified;
        for (const auto& token : tokenize(body)) document.tf[token] += 1.0;
        if (document.tf.empty()) {
            corpus.documents.erase(key);
            continue;
        }
        if (body.size() > max_bytes_per_file) {
            body = utf8::truncateBytes(body, max_bytes_per_file);
            body += "\n...[truncated]";
        }
        document.excerpt = std::move(body);
        corpus.documents[key] = std::move(document);
        metrics::add(metrics::Counter::RagFilesScanned);
        metrics::add(metrics::Counter::RagInputBytes, size);
    }
    // Unseen cached documents are never ranked. This bounds cache memory and
    // prevents deleted/stale content from surviving a partial scan.
    for (auto it = corpus.documents.begin(); it != corpus.documents.end();) {
        if (!live_paths.contains(it->first)) {
            it = corpus.documents.erase(it);
        } else {
            ++it;
        }
    }

    std::unordered_map<std::string, int> document_frequency;
    for (const auto& [key, document] : corpus.documents) {
        (void)key;
        for (const auto& [term, count] : document.tf) {
            (void)count;
            ++document_frequency[term];
        }
    }
    if (corpus.documents.empty()) return {};

    const double document_count =
        static_cast<double>(corpus.documents.size());
    std::unordered_map<std::string, double> idf;
    for (const auto& [term, frequency] : document_frequency) {
        idf[term] =
            std::log((1.0 + document_count) / (1.0 + frequency)) + 1.0;
    }

    std::unordered_map<std::string, double> query_frequency;
    for (const auto& token : query_tokens) query_frequency[token] += 1.0;
    std::unordered_map<std::string, double> query_vector;
    double query_norm = 0.0;
    for (const auto& [term, count] : query_frequency) {
        const double weight = count * idf[term];
        query_vector[term] = weight;
        query_norm += weight * weight;
    }
    query_norm = std::sqrt(query_norm);
    if (query_norm == 0.0) return {};

    struct Hit {
        const RagDocument* document;
        double score;
    };
    auto lower_score = [](const Hit& left, const Hit& right) {
        return left.score > right.score;
    };
    std::priority_queue<Hit, std::vector<Hit>, decltype(lower_score)> hits(
        lower_score);
    for (const auto& [key, document] : corpus.documents) {
        (void)key;
        if (stop.stop_requested()) return {{}, true};
        double squared_norm = 0.0;
        double dot = 0.0;
        for (const auto& [term, count] : document.tf) {
            const auto idf_it = idf.find(term);
            const double weight =
                count * (idf_it == idf.end() ? 1.0 : idf_it->second);
            squared_norm += weight * weight;
            const auto query_it = query_vector.find(term);
            if (query_it != query_vector.end()) dot += query_it->second * weight;
        }
        const double document_norm = std::sqrt(squared_norm);
        if (document_norm == 0.0) continue;
        const double score = dot / (query_norm * document_norm);
        if (score <= 0.0) continue;
        hits.push({&document, score});
        if (hits.size() > max_files) hits.pop();
    }
    if (hits.empty()) return {};

    std::vector<Hit> ordered;
    while (!hits.empty()) {
        ordered.push_back(hits.top());
        hits.pop();
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Hit& left, const Hit& right) {
                  return left.score > right.score;
              });

    std::ostringstream output;
    output << "Retrieved excerpts (TF-IDF cosine scored):\n";
    for (const auto& hit : ordered) {
        std::ostringstream score;
        score << std::fixed;
        score.precision(3);
        score << hit.score;
        output << "\n--- "
               << fs::relative(hit.document->path, workspace).string()
               << " (score " << score.str() << ") ---\n"
               << hit.document->excerpt << "\n";
    }
    RagResult result{output.str(), false};
    metrics::observe(metrics::Distribution::RagContextBytes,
                     result.context.size());
    return result;
}

// Run the inner tool-use loop until the model stops calling tools or we hit
// the iteration cap. Appends to `messages`.
TurnOutcome runReactLoop(const AgentContext& ctx,
                         std::vector<Message>& messages,
                         const std::string& system_prompt,
                         const TranscriptSink& sink, std::stop_token stop,
                         std::size_t& protected_message_index) {
    int iteration = 0;
    while (iteration < ctx.config.max_tool_iterations) {
        if (stop.stop_requested()) return TurnOutcome::Cancelled;
        ++iteration;
        const std::size_t context_ceiling =
            ctx.config.max_context_bytes == 0
                ? ctx.config.max_request_bytes
                : ctx.config.max_request_bytes == 0
                      ? ctx.config.max_context_bytes
                      : std::min(ctx.config.max_context_bytes,
                                 ctx.config.max_request_bytes);
        std::size_t removed_messages = 0;
        std::size_t serialized_bytes = 0;
        while (true) {
            const auto& tools = builtinTools();
            const ChatRequest candidate{system_prompt, messages, tools};
            try {
                serialized_bytes =
                    serializeChatRequestBody(ctx.config, candidate).size();
            } catch (const std::exception& error) {
                emit(sink, TranscriptEntry::Kind::Error, "Request encoding",
                     error.what());
                return TurnOutcome::Failed;
            }
            if (context_ceiling == 0 || serialized_bytes <= context_ceiling) {
                break;
            }
            if (protected_message_index == 0 || messages.empty()) break;
            const auto protected_end = messages.begin() +
                static_cast<std::ptrdiff_t>(std::min(
                    protected_message_index, messages.size()));
            auto next_user = std::find_if(
                messages.begin() + 1, protected_end,
                [](const Message& message) {
                    return message.role == Message::Role::User;
                });
            const std::size_t removed = static_cast<std::size_t>(
                (next_user == protected_end ? protected_end : next_user) -
                messages.begin());
            if (removed == 0) break;
            removed_messages += removed;
            messages.erase(messages.begin(), next_user);
            protected_message_index -= removed;
        }
        if (removed_messages > 0 &&
            !emit(sink, TranscriptEntry::Kind::System, "context",
                  "Compacted " + std::to_string(removed_messages) +
                      " old messages to stay within the context budget.")) {
            return TurnOutcome::Cancelled;
        }
        if (context_ceiling > 0 && serialized_bytes > context_ceiling) {
            emit(sink, TranscriptEntry::Kind::Error, "Context budget",
                 "The current serialized request is " +
                     std::to_string(serialized_bytes) +
                     " bytes and cannot fit the configured context/request "
                     "budget.");
            return TurnOutcome::Failed;
        }
        const auto& tools = builtinTools();
        ChatRequest req{system_prompt, messages, tools};
        auto resp = ctx.chat_handler(ctx.config, req, stop);
        if (resp.cancelled || stop.stop_requested()) {
            return TurnOutcome::Cancelled;
        }
        if (!resp.error.empty()) {
            emit(sink, TranscriptEntry::Kind::Error,
                  providerLabel(ctx.config.provider) + " error",
                  resp.error);
            return TurnOutcome::Failed;
        }

        // Record assistant turn.
        Message assistant;
        assistant.role = Message::Role::Assistant;
        assistant.text = resp.text;
        assistant.tool_calls = resp.tool_calls;
        assistant.reasoning_details = resp.reasoning_details;
        messages.push_back(assistant);

        if (!resp.text.empty()) {
            if (!emit(sink, TranscriptEntry::Kind::Assistant, "assistant",
                      resp.text))
                return TurnOutcome::Cancelled;
        }

        if (resp.tool_calls.empty()) return TurnOutcome::Completed;

        for (const auto& tc : resp.tool_calls) {
            if (stop.stop_requested()) return TurnOutcome::Cancelled;
            if (!emit(sink, TranscriptEntry::Kind::ToolCall, tc.name,
                      humanizeToolCall(tc)))
                return TurnOutcome::Cancelled;
            auto result = executeTool(ctx.workspace, tc, stop,
                                      ctx.workspace_fd);
            if (stop.stop_requested()) return TurnOutcome::Cancelled;
            Message m;
            m.role = Message::Role::Tool;
            m.tool_call_id = result.tool_call_id;
            m.tool_name = result.tool_name;
            m.text = result.content;
            m.is_error = result.is_error;
            messages.push_back(m);
            auto kind = result.is_error ? TranscriptEntry::Kind::Error
                                        : TranscriptEntry::Kind::ToolResult;
            if (!emit(sink, kind, tc.name, humanizeToolResult(tc, result)))
                return TurnOutcome::Cancelled;
        }
    }
    emit(sink, TranscriptEntry::Kind::Error, "Tool loop",
         "Reached max_tool_iterations without a final answer.");
    return TurnOutcome::Failed;
}

struct PluginHostContext {
    const AgentContext& agent;
    std::vector<Message>& messages;
    const TranscriptSink& sink;
    std::string last_assistant_text;
    bool cancelled = false;
    bool failed = false;
    int react_runs = 0;
    std::stop_token stop;
    std::size_t protected_message_index = 0;
    std::map<std::string, std::string> parameter_values;
};

void updateLastAssistantText(PluginHostContext& context) {
    context.last_assistant_text.clear();
    for (auto it = context.messages.rbegin(); it != context.messages.rend();
         ++it) {
        if (it->role == Message::Role::Assistant) {
            context.last_assistant_text = it->text;
            return;
        }
    }
}

int pluginEmit(void* opaque, OviCodePluginMessageKindV1 kind,
               const char* title, const char* body);

int pluginRunReact(void* opaque, const char* system_prompt,
                   const char* user_input) {
    auto& context = *static_cast<PluginHostContext*>(opaque);
    if (!user_input || context.cancelled || context.stop.stop_requested()) {
        context.cancelled = true;
        return 0;
    }
    if (context.react_runs >= context.agent.config.plugin_iterations) {
        pluginEmit(&context, OVI_CODE_PLUGIN_MESSAGE_ERROR, "Technique plugin",
                    "Plugin exceeded its configured ReAct pass limit.");
        context.failed = true;
        return 0;
    }
    ++context.react_runs;

    Message user;
    user.role = Message::Role::User;
    user.text = user_input;
    context.messages.push_back(user);
    if (!emit(context.sink, TranscriptEntry::Kind::User, "you", user.text)) {
        context.cancelled = true;
        return 0;
    }

    const std::string prompt =
        system_prompt && *system_prompt
            ? std::string(system_prompt)
            : assembleSystemPrompt(context.agent.md, Technique::Standard,
                                   context.agent.workspace);
    const TurnOutcome outcome = runReactLoop(
        context.agent, context.messages, prompt, context.sink, context.stop,
        context.protected_message_index);
    if (outcome == TurnOutcome::Cancelled) context.cancelled = true;
    if (outcome == TurnOutcome::Failed) context.failed = true;
    updateLastAssistantText(context);
    return outcome == TurnOutcome::Completed ? 1 : 0;
}

const char* pluginGetParameter(void* opaque, const char* key) {
    if (!opaque || !key) return nullptr;
    auto& context = *static_cast<PluginHostContext*>(opaque);
    const auto value = context.parameter_values.find(key);
    return value == context.parameter_values.end() ? nullptr
                                                    : value->second.c_str();
}

const char* pluginLastAssistantText(void* opaque) {
    auto& context = *static_cast<PluginHostContext*>(opaque);
    return context.last_assistant_text.c_str();
}

int pluginEmit(void* opaque, OviCodePluginMessageKindV1 kind,
               const char* title, const char* body) {
    auto& context = *static_cast<PluginHostContext*>(opaque);
    const auto entry_kind = kind == OVI_CODE_PLUGIN_MESSAGE_ERROR
                                ? TranscriptEntry::Kind::Error
                                : TranscriptEntry::Kind::System;
    const bool keep_running =
        emit(context.sink, entry_kind, title ? title : "plugin",
             body ? body : "");
    if (!keep_running) context.cancelled = true;
    return keep_running ? 1 : 0;
}

}  // namespace

TurnOutcome runTurn(const AgentContext& ctx, std::vector<Message>& messages,
                    const std::string& user_input, const TranscriptSink& sink,
                    std::stop_token stop) {
    metrics::ScopedTimer timer(metrics::Distribution::TurnDurationUs);
    metrics::add(metrics::Counter::Turns);
    const std::vector<Message> initial_messages = messages;
    auto cancel = [&] {
        messages = initial_messages;
        return TurnOutcome::Cancelled;
    };
    if (stop.stop_requested()) return cancel();

    const std::string technique_id =
        ctx.config.technique_id.empty()
            ? techniqueToString(ctx.config.technique)
            : ctx.config.technique_id;
    const LoadedTechniquePlugin* plugin =
        ctx.plugins ? ctx.plugins->find(technique_id) : nullptr;
    if (plugin) {
        const std::string base_system_prompt = assembleSystemPrompt(
            ctx.md, Technique::Standard, ctx.workspace);
        const std::string workspace = ctx.workspace.string();
        PluginHostContext host_context{ctx, messages, sink, "", false, false,
                                       0, stop, initial_messages.size(), {}};
        const auto configured_plugin =
            ctx.config.plugin_parameters.find(plugin->id);
        for (const auto& parameter : plugin->parameters) {
            std::string value = parameter.default_value;
            if (configured_plugin != ctx.config.plugin_parameters.end()) {
                const auto configured_value =
                    configured_plugin->second.find(parameter.key);
                if (configured_value != configured_plugin->second.end()) {
                    if (auto normalized = normalizePluginParameterValue(
                            parameter, configured_value->second)) {
                        value = std::move(*normalized);
                    }
                }
            }
            host_context.parameter_values.emplace(parameter.key,
                                                  std::move(value));
        }
        OviCodeTechniqueHostV1 host_v1{
            OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION,
            &host_context,
            workspace.c_str(),
            base_system_prompt.c_str(),
            ctx.config.plugin_iterations,
            pluginRunReact,
            pluginLastAssistantText,
            pluginEmit,
        };
        OviCodeTechniqueHostV2 host_v2{
            OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION_V2,
            &host_context,
            workspace.c_str(),
            base_system_prompt.c_str(),
            ctx.config.plugin_iterations,
            pluginRunReact,
            pluginLastAssistantText,
            pluginEmit,
            pluginGetParameter,
        };
        if (!plugin->run(host_v1, host_v2, user_input) &&
            !host_context.cancelled) {
            emit(sink, TranscriptEntry::Kind::Error, plugin->label,
                  "Technique plugin failed the turn.");
            return TurnOutcome::Failed;
        }
        if (host_context.cancelled) return cancel();
        return host_context.failed ? TurnOutcome::Failed
                                   : TurnOutcome::Completed;
    }
    if (technique_id != techniqueToString(ctx.config.technique)) {
        emit(sink, TranscriptEntry::Kind::Error, "Technique plugin",
             "Configured plugin '" + technique_id + "' is not loaded.");
        return TurnOutcome::Failed;
    }

    std::string system_prompt =
        assembleSystemPrompt(ctx.md, ctx.config.technique, ctx.workspace);

    if (ctx.config.technique == Technique::Rag) {
        auto rag = ragContext(ctx.workspace, user_input, 4, 2000, stop,
                              ctx.workspace_fd);
        if (rag.cancelled) return cancel();
        if (!rag.context.empty()) {
            if (!emit(sink, TranscriptEntry::Kind::System, "RAG context",
                      "Retrieved " + std::to_string(rag.context.size()) +
                          " bytes of workspace context.")) {
                return cancel();
            }
            Message m;
            m.role = Message::Role::User;
            m.text = rag.context + "\n\n--- end retrieved context ---";
            messages.push_back(m);
        }
    }

    Message user;
    user.role = Message::Role::User;
    user.text = user_input;
    messages.push_back(user);
    if (!emit(sink, TranscriptEntry::Kind::User, "you", user_input)) {
        return cancel();
    }

    if (ctx.config.technique == Technique::RalphLoop) {
        std::size_t protected_message_index = initial_messages.size();
        for (int i = 0; i < ctx.config.ralph_iterations; ++i) {
            if (stop.stop_requested()) return cancel();
            if (!emit(sink, TranscriptEntry::Kind::System, "Ralph loop",
                      "Iteration " + std::to_string(i + 1) + " of " +
                          std::to_string(ctx.config.ralph_iterations))) {
                return cancel();
            }
            const TurnOutcome outcome =
                runReactLoop(ctx, messages, system_prompt, sink, stop,
                             protected_message_index);
            if (outcome == TurnOutcome::Cancelled) return cancel();
            if (outcome == TurnOutcome::Failed) return outcome;
            // Detect DONE token in the last assistant message.
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (it->role == Message::Role::Assistant) {
                    if (it->text.find("DONE") != std::string::npos) {
                        if (!emit(sink, TranscriptEntry::Kind::System,
                                  "Ralph loop",
                                  "Model reported DONE; stopping early.")) {
                            return cancel();
                        }
                        return TurnOutcome::Completed;
                    }
                    break;
                }
            }
            // Re-inject the original goal for the next iteration.
            if (i + 1 < ctx.config.ralph_iterations) {
                Message again;
                again.role = Message::Role::User;
                again.text =
                    "Continue with the original goal. If it's already "
                    "complete, reply with `DONE` and a one-line summary. "
                    "Otherwise make one more concrete improvement.";
                messages.push_back(again);
            }
        }
        return TurnOutcome::Completed;
    }

    std::size_t protected_message_index = initial_messages.size();
    const TurnOutcome outcome =
        runReactLoop(ctx, messages, system_prompt, sink, stop,
                     protected_message_index);
    if (outcome == TurnOutcome::Cancelled) return cancel();
    return outcome;
}

}  // namespace ovicode
