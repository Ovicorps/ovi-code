#include "ovicode/session.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "ovicode/utf8.hpp"

namespace ovicode {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr std::size_t kMaxSessionBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxDraftBytes = 1024 * 1024;

std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct FileDescriptor {
    int value;
    explicit FileDescriptor(int fd) : value(fd) {}
    ~FileDescriptor() { if (value >= 0) ::close(value); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
};

[[noreturn]] void ioError(const fs::path& path) {
    throw std::system_error(errno, std::generic_category(), path.string());
}

fs::path workspacePath(const fs::path& workspace) {
    return fs::weakly_canonical(fs::absolute(workspace));
}

fs::path workspaceDirectory(const fs::path& directory,
                            const fs::path& workspace) {
    // Stable across launches and standard-library implementations.
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : workspacePath(workspace).string()) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream key;
    key << std::hex << std::setw(16) << std::setfill('0') << hash;
    return directory / key.str();
}

bool validId(const std::string& id) {
    return !id.empty() && id.size() <= 100 &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
           });
}

void privateDirectory(const fs::path& path) {
    fs::create_directories(path);
    if (fs::is_symlink(fs::symlink_status(path)))
        throw std::runtime_error("Refusing symlink session directory: " +
                                 path.string());
    if (::chmod(path.c_str(), 0700) != 0) ioError(path);
}

json readJson(const fs::path& path, std::size_t limit) {
    FileDescriptor file(::open(path.c_str(),
                               O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (file.value < 0) ioError(path);
    struct stat status {};
    if (::fstat(file.value, &status) != 0) ioError(path);
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > limit)
        throw std::runtime_error("Invalid or oversized session file: " +
                                 path.string());
    std::string contents(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = ::read(file.value, contents.data() + offset,
                                     contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) ioError(path);
        if (count == 0) throw std::runtime_error("Incomplete session: " + path.string());
        offset += static_cast<std::size_t>(count);
    }
    try {
        return json::parse(contents);
    } catch (const json::exception& error) {
        throw std::runtime_error("Invalid JSON in " + path.string() + ": " + error.what());
    }
}

void atomicWrite(const fs::path& path, const json& document,
                 std::size_t limit) {
    const std::string contents = document.dump(-1, ' ', false,
                                               json::error_handler_t::replace);
    if (contents.size() > limit)
        throw std::runtime_error("Session save exceeds the size limit: " +
                                 path.string());
    std::string temporary = path.string() + ".tmp.XXXXXX";
    FileDescriptor file(::mkstemp(temporary.data()));
    if (file.value < 0) ioError(path);
    try {
        if (::fchmod(file.value, 0600) != 0) ioError(path);
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const ssize_t count = ::write(file.value, contents.data() + offset,
                                          contents.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) ioError(path);
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(file.value) != 0) ioError(path);
        if (::rename(temporary.c_str(), path.c_str()) != 0) ioError(path);
        FileDescriptor directory(::open(path.parent_path().c_str(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (directory.value < 0) ioError(path.parent_path());
        if (::fsync(directory.value) != 0) ioError(path.parent_path());
    } catch (...) {
        ::unlink(temporary.c_str());
        throw;
    }
}

const std::vector<std::string> roles{"system", "user", "assistant", "tool"};
const std::vector<std::string> kinds{
    "user", "assistant", "tool_call", "tool_result", "system", "error"};

int enumIndex(const std::vector<std::string>& values, const json& value) {
    const std::string name = value.get<std::string>();
    const auto found = std::find(values.begin(), values.end(), name);
    if (found == values.end()) throw std::runtime_error("Invalid session enum: " + name);
    return static_cast<int>(found - values.begin());
}

json encodeMessage(const Message& message) {
    json calls = json::array();
    for (const auto& call : message.tool_calls)
        calls.push_back({{"id", call.id}, {"name", call.name}, {"input", call.input}});
    return {{"role", roles.at(static_cast<std::size_t>(message.role))},
            {"text", message.text}, {"tool_calls", calls},
            {"reasoning_details", message.reasoning_details},
            {"tool_call_id", message.tool_call_id},
            {"tool_name", message.tool_name}, {"is_error", message.is_error}};
}

SessionState decodeSession(const json& document, const fs::path& workspace,
                           const std::string& id) {
    if (document.at("version").get<int>() != 1 ||
        document.at("id").get<std::string>() != id ||
        document.at("workspace").get<std::string>() != workspace.string())
        throw std::runtime_error("Session version, ID, or workspace does not match.");
    SessionState state;
    state.id = id;
    state.workspace = workspace;
    state.provider = document.at("provider").get<std::string>();
    state.model = document.at("model").get<std::string>();
    state.pending_prompt = document.at("pending_prompt").get<std::string>();
    state.turn_in_progress = document.at("turn_in_progress").get<bool>();
    const auto& messages = document.at("messages");
    const auto& transcript = document.at("transcript");
    if (!messages.is_array() || !transcript.is_array())
        throw std::runtime_error("Invalid session history.");
    for (const auto& item : messages) {
        Message message;
        message.role = static_cast<Message::Role>(enumIndex(roles, item.at("role")));
        message.text = item.at("text").get<std::string>();
        message.reasoning_details = item.at("reasoning_details");
        message.tool_call_id = item.at("tool_call_id").get<std::string>();
        message.tool_name = item.at("tool_name").get<std::string>();
        message.is_error = item.at("is_error").get<bool>();
        if (!item.at("tool_calls").is_array())
            throw std::runtime_error("Invalid session tool calls.");
        for (const auto& call : item.at("tool_calls"))
            message.tool_calls.push_back({call.at("id").get<std::string>(),
                                         call.at("name").get<std::string>(),
                                         call.at("input")});
        state.messages.push_back(std::move(message));
    }
    for (const auto& item : transcript)
        state.transcript.push_back({
            static_cast<TranscriptEntry::Kind>(enumIndex(kinds, item.at("kind"))),
            item.at("title").get<std::string>(), item.at("body").get<std::string>()});
    return state;
}

// A crash can leave an assistant's tool calls without all their results. Keep
// completed results and close only the missing ones, without executing tools.
void closeInterruptedToolCalls(std::vector<Message>& messages) {
    std::vector<Message> repaired;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        repaired.push_back(messages[i]);
        if (messages[i].role != Message::Role::Assistant ||
            messages[i].tool_calls.empty()) continue;
        std::unordered_set<std::string> completed;
        std::size_t end = i + 1;
        while (end < messages.size() && messages[end].role == Message::Role::Tool) {
            completed.insert(messages[end].tool_call_id);
            repaired.push_back(messages[end++]);
        }
        for (const auto& call : messages[i].tool_calls) {
            if (completed.contains(call.id)) continue;
            Message result;
            result.role = Message::Role::Tool;
            result.tool_call_id = call.id;
            result.tool_name = call.name;
            result.is_error = true;
            result.text = "Session interrupted before this tool result was saved. "
                          "The tool may already have changed files. Inspect the "
                          "workspace before retrying; it was not automatically rerun.";
            repaired.push_back(std::move(result));
        }
        i = end - 1;
    }
    messages = std::move(repaired);
}

fs::path draftPath(const fs::path& path) {
    return path.parent_path() / (path.stem().string() + ".draft.json");
}

}  // namespace

std::vector<SessionInfo> SessionStore::list(const fs::path& directory,
                                          const fs::path& workspace) {
    std::vector<SessionInfo> sessions;
    const fs::path root = workspacePath(workspace);
    const fs::path bucket = workspaceDirectory(directory, root);
    if (!fs::exists(bucket)) return sessions;
    for (const auto& entry : fs::directory_iterator(bucket)) {
        const std::string id = entry.path().stem().string();
        if (entry.path().extension() != ".json" || !validId(id)) continue;
        try {
            const auto document = readJson(entry.path(), kMaxSessionBytes);
            const auto state = decodeSession(document, root, id);
            sessions.push_back({id, document.at("title").get<std::string>(),
                                state.model, document.at("updated_at").get<std::int64_t>()});
        } catch (const std::exception&) {
            // Keep damaged checkpoints in the list: a damaged newest session
            // must fail explicitly, but old damage must not block newer work.
            const auto age = fs::last_write_time(entry.path()) -
                             fs::file_time_type::clock::now();
            const auto modified = std::chrono::system_clock::now().time_since_epoch() + age;
            sessions.push_back({id, "[unreadable session]", "",
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(modified).count()});
        }
    }
    std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
        if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
        return a.id > b.id;
    });
    return sessions;
}

SessionStore::SessionStore(const fs::path& directory, const fs::path& workspace,
                           const Config& cfg, bool start_new,
                           const std::string& resume_id) {
    if (start_new && !resume_id.empty())
        throw std::runtime_error("Use either --new-session or --resume, not both.");
    const fs::path root = workspacePath(workspace);
    const fs::path bucket = workspaceDirectory(directory, root);
    privateDirectory(directory);
    privateDirectory(bucket);
    std::string id = resume_id;
    if (id.empty() && !start_new) {
        const auto sessions = list(directory, root);
        if (!sessions.empty()) id = sessions.front().id;
    }
    const bool restoring = !id.empty();
    if (restoring && !validId(id)) throw std::runtime_error("Invalid session ID.");
    if (!restoring) {
        const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
        id = std::to_string(ticks) + "-" + std::to_string(::getpid());
    }
    path_ = bucket / (id + ".json");
    const fs::path lock_path = bucket / (id + ".lock");
    FileDescriptor lock(::open(lock_path.c_str(),
                               O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (lock.value < 0) ioError(lock_path);
    struct stat lock_status {};
    if (::fstat(lock.value, &lock_status) != 0) ioError(lock_path);
    if (!S_ISREG(lock_status.st_mode))
        throw std::runtime_error("Invalid session lock file: " + lock_path.string());
    if (::fchmod(lock.value, 0600) != 0) ioError(lock_path);
    if (::flock(lock.value, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("Session is open in another process. Use --new-session.");
    if (restoring) {
        state_ = decodeSession(readJson(path_, kMaxSessionBytes), root, id);
        bool recover_pending = true;
        if (fs::exists(draftPath(path_))) {
            const auto draft = readJson(draftPath(path_), kMaxDraftBytes);
            state_.draft = draft.at("draft").get<std::string>();
            recover_pending = draft.at("recover_pending").get<bool>();
        }
        if (recover_pending && state_.draft.empty() && !state_.pending_prompt.empty())
            state_.draft = state_.pending_prompt;
        closeInterruptedToolCalls(state_.messages);
    } else {
        state_.id = id;
        state_.workspace = root;
        state_.provider = providerToString(cfg.provider);
        state_.model = selectedModel(cfg);
        saveLocked();
    }
    lock_fd_ = lock.value;
    lock.value = -1;
}

SessionStore::~SessionStore() {
    if (lock_fd_ >= 0) ::close(lock_fd_);
}

SessionState SessionStore::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void SessionStore::setTranscriptLocked(const TranscriptSnapshot& transcript) {
    // UI clears and worker checkpoints can race; never restore a stale view.
    if (transcript.generation < transcript_generation_) return;
    transcript_generation_ = transcript.generation;
    state_.transcript.clear();
    for (const auto& entry : transcript.entries)
        state_.transcript.push_back(entry->entry);
}

void SessionStore::saveLocked() {
    json messages = json::array();
    json transcript = json::array();
    std::string title = "New session";
    for (const auto& message : state_.messages) messages.push_back(encodeMessage(message));
    for (const auto& entry : state_.transcript) {
        transcript.push_back({{"kind", kinds.at(static_cast<std::size_t>(entry.kind))},
                              {"title", entry.title}, {"body", entry.body}});
        if (title == "New session" && entry.kind == TranscriptEntry::Kind::User)
            title = utf8::truncateColumns(entry.body.substr(0, entry.body.find('\n')), 60);
    }
    if (title == "New session" && !state_.pending_prompt.empty())
        title = utf8::truncateColumns(state_.pending_prompt.substr(
            0, state_.pending_prompt.find('\n')), 60);
    atomicWrite(path_, {{"version", 1}, {"id", state_.id},
                        {"workspace", state_.workspace.string()},
                        {"provider", state_.provider}, {"model", state_.model},
                        {"updated_at", nowMillis()}, {"title", title},
                        {"pending_prompt", state_.pending_prompt},
                        {"turn_in_progress", state_.turn_in_progress},
                        {"messages", messages}, {"transcript", transcript}},
                kMaxSessionBytes);
}

void SessionStore::saveDraft(const std::string& draft) {
    std::lock_guard lock(mutex_);
    if (draft == state_.draft) return;
    atomicWrite(draftPath(path_), {{"draft", draft}, {"recover_pending", false}},
                kMaxDraftBytes);
    state_.draft = draft;
}

void SessionStore::beginTurn(const std::string& prompt, const Config& cfg,
                             const TranscriptSnapshot& transcript) {
    std::lock_guard lock(mutex_);
    state_.provider = providerToString(cfg.provider);
    state_.model = selectedModel(cfg);
    state_.pending_prompt = prompt;
    state_.turn_in_progress = true;
    setTranscriptLocked(transcript);
    saveLocked();  // Save the submitted input before any network/tool work.
    atomicWrite(draftPath(path_), {{"draft", ""}, {"recover_pending", true}},
                kMaxDraftBytes);
    state_.draft.clear();
}

void SessionStore::checkpoint(const std::vector<Message>& messages,
                              const TranscriptSnapshot& transcript) {
    std::lock_guard lock(mutex_);
    state_.messages = messages;
    setTranscriptLocked(transcript);
    saveLocked();
}

void SessionStore::finishTurn(const std::vector<Message>& messages,
                              const TranscriptSnapshot& transcript,
                              bool completed, bool cancelled) {
    std::lock_guard lock(mutex_);
    // runTurn rolls back on cancellation. Preserve the last checkpoint instead
    // so recovery retains assistant/tool output that the user already saw.
    if (!cancelled) state_.messages = messages;
    if (!completed) closeInterruptedToolCalls(state_.messages);
    state_.turn_in_progress = false;
    if (completed) state_.pending_prompt.clear();
    setTranscriptLocked(transcript);
    saveLocked();
}

void SessionStore::saveTranscript(const TranscriptSnapshot& transcript) {
    std::lock_guard lock(mutex_);
    setTranscriptLocked(transcript);
    saveLocked();
}

void SessionStore::resetConversation(const Config& cfg,
                                     const TranscriptSnapshot& transcript) {
    std::lock_guard lock(mutex_);
    state_.messages.clear();
    state_.provider = providerToString(cfg.provider);
    state_.model = selectedModel(cfg);
    state_.pending_prompt.clear();
    state_.turn_in_progress = false;
    setTranscriptLocked(transcript);
    saveLocked();
}

}  // namespace ovicode
