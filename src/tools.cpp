#include "ovicode/tools.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace ovicode {

namespace fs = std::filesystem;
using nlohmann::json;

const std::vector<ToolDefinition>& builtinTools() {
    static const std::vector<ToolDefinition> v = {
        {
            "read_file",
            "Read the contents of a file at the given path, relative to the "
            "workspace. Returns the entire file as a UTF-8 string.",
            json{
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "Relative path to the file."},
                    }},
                }},
                {"required", json::array({"path"})},
            },
        },
        {
            "list_files",
            "List files and directories under a path (relative to the "
            "workspace). Omit `path` to list the workspace root. Directories "
            "are returned with a trailing slash.",
            json{
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description",
                         "Optional relative path. Defaults to '.'."},
                    }},
                }},
            },
        },
        {
            "edit_file",
            "Edit a file by replacing `old_str` with `new_str`. If the file "
            "does not exist and `old_str` is empty, the file is created with "
            "`new_str` as its contents. `old_str` must occur exactly once.",
            json{
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "Relative path to the file."},
                    }},
                    {"old_str", {
                        {"type", "string"},
                        {"description",
                         "Exact existing substring to replace. Use empty "
                         "string to create a new file."},
                    }},
                    {"new_str", {
                        {"type", "string"},
                        {"description", "Replacement string."},
                    }},
                }},
                {"required", json::array({"path", "old_str", "new_str"})},
            },
        },
    };
    return v;
}

namespace {

// Resolve a user-supplied relative path against the workspace, ensuring it
// stays inside the workspace tree.
bool resolveInside(const fs::path& workspace, const std::string& rel,
                   fs::path& out, std::string& err) {
    std::error_code ec;
    fs::path joined = workspace / rel;
    fs::path normalized = joined.lexically_normal();
    fs::path ws_normalized = workspace.lexically_normal();
    auto ws_str = ws_normalized.string();
    auto p_str = normalized.string();
    if (p_str.rfind(ws_str, 0) != 0) {
        err = "Path escapes workspace: " + rel;
        return false;
    }
    out = normalized;
    return true;
}

std::string readWholeFile(const fs::path& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        ok = false;
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

bool writeWholeFile(const fs::path& p, const std::string& s) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    return static_cast<bool>(f);
}

ToolResult runReadFile(const fs::path& workspace, const ToolCall& call) {
    ToolResult r{call.id, call.name, "", false};
    if (!call.input.contains("path") || !call.input["path"].is_string()) {
        r.is_error = true;
        r.content = "Missing required string parameter `path`.";
        return r;
    }
    fs::path resolved;
    std::string err;
    if (!resolveInside(workspace, call.input["path"].get<std::string>(),
                       resolved, err)) {
        r.is_error = true;
        r.content = err;
        return r;
    }
    std::error_code ec;
    if (!fs::exists(resolved, ec) || fs::is_directory(resolved, ec)) {
        r.is_error = true;
        r.content = "File does not exist: " +
                    call.input["path"].get<std::string>();
        return r;
    }
    bool ok = false;
    std::string body = readWholeFile(resolved, ok);
    if (!ok) {
        r.is_error = true;
        r.content = "Failed to read file.";
        return r;
    }
    r.content = body;
    return r;
}

ToolResult runListFiles(const fs::path& workspace, const ToolCall& call) {
    ToolResult r{call.id, call.name, "", false};
    std::string rel = ".";
    if (call.input.contains("path") && call.input["path"].is_string()) {
        rel = call.input["path"].get<std::string>();
        if (rel.empty()) rel = ".";
    }
    fs::path resolved;
    std::string err;
    if (!resolveInside(workspace, rel, resolved, err)) {
        r.is_error = true;
        r.content = err;
        return r;
    }
    std::error_code ec;
    if (!fs::exists(resolved, ec)) {
        r.is_error = true;
        r.content = "Path does not exist: " + rel;
        return r;
    }
    std::ostringstream ss;
    if (fs::is_directory(resolved, ec)) {
        for (const auto& entry :
             fs::recursive_directory_iterator(
                 resolved, fs::directory_options::skip_permission_denied, ec)) {
            auto p = fs::relative(entry.path(), workspace, ec).string();
            // Skip dotfile dirs commonly noisy.
            if (p.find("/.git/") != std::string::npos ||
                p.rfind(".git/", 0) == 0 || p == ".git") {
                continue;
            }
            if (entry.is_directory(ec)) p += "/";
            ss << p << "\n";
        }
    } else {
        ss << fs::relative(resolved, workspace, ec).string() << "\n";
    }
    r.content = ss.str();
    if (r.content.empty()) r.content = "(empty)";
    return r;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// Render a minimal unified-diff-like block for the user/LLM. We don't compute
// LCS — we just print every "before" line with `-` and every "after" line
// with `+`. That keeps the implementation tiny while still being a clear,
// human- and model-readable change description.
std::string buildEditDiff(const std::string& path,
                          const std::string& old_str,
                          const std::string& new_str) {
    std::ostringstream ss;
    ss << "--- a/" << path << "\n";
    ss << "+++ b/" << path << "\n";
    auto olds = splitLines(old_str);
    auto news = splitLines(new_str);
    // Drop trailing empty line introduced by terminal newline.
    if (!olds.empty() && olds.back().empty()) olds.pop_back();
    if (!news.empty() && news.back().empty()) news.pop_back();
    ss << "@@ " << olds.size() << " → " << news.size() << " lines @@\n";
    for (const auto& l : olds) ss << "-" << l << "\n";
    for (const auto& l : news) ss << "+" << l << "\n";
    return ss.str();
}

ToolResult runEditFile(const fs::path& workspace, const ToolCall& call) {
    ToolResult r{call.id, call.name, "", false};
    for (const char* k : {"path", "old_str", "new_str"}) {
        if (!call.input.contains(k) || !call.input[k].is_string()) {
            r.is_error = true;
            r.content = std::string("Missing or non-string parameter: ") + k;
            return r;
        }
    }
    fs::path resolved;
    std::string err;
    auto rel = call.input["path"].get<std::string>();
    if (!resolveInside(workspace, rel, resolved, err)) {
        r.is_error = true;
        r.content = err;
        return r;
    }
    auto old_str = call.input["old_str"].get<std::string>();
    auto new_str = call.input["new_str"].get<std::string>();
    std::error_code ec;
    bool exists = fs::exists(resolved, ec);

    if (!exists) {
        if (!old_str.empty()) {
            r.is_error = true;
            r.content =
                "File does not exist and `old_str` is non-empty. Pass an "
                "empty `old_str` to create a new file.";
            return r;
        }
        if (!writeWholeFile(resolved, new_str)) {
            r.is_error = true;
            r.content = "Failed to create file.";
            return r;
        }
        r.content = buildEditDiff(rel, "", new_str) +
                    "(created " + std::to_string(new_str.size()) + " bytes)\n";
        return r;
    }

    if (fs::is_directory(resolved, ec)) {
        r.is_error = true;
        r.content = "Path is a directory.";
        return r;
    }

    bool ok = false;
    std::string body = readWholeFile(resolved, ok);
    if (!ok) {
        r.is_error = true;
        r.content = "Failed to read file.";
        return r;
    }

    if (old_str.empty()) {
        r.is_error = true;
        r.content =
            "File already exists; `old_str` must be a unique substring to "
            "replace.";
        return r;
    }

    auto first = body.find(old_str);
    if (first == std::string::npos) {
        r.is_error = true;
        r.content = "`old_str` not found in file.";
        return r;
    }
    auto second = body.find(old_str, first + 1);
    if (second != std::string::npos) {
        r.is_error = true;
        r.content =
            "`old_str` matches more than once; widen it so the match is unique.";
        return r;
    }
    body.replace(first, old_str.size(), new_str);
    if (!writeWholeFile(resolved, body)) {
        r.is_error = true;
        r.content = "Failed to write file.";
        return r;
    }
    r.content = buildEditDiff(rel, old_str, new_str);
    return r;
}

}  // namespace

ToolResult executeTool(const fs::path& workspace, const ToolCall& call) {
    if (call.name == "read_file") return runReadFile(workspace, call);
    if (call.name == "list_files") return runListFiles(workspace, call);
    if (call.name == "edit_file") return runEditFile(workspace, call);
    ToolResult r{call.id, call.name, "Unknown tool: " + call.name, true};
    return r;
}

}  // namespace ovicode
