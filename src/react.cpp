#include "ovicode/react.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include "ovicode/tools.hpp"

namespace ovicode {

namespace fs = std::filesystem;

namespace {

bool emit(const TranscriptSink& sink, TranscriptEntry::Kind kind,
          std::string title, std::string body) {
    if (!sink) return true;
    return sink({kind, std::move(title), std::move(body)});
}

std::vector<std::string> keywords(const std::string& s) {
    static const std::unordered_set<std::string> stop = {
        "the", "a", "an", "to", "in", "of", "and", "or", "is", "are", "for",
        "with", "on", "this", "that", "it", "be", "as", "by", "do", "from",
        "we", "you", "i", "me", "my", "your", "our", "us", "can", "will",
        "should", "would", "could", "what", "how", "why", "when", "where",
        "but", "not", "so", "if", "then", "else", "into", "out", "up", "down",
        "use", "make", "add", "create", "remove", "delete", "fix", "update",
    };
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else {
            if (cur.size() >= 3 && !stop.count(cur)) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= 3 && !stop.count(cur)) out.push_back(cur);
    return out;
}

std::string ragContext(const fs::path& workspace, const std::string& query,
                       size_t max_files, size_t max_bytes_per_file) {
    auto kws = keywords(query);
    if (kws.empty()) return {};
    std::error_code ec;
    struct Hit {
        fs::path path;
        int score;
    };
    std::vector<Hit> hits;
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
        std::string lower = body;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        int score = 0;
        for (const auto& k : kws) {
            size_t pos = 0;
            while ((pos = lower.find(k, pos)) != std::string::npos) {
                ++score;
                pos += k.size();
            }
        }
        if (score > 0) hits.push_back({path, score});
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > max_files) hits.resize(max_files);
    if (hits.empty()) return {};

    std::ostringstream ss;
    ss << "Retrieved excerpts (keyword scored):\n";
    for (const auto& h : hits) {
        std::ifstream f(h.path);
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.size() > max_bytes_per_file) {
            body.resize(max_bytes_per_file);
            body += "\n...[truncated]";
        }
        ss << "\n--- " << fs::relative(h.path, workspace).string()
           << " (score " << h.score << ") ---\n"
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
            emit(sink, TranscriptEntry::Kind::Error, "Provider error",
                 resp.error);
            return false;
        }

        // Record assistant turn.
        Message assistant;
        assistant.role = Message::Role::Assistant;
        assistant.text = resp.text;
        assistant.tool_calls = resp.tool_calls;
        messages.push_back(assistant);

        if (!resp.text.empty()) {
            if (!emit(sink, TranscriptEntry::Kind::Assistant, "assistant",
                      resp.text))
                return false;
        }

        if (resp.tool_calls.empty()) return true;

        for (const auto& tc : resp.tool_calls) {
            std::ostringstream titlebuf;
            titlebuf << tc.name << "(" << tc.input.dump() << ")";
            if (!emit(sink, TranscriptEntry::Kind::ToolCall, "tool",
                      titlebuf.str()))
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
            std::string preview = result.content;
            if (preview.size() > 4000)
                preview = preview.substr(0, 4000) + "\n...[truncated]";
            if (!emit(sink, kind, tc.name, preview)) return false;
        }
    }
    emit(sink, TranscriptEntry::Kind::Error, "Tool loop",
         "Reached max_tool_iterations without a final answer.");
    return false;
}

}  // namespace

void runTurn(const AgentContext& ctx, std::vector<Message>& messages,
             const std::string& user_input, const TranscriptSink& sink) {
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
