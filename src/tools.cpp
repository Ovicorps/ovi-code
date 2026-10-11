#include "ovicode/tools.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ovicode/metrics.hpp"
#include "ovicode/utf8.hpp"

namespace ovicode {

namespace fs = std::filesystem;
using nlohmann::json;

const std::vector<ToolDefinition>& builtinTools() {
    static const std::vector<ToolDefinition> v = {
        {
            "read_file",
            "Read the contents of a file at the given path, relative to the "
            "workspace. Results are bounded; use start_line to continue.",
            json{
                {"type", "object"},
                {"properties", {
                    {"path", {
                        {"type", "string"},
                        {"description", "Relative path to the file."},
                    }},
                    {"start_line", {
                        {"type", "integer"},
                        {"minimum", 1},
                        {"description", "First line to return (1-based)."},
                    }},
                    {"max_lines", {
                        {"type", "integer"},
                        {"minimum", 1},
                        {"maximum", 1000},
                        {"description", "Maximum lines to return."},
                    }},
                    {"start_byte", {
                        {"type", "integer"},
                        {"minimum", 0},
                        {"description",
                         "Byte offset within start_line for a continued long "
                         "line."},
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
                    {"offset", {
                        {"type", "integer"},
                        {"minimum", 0},
                        {"description", "Number of sorted entries to skip."},
                    }},
                    {"limit", {
                        {"type", "integer"},
                        {"minimum", 1},
                        {"maximum", 1000},
                        {"description", "Maximum entries to return."},
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

constexpr std::size_t kMaxInputFileBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxReadResultBytes = 128 * 1024;
constexpr std::size_t kDefaultReadLines = 400;
constexpr std::size_t kMaxReadLines = 1000;
constexpr std::size_t kDefaultListEntries = 500;
constexpr std::size_t kMaxListEntries = 1000;
constexpr std::size_t kMaxListScannedEntries = 10000;
constexpr std::size_t kMaxListResultBytes = 128 * 1024;
constexpr std::size_t kMaxDiffBytes = 64 * 1024;
constexpr std::size_t kMaxDiffLines = 500;

bool safeRelativePath(const std::string& value, fs::path& relative) {
    fs::path input(value.empty() ? "." : value);
    if (input.is_absolute()) return false;
    relative = input.lexically_normal();
    for (const auto& part : relative) {
        if (part == "..") return false;
    }
    return true;
}

struct FileDescriptor {
    int value = -1;
    ~FileDescriptor() {
        if (value >= 0) ::close(value);
    }
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : value(fd) {}
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

FileDescriptor openParentInside(const fs::path& workspace,
                                const fs::path& path,
                                std::string& filename,
                                bool create_directories = false,
                                int trusted_workspace_fd = -1) {
    const fs::path relative = path.lexically_relative(workspace);
    if (relative.empty() || relative.is_absolute()) return {};
    for (const auto& component : relative) {
        if (component == "..") return {};
    }

    FileDescriptor current(trusted_workspace_fd >= 0
                               ? ::dup(trusted_workspace_fd)
                               : ::open(workspace.c_str(), O_RDONLY |
                                                             O_DIRECTORY |
                                                             O_CLOEXEC));
    if (current.value < 0) return {};
    auto part = relative.begin();
    while (part != relative.end()) {
        auto next = part;
        ++next;
        if (next == relative.end()) {
            filename = part->string();
            return current;
        }
        int child_fd = ::openat(current.value, part->c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                    O_CLOEXEC);
        if (child_fd < 0 && create_directories && errno == ENOENT) {
            if (::mkdirat(current.value, part->c_str(), 0700) != 0) return {};
            child_fd = ::openat(current.value, part->c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                    O_CLOEXEC);
        }
        FileDescriptor child(child_fd);
        if (child.value < 0) return {};
        current = std::move(child);
        part = next;
    }
    return {};
}

// Resolve a user-supplied relative path against the workspace, ensuring it
// stays inside the workspace tree.
bool resolveInside(const fs::path& workspace, const std::string& rel,
                    fs::path& out, std::string& err) {
    fs::path relative;
    if (!safeRelativePath(rel, relative)) {
        err = "Path escapes workspace: " + rel;
        return false;
    }
    out = workspace / relative;
    return true;
}

std::string readWholeFile(const fs::path& workspace, const fs::path& p, bool& ok,
                           std::size_t max_bytes = kMaxInputFileBytes,
                           int workspace_fd = -1) {
    std::string filename;
    FileDescriptor parent =
        openParentInside(workspace, p, filename, false, workspace_fd);
    if (parent.value < 0) {
        ok = false;
        return {};
    }
    FileDescriptor file(::openat(parent.value, filename.c_str(),
                                 O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat status {};
    if (file.value < 0 || ::fstat(file.value, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > max_bytes) {
        ok = false;
        return {};
    }
    std::string body(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < body.size()) {
        const ssize_t read_bytes =
            ::read(file.value, body.data() + offset, body.size() - offset);
        if (read_bytes <= 0) {
            ok = false;
            return {};
        }
        offset += static_cast<std::size_t>(read_bytes);
    }
    ok = true;
    return body;
}

bool writeWholeFile(const fs::path& workspace, const fs::path& p,
                    const std::string& s, bool create_exclusive = false,
                    int workspace_fd = -1) {
    std::string filename;
    FileDescriptor parent =
        openParentInside(workspace, p, filename, true, workspace_fd);
    if (parent.value < 0) return false;
    const int flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW |
                      (create_exclusive ? O_CREAT | O_EXCL : O_TRUNC);
    FileDescriptor file(::openat(parent.value, filename.c_str(), flags, 0600));
    if (file.value < 0) return false;
    std::size_t offset = 0;
    while (offset < s.size()) {
        const ssize_t written =
            ::write(file.value, s.data() + offset, s.size() - offset);
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::size_t integerArgument(const ToolCall& call, const char* name,
                            std::size_t fallback, std::size_t minimum,
                            std::size_t maximum) {
    if (!call.input.contains(name) || !call.input[name].is_number_integer()) {
        return fallback;
    }
    long long value = 0;
    try {
        value = call.input[name].get<long long>();
    } catch (...) {
        return fallback;
    }
    if (value < static_cast<long long>(minimum)) return minimum;
    return std::min(static_cast<std::size_t>(value), maximum);
}

bool ignoredDirectory(const fs::path& path) {
    const std::string name = path.filename().string();
    return name == ".git" || name == "build" || name == "site" ||
           name == ".cache" || name.starts_with("build-");
}

bool collectDirectoryEntries(int directory_fd, const fs::path& prefix,
                             std::vector<std::string>& entries,
                             std::stop_token stop) {
    const int duplicate = ::dup(directory_fd);
    if (duplicate < 0) return false;
    DIR* directory = ::fdopendir(duplicate);
    if (!directory) {
        ::close(duplicate);
        return false;
    }
    while (dirent* item = ::readdir(directory)) {
        if (stop.stop_requested()) {
            ::closedir(directory);
            return false;
        }
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        struct stat status {};
        if (::fstatat(directory_fd, name.c_str(), &status,
                      AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(status.st_mode)) {
            continue;
        }
        const fs::path relative = prefix / name;
        if (S_ISDIR(status.st_mode)) {
            if (ignoredDirectory(relative)) continue;
            entries.push_back(relative.string() + "/");
            if (entries.size() >= kMaxListScannedEntries) break;
            FileDescriptor child(::openat(
                directory_fd, name.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (child.value >= 0 &&
                !collectDirectoryEntries(child.value, relative, entries,
                                         stop)) {
                ::closedir(directory);
                return false;
            }
        } else if (S_ISREG(status.st_mode)) {
            entries.push_back(relative.string());
        }
        if (entries.size() >= kMaxListScannedEntries) break;
    }
    ::closedir(directory);
    return true;
}

ToolResult runReadFile(const fs::path& workspace, const ToolCall& call,
                       std::stop_token stop, int workspace_fd) {
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
    if (stop.stop_requested()) {
        r.is_error = true;
        r.content = "Cancelled.";
        return r;
    }
    bool ok = false;
    std::string body =
        readWholeFile(workspace, resolved, ok, kMaxInputFileBytes, workspace_fd);
    if (!ok) {
        r.is_error = true;
        r.content = "Failed to read file or file exceeds the 4 MiB limit.";
        return r;
    }
    if (!utf8::isValid(body)) {
        r.is_error = true;
        r.content = "File is not valid UTF-8.";
        return r;
    }
    const std::size_t start_line = integerArgument(
        call, "start_line", 1, 1, std::numeric_limits<std::size_t>::max());
    const std::size_t max_lines = integerArgument(
        call, "max_lines", kDefaultReadLines, 1, kMaxReadLines);
    const std::size_t start_byte = integerArgument(
        call, "start_byte", 0, 0, kMaxInputFileBytes);
    std::istringstream input(body);
    std::string line;
    std::size_t line_number = 0;
    std::size_t returned = 0;
    bool more = false;
    while (std::getline(input, line)) {
        if (stop.stop_requested()) {
            r.is_error = true;
            r.content = "Cancelled.";
            return r;
        }
        ++line_number;
        if (line_number < start_line) continue;
        std::size_t line_offset = line_number == start_line ? start_byte : 0;
        line_offset = utf8::clampBoundary(line, line_offset);
        std::string_view remaining(line);
        remaining.remove_prefix(std::min(line_offset, remaining.size()));
        constexpr std::size_t kMarkerReserve = 80;
        if (returned >= max_lines) {
            more = true;
            break;
        }
        if (r.content.size() + remaining.size() + 1 + kMarkerReserve >
                kMaxReadResultBytes) {
            const std::size_t available =
                kMaxReadResultBytes - kMarkerReserve - r.content.size();
            if (available > 0 && !remaining.empty()) {
                const std::string fragment =
                    utf8::truncateBytes(remaining, available);
                r.content += fragment;
                const std::size_t next_byte = line_offset + fragment.size();
                r.content += "\n...[truncated; continue with start_line=" +
                             std::to_string(line_number) +
                             ", start_byte=" + std::to_string(next_byte) +
                             "]";
                return r;
            }
            more = true;
            break;
        }
        r.content += remaining;
        if (!input.eof() || body.ends_with('\n')) r.content += '\n';
        ++returned;
    }
    if (r.content.empty() && start_line == 1 && body.empty()) r.content = "";
    if (more) {
        r.content += "\n...[truncated; continue with start_line=" +
                     std::to_string(line_number) + "]";
    }
    r.content = utf8::truncateBytes(r.content, kMaxReadResultBytes);
    return r;
}

ToolResult runListFiles(const fs::path& workspace, const ToolCall& call,
                         std::stop_token stop, int workspace_fd) {
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
    const std::size_t offset = integerArgument(
        call, "offset", 0, 0, kMaxListScannedEntries);
    const std::size_t limit = integerArgument(
        call, "limit", kDefaultListEntries, 1, kMaxListEntries);
    std::vector<std::string> entries;
    bool scan_truncated = false;
    fs::path relative;
    if (!safeRelativePath(rel, relative)) {
        r.is_error = true;
        r.content = "Path escapes workspace: " + rel;
        return r;
    }
    FileDescriptor current(workspace_fd >= 0
                               ? ::dup(workspace_fd)
                               : ::open(workspace.c_str(), O_RDONLY |
                                                             O_DIRECTORY |
                                                             O_CLOEXEC));
    if (current.value < 0) {
        r.is_error = true;
        r.content = "Cannot open workspace.";
        return r;
    }
    fs::path prefix;
    for (auto component = relative.begin(); component != relative.end();
         ++component) {
        if (*component == ".") continue;
        auto next = component;
        ++next;
        const bool last = next == relative.end();
        FileDescriptor child(::openat(current.value, component->c_str(),
                                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                                          (last ? 0 : O_DIRECTORY)));
        if (child.value < 0) {
            r.is_error = true;
            r.content = "Path is not a directory or does not exist: " + rel;
            return r;
        }
        current = std::move(child);
        prefix /= *component;
    }
    struct stat listed_status {};
    if (::fstat(current.value, &listed_status) != 0) {
        r.is_error = true;
        r.content = "Failed to inspect path: " + rel;
        return r;
    }
    if (S_ISREG(listed_status.st_mode)) {
        entries.push_back(prefix.string());
    } else if (!S_ISDIR(listed_status.st_mode) ||
               !collectDirectoryEntries(current.value, prefix, entries, stop)) {
        r.is_error = true;
        r.content = stop.stop_requested() ? "Cancelled."
                                          : "Failed to list directory.";
        return r;
    }
    scan_truncated = entries.size() >= kMaxListScannedEntries;
    std::sort(entries.begin(), entries.end());
    const std::size_t begin = std::min(offset, entries.size());
    const std::size_t end = std::min(entries.size(), begin + limit);
    std::size_t next_offset = begin;
    constexpr std::size_t kMarkerReserve = 80;
    for (std::size_t i = begin; i < end; ++i) {
        if (r.content.size() + entries[i].size() + 1 + kMarkerReserve >
            kMaxListResultBytes) {
            scan_truncated = true;
            break;
        }
        r.content += entries[i] + '\n';
        next_offset = i + 1;
    }
    if (next_offset < entries.size() && next_offset < kMaxListScannedEntries) {
        r.content += "...[truncated; continue with offset=" +
                     std::to_string(next_offset) + "]\n";
    } else if (scan_truncated) {
        r.content += "...[scan limit reached; narrow path or query]\n";
    }
    r.content = utf8::truncateBytes(r.content, kMaxListResultBytes);
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
    std::size_t output_bytes = ss.str().size();
    std::size_t emitted = 0;
    bool truncated = false;
    auto append = [&](char prefix, const std::string& line) {
        if (emitted >= kMaxDiffLines) {
            truncated = true;
            return;
        }
        if (output_bytes + line.size() + 2 > kMaxDiffBytes) {
            truncated = true;
            return;
        }
        ss << prefix << line << "\n";
        output_bytes += line.size() + 2;
        ++emitted;
    };
    for (const auto& line : olds) append('-', line);
    for (const auto& line : news) append('+', line);
    if (truncated) {
        ss << "... (diff truncated; " << olds.size() << " old and "
           << news.size() << " new lines total)\n";
    }
    return utf8::truncateBytes(ss.str(), kMaxDiffBytes);
}

ToolResult runEditFile(const fs::path& workspace, const ToolCall& call,
                       std::stop_token stop, int workspace_fd) {
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
    if (new_str.size() > kMaxInputFileBytes || old_str.size() > kMaxInputFileBytes) {
        r.is_error = true;
        r.content = "Edit parameters exceed the 4 MiB limit.";
        return r;
    }
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
        if (stop.stop_requested()) {
            r.is_error = true;
            r.content = "Cancelled.";
            return r;
        }
        if (!writeWholeFile(workspace, resolved, new_str, true, workspace_fd)) {
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
    std::string body =
        readWholeFile(workspace, resolved, ok, kMaxInputFileBytes, workspace_fd);
    if (!ok) {
        r.is_error = true;
        r.content = "Failed to read file or file exceeds the 4 MiB limit.";
        return r;
    }
    if (!utf8::isValid(body)) {
        r.is_error = true;
        r.content = "File is not valid UTF-8.";
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
    if (body.size() > kMaxInputFileBytes) {
        r.is_error = true;
        r.content = "Edited file would exceed the 4 MiB limit.";
        return r;
    }
    if (stop.stop_requested()) {
        r.is_error = true;
        r.content = "Cancelled.";
        return r;
    }
    if (!writeWholeFile(workspace, resolved, body, false, workspace_fd)) {
        r.is_error = true;
        r.content = "Failed to write file.";
        return r;
    }
    r.content = buildEditDiff(rel, old_str, new_str);
    return r;
}

}  // namespace

ToolResult executeTool(const fs::path& workspace, const ToolCall& call,
                       std::stop_token stop, int workspace_fd) {
    metrics::ScopedTimer timer(metrics::Distribution::ToolDurationUs);
    metrics::add(metrics::Counter::ToolCalls);
    ToolResult r;
    if (call.name == "read_file") {
        r = runReadFile(workspace, call, stop, workspace_fd);
    } else if (call.name == "list_files") {
        r = runListFiles(workspace, call, stop, workspace_fd);
    } else if (call.name == "edit_file") {
        r = runEditFile(workspace, call, stop, workspace_fd);
    } else {
        r = ToolResult{call.id, call.name, "Unknown tool: " + call.name, true};
    }
    if (r.is_error) metrics::add(metrics::Counter::ToolErrors);
    metrics::observe(metrics::Distribution::ToolResultBytes, r.content.size());
    return r;
}

}  // namespace ovicode
