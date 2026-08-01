#include "ovicode/react.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include "ovicode/tools.hpp"

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

// TF-IDF / cosine retrieval over the workspace, mirroring the DPR + MIPS
// stand-in retriever from arXiv:2005.11401 (see cpp-agent/rag.cpp): tokenize
// each candidate file, build an IDF table across the corpus, then rank files by
// cosine similarity between the query's TF-IDF vector and each file's. Unlike a
// raw keyword count, IDF down-weights terms common to many files (language
// keywords, boilerplate) so distinctive matches float to the top.
std::string ragContext(const fs::path& workspace, const std::string& query,
                       size_t max_files, size_t max_bytes_per_file) {
    auto qtokens = tokenize(query);
    if (qtokens.empty()) return {};

    struct Doc {
        fs::path path;
        std::unordered_map<std::string, double> tf;  // term -> raw count
    };
    std::vector<Doc> docs;
    std::unordered_map<std::string, int> df;  // term -> document frequency

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             workspace, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& path = it->path();
        auto rel = fs::relative(path, workspace, ec).string();
        if (rel.find("/.git/") != std::string::npos ||
            rel.rfind(".git/", 0) == 0 || rel == ".git" ||
            rel.find("/build/") != std::string::npos ||
            rel.rfind("build/", 0) == 0) {
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        auto sz = fs::file_size(path, ec);
        if (ec || sz == 0 || sz > 1 * 1024 * 1024) continue;
        std::ifstream f(path);
        if (!f) continue;
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        Doc doc;
        doc.path = path;
        for (const auto& t : tokenize(body)) doc.tf[t] += 1.0;
        if (doc.tf.empty()) continue;
        for (const auto& kv : doc.tf) df[kv.first] += 1;
        docs.push_back(std::move(doc));
    }
    if (docs.empty()) return {};

    const double n = static_cast<double>(docs.size());
    std::unordered_map<std::string, double> idf;
    for (const auto& kv : df) {
        idf[kv.first] = std::log((1.0 + n) / (1.0 + kv.second)) + 1.0;
    }

    // Project raw term counts into a TF-IDF vector and report its L2 norm.
    auto vectorize = [&](const std::unordered_map<std::string, double>& tf,
                         double& vnorm) {
        std::unordered_map<std::string, double> v;
        double sum = 0.0;
        for (const auto& kv : tf) {
            auto it = idf.find(kv.first);
            double w = kv.second * (it == idf.end() ? 1.0 : it->second);
            v[kv.first] = w;
            sum += w * w;
        }
        vnorm = std::sqrt(sum);
        return v;
    };

    std::unordered_map<std::string, double> qtf;
    for (const auto& t : qtokens) qtf[t] += 1.0;
    double qnorm = 0.0;
    auto qvec = vectorize(qtf, qnorm);
    if (qnorm == 0.0) return {};

    struct Hit {
        fs::path path;
        double score;
    };
    std::vector<Hit> hits;
    for (const auto& doc : docs) {
        double dnorm = 0.0;
        auto dvec = vectorize(doc.tf, dnorm);
        if (dnorm == 0.0) continue;
        // Cosine similarity, iterating over the smaller vector.
        const auto* small = &qvec;
        const auto* large = &dvec;
        if (small->size() > large->size()) std::swap(small, large);
        double dot = 0.0;
        for (const auto& kv : *small) {
            auto it = large->find(kv.first);
            if (it != large->end()) dot += kv.second * it->second;
        }
        double sim = dot / (qnorm * dnorm);
        if (sim > 0.0) hits.push_back({doc.path, sim});
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > max_files) hits.resize(max_files);
    if (hits.empty()) return {};

    std::ostringstream ss;
    ss << "Retrieved excerpts (TF-IDF cosine scored):\n";
    for (const auto& h : hits) {
        std::ifstream f(h.path);
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.size() > max_bytes_per_file) {
            body.resize(max_bytes_per_file);
            body += "\n...[truncated]";
        }
        std::ostringstream score;
        score << std::fixed;
        score.precision(3);
        score << h.score;
        ss << "\n--- " << fs::relative(h.path, workspace).string()
           << " (score " << score.str() << ") ---\n"
           << body << "\n";
    }
    return ss.str();
}

// Run the inner tool-use loop until the model stops calling tools or we hit
// the iteration cap. Appends to `messages`.
bool runReactLoop(const AgentContext& ctx, std::vector<Message>& messages,
                  const std::string& system_prompt,
                  const TranscriptSink& sink) {
    int iteration = 0;
    while (iteration < ctx.config.max_tool_iterations) {
        ++iteration;
        ChatRequest req;
        req.system = system_prompt;
        req.messages = messages;
        req.tools = builtinTools();
        auto resp = chat(ctx.config, req);
        if (!resp.error.empty()) {
            emit(sink, TranscriptEntry::Kind::Error, "OpenRouter error",
                 resp.error);
            return false;
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
                return false;
        }

        if (resp.tool_calls.empty()) return true;

        for (const auto& tc : resp.tool_calls) {
            if (!emit(sink, TranscriptEntry::Kind::ToolCall, tc.name,
                      humanizeToolCall(tc)))
                return false;
            auto result = executeTool(ctx.workspace, tc);
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
                return false;
        }
    }
    emit(sink, TranscriptEntry::Kind::Error, "Tool loop",
         "Reached max_tool_iterations without a final answer.");
    return false;
}

struct PluginHostContext {
    const AgentContext& agent;
    std::vector<Message>& messages;
    const TranscriptSink& sink;
    std::string last_assistant_text;
    bool cancelled = false;
    int react_runs = 0;
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
    if (!user_input || context.cancelled) return 0;
    if (context.react_runs >= context.agent.config.plugin_iterations) {
        pluginEmit(&context, OVI_CODE_PLUGIN_MESSAGE_ERROR, "Technique plugin",
                   "Plugin exceeded its configured ReAct pass limit.");
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
    const bool ok = runReactLoop(context.agent, context.messages, prompt,
                                 context.sink);
    updateLastAssistantText(context);
    return ok ? 1 : 0;
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

void runTurn(const AgentContext& ctx, std::vector<Message>& messages,
             const std::string& user_input, const TranscriptSink& sink) {
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
        PluginHostContext host_context{ctx, messages, sink, "", false, 0};
        OviCodeTechniqueHostV1 host{
            OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION,
            &host_context,
            workspace.c_str(),
            base_system_prompt.c_str(),
            ctx.config.plugin_iterations,
            pluginRunReact,
            pluginLastAssistantText,
            pluginEmit,
        };
        if (!plugin->run(host, user_input) && !host_context.cancelled) {
            emit(sink, TranscriptEntry::Kind::Error, plugin->label,
                 "Technique plugin failed the turn.");
        }
        return;
    }
    if (technique_id != techniqueToString(ctx.config.technique)) {
        emit(sink, TranscriptEntry::Kind::Error, "Technique plugin",
             "Configured plugin '" + technique_id + "' is not loaded.");
        return;
    }

    std::string system_prompt =
        assembleSystemPrompt(ctx.md, ctx.config.technique, ctx.workspace);

    if (ctx.config.technique == Technique::Rag) {
        auto rag = ragContext(ctx.workspace, user_input, 4, 2000);
        if (!rag.empty()) {
            emit(sink, TranscriptEntry::Kind::System, "RAG context",
                 "Retrieved " + std::to_string(rag.size()) +
                     " bytes of workspace context.");
            Message m;
            m.role = Message::Role::User;
            m.text = rag + "\n\n--- end retrieved context ---";
            messages.push_back(m);
        }
    }

    Message user;
    user.role = Message::Role::User;
    user.text = user_input;
    messages.push_back(user);
    emit(sink, TranscriptEntry::Kind::User, "you", user_input);

    if (ctx.config.technique == Technique::RalphLoop) {
        for (int i = 0; i < ctx.config.ralph_iterations; ++i) {
            emit(sink, TranscriptEntry::Kind::System, "Ralph loop",
                 "Iteration " + std::to_string(i + 1) + " of " +
                     std::to_string(ctx.config.ralph_iterations));
            if (!runReactLoop(ctx, messages, system_prompt, sink)) return;
            // Detect DONE token in the last assistant message.
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (it->role == Message::Role::Assistant) {
                    if (it->text.find("DONE") != std::string::npos) {
                        emit(sink, TranscriptEntry::Kind::System, "Ralph loop",
                             "Model reported DONE; stopping early.");
                        return;
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
        return;
    }

    runReactLoop(ctx, messages, system_prompt, sink);
}

}  // namespace ovicode
