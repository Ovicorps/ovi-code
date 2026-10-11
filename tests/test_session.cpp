#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "ovicode/react.hpp"
#include "ovicode/session.hpp"

namespace {

namespace fs = std::filesystem;
using namespace ovicode;
using json = nlohmann::json;
int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

struct TempDir {
    fs::path path = fs::temp_directory_path() /
        ("ovi-code-session-test-" + std::to_string(::getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() { std::error_code error; fs::remove_all(path, error); }
};

template <typename Action>
bool throws(Action action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}

json readDocument(const fs::path& path) {
    std::ifstream file(path);
    return json::parse(file);
}

Config testConfig() {
    Config cfg;
    cfg.provider = Provider::OpenAI;
    cfg.openai.model = "test-model";
    cfg.openai.api_key = "SECRET-KEY-NOT-FOR-SESSION";
    return cfg;
}

Message message(Message::Role role, const std::string& text) {
    Message result;
    result.role = role;
    result.text = text;
    return result;
}

void testRoundTripAndContinuation() {
    TempDir temp;
    const auto directory = temp.path / "sessions";
    const auto cfg = testConfig();
    std::string id;
    fs::path checkpoint_path;
    {
        SessionStore session(directory, temp.path, cfg);
        id = session.state().id;
        checkpoint_path = session.path();
        TranscriptStore transcript;
        std::vector<Message> messages;
        AgentContext ctx{cfg, {}, temp.path};
        int requests = 0;
        ctx.chat_handler = [&](const Config&, const ChatRequest& request, std::stop_token) {
            ChatResponse response;
            if (++requests == 1) {
                CHECK(request.messages.back().text == "List project files");
                response.text = "Inspecting the project.";
                response.tool_calls = {{"call-1", "list_files", {{"path", "."}}}};
                response.reasoning_details = json::array({{{"opaque", "provider-state"}}});
            } else {
                CHECK(request.messages.back().role == Message::Role::Tool);
                response.text = "The project is ready.\nNext: add tests. ✓";
            }
            return response;
        };
        session.saveDraft("List project files");
        session.beginTurn("List project files", cfg, transcript.snapshot());
        const auto outcome = runTurn(ctx, messages, "List project files",
            [&](const TranscriptEntry& entry) {
                transcript.push(entry);
                session.checkpoint(messages, transcript.snapshot());
                return true;
            });
        CHECK(outcome == TurnOutcome::Completed);
        session.finishTurn(messages, transcript.snapshot(), true, false);
        session.saveDraft("Now add tests\nwithout changing the API ✓");
        CHECK(requests == 2);
        CHECK(session.state().messages.size() == 4);
        CHECK(!session.state().turn_in_progress);
        CHECK(session.state().pending_prompt.empty());
        const auto document = readDocument(session.path());
        CHECK(document.dump().find(cfg.openai.api_key) == std::string::npos);
        struct stat status {};
        CHECK(::stat(session.path().c_str(), &status) == 0);
        CHECK((status.st_mode & 0777) == 0600);
        CHECK(::stat(directory.c_str(), &status) == 0);
        CHECK((status.st_mode & 0777) == 0700);
        CHECK(::stat(session.path().parent_path().c_str(), &status) == 0);
        CHECK((status.st_mode & 0777) == 0700);
    }
    {
        SessionStore resumed(directory, temp.path / ".", cfg);
        const auto saved = resumed.state();
        CHECK(saved.id == id);
        CHECK(saved.draft == "Now add tests\nwithout changing the API ✓");
        CHECK(saved.transcript.size() == 5);
        CHECK(saved.transcript.front().kind == TranscriptEntry::Kind::User);
        CHECK(saved.messages[1].tool_calls[0].input == json({{"path", "."}}));
        CHECK(saved.messages[1].reasoning_details[0]["opaque"] == "provider-state");
        CHECK(saved.messages[2].tool_call_id == "call-1");
        CHECK(saved.messages[2].tool_name == "list_files");
        auto messages = saved.messages;
        AgentContext ctx{cfg, {}, temp.path};
        ctx.chat_handler = [&](const Config&, const ChatRequest& request, std::stop_token) {
            CHECK(request.messages.size() == 5);
            CHECK(request.messages[3].text.find("Next: add tests") != std::string::npos);
            ChatResponse response;
            response.text = "Continuing with the saved context.";
            return response;
        };
        CHECK(runTurn(ctx, messages, "Continue", {}) == TurnOutcome::Completed);
        // Draft autosave is small and does not rewrite the checkpoint.
        const auto before = fs::last_write_time(checkpoint_path);
        resumed.saveDraft("A new draft");
        CHECK(fs::last_write_time(checkpoint_path) == before);
    }
}

void testSessionSelectionAndIsolation() {
    TempDir temp;
    const auto directory = temp.path / "sessions";
    const auto cfg = testConfig();
    CHECK(SessionStore::list(directory, temp.path).empty());
    std::string first;
    std::string second;
    {
        SessionStore session(directory, temp.path, cfg);
        first = session.state().id;
        session.saveDraft("First draft");
    }
    {
        SessionStore session(directory, temp.path, cfg, true);
        second = session.state().id;
        CHECK(first != second);
        CHECK(session.state().draft.empty());
    }
    {
        SessionStore session(directory, temp.path, cfg);
        CHECK(session.state().id == second);
        CHECK(throws([&] { SessionStore competing(directory, temp.path, cfg); }));
    }
    {
        SessionStore session(directory, temp.path, cfg, false, first);
        CHECK(session.state().draft == "First draft");
        CHECK(throws([&] { SessionStore competing(directory, temp.path, cfg, false, first); }));
        SessionStore parallel(directory, temp.path, cfg, true);
        CHECK(parallel.state().id != first);
    }
    const auto other = temp.path / "other-project";
    fs::create_directories(other);
    CHECK(SessionStore::list(directory, other).empty());
    CHECK(throws([&] { SessionStore wrong_workspace(directory, other, cfg, false, first); }));
    CHECK(throws([&] { SessionStore invalid(directory, temp.path, cfg, false, "../escape"); }));
    CHECK(throws([&] { SessionStore invalid(directory, temp.path, cfg, true, first); }));
    CHECK(SessionStore::list(directory, temp.path).size() == 3);
    const auto alias = temp.path / "workspace-alias";
    fs::create_directory_symlink(other, alias);
    SessionStore isolated(directory, other, cfg);
    CHECK(SessionStore::list(directory, alias).front().id == isolated.state().id);
}

void testAbruptExitAndToolRecovery() {
    TempDir temp;
    const auto directory = temp.path / "sessions";
    const auto cfg = testConfig();
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            SessionStore session(directory, temp.path, cfg);
            TranscriptStore transcript;
            session.beginTurn("Make two edits", cfg, transcript.snapshot());
            Message assistant = message(Message::Role::Assistant, "Making edits");
            assistant.tool_calls = {
                {"done", "edit_file", {{"path", "first.txt"}}},
                {"missing", "edit_file", {{"path", "second.txt"}}}};
            Message result = message(Message::Role::Tool, "First edit saved");
            result.tool_call_id = "done";
            result.tool_name = "edit_file";
            transcript.push({TranscriptEntry::Kind::User, "you", "Make two edits"});
            transcript.push({TranscriptEntry::Kind::Assistant, "assistant", "Making edits"});
            session.checkpoint({message(Message::Role::User, "Make two edits"),
                                assistant, result}, transcript.snapshot());
            // No destructor or shutdown handler: simulate terminal/process loss.
            ::_exit(0);
        } catch (...) { ::_exit(1); }
    }
    if (child < 0) return;
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    {
        SessionStore session(directory, temp.path, cfg);
        const auto saved = session.state();
        CHECK(saved.turn_in_progress);
        CHECK(saved.draft == "Make two edits");
        CHECK(saved.messages.size() == 4);
        CHECK(saved.messages[2].text == "First edit saved");
        CHECK(!saved.messages[2].is_error);
        CHECK(saved.messages[3].tool_call_id == "missing");
        CHECK(saved.messages[3].is_error);
        CHECK(saved.messages[3].text.find("not automatically rerun") != std::string::npos);
        CHECK(!fs::exists(temp.path / "second.txt"));
        // Intentionally clearing a recovered prompt stays cleared next launch.
        session.saveDraft("");
    }
    {
        SessionStore session(directory, temp.path, cfg);
        CHECK(session.state().draft.empty());
        session.saveDraft("A newer unfinished prompt");
    }
    {
        SessionStore session(directory, temp.path, cfg);
        CHECK(session.state().draft == "A newer unfinished prompt");
    }
}

void testCancellationAndTranscriptClear() {
    TempDir temp;
    const auto directory = temp.path / "sessions";
    const auto cfg = testConfig();
    {
        SessionStore session(directory, temp.path, cfg);
        TranscriptStore transcript;
        session.beginTurn("Continue task", cfg, transcript.snapshot());
        transcript.push({TranscriptEntry::Kind::Assistant, "assistant", "Partial output"});
        session.checkpoint({message(Message::Role::User, "Continue task"),
                            message(Message::Role::Assistant, "Partial output")},
                           transcript.snapshot());
        const auto stale = transcript.snapshot();
        transcript.clear();
        session.saveTranscript(transcript.snapshot());
        session.finishTurn({}, stale, false, true);
        CHECK(session.state().messages.size() == 2);
        CHECK(session.state().transcript.empty());
    }
    {
        SessionStore session(directory, temp.path, cfg);
        CHECK(session.state().draft == "Continue task");
        CHECK(session.state().transcript.empty());
        CHECK(session.state().messages.back().text == "Partial output");
        Config different = cfg;
        different.openai.model = "different-model";
        TranscriptStore transcript;
        session.resetConversation(different, transcript.snapshot());
        CHECK(session.state().messages.empty());
        CHECK(session.state().model == "different-model");
        CHECK(session.state().pending_prompt.empty());
    }
}

void testDamagedFilesAndFailedSaves() {
    TempDir temp;
    const auto directory = temp.path / "sessions";
    const auto cfg = testConfig();
    fs::path path;
    std::string id;
    {
        SessionStore session(directory, temp.path, cfg);
        path = session.path();
        id = session.state().id;
        session.saveDraft("Good draft");
        CHECK(throws([&] { session.saveDraft(std::string(1024 * 1024, 'x')); }));
        CHECK(session.state().draft == "Good draft");
    }
    {
        std::ofstream file(path);
        file << "{broken";
    }
    CHECK(throws([&] { SessionStore damaged(directory, temp.path, cfg); }));
    CHECK(throws([&] { SessionStore damaged(directory, temp.path, cfg, false, id); }));
    CHECK(fs::file_size(path) == 7);
    std::string fresh_id;
    {
        // An explicit fresh session does not delete or overwrite the damaged one.
        SessionStore fresh(directory, temp.path, cfg, true);
        fresh_id = fresh.state().id;
        CHECK(fresh.state().id != id);
        CHECK(fs::file_size(path) == 7);
        const fs::path moved = temp.path / "moved-session-directory";
        fs::rename(fresh.path().parent_path(), moved);
        CHECK(throws([&] { fresh.saveDraft("Cannot save here"); }));
        CHECK(fs::exists(moved / fresh.path().filename()));
        const auto previous = readDocument(moved / fresh.path().filename());
        TranscriptStore transcript;
        CHECK(throws([&] { fresh.beginTurn("Cannot checkpoint here", cfg,
                                          transcript.snapshot()); }));
        CHECK(readDocument(moved / fresh.path().filename()) == previous);
        fs::rename(moved, fresh.path().parent_path());
    }
    {
        SessionStore latest(directory, temp.path, cfg);
        CHECK(latest.state().id == fresh_id);
        const auto sessions = SessionStore::list(directory, temp.path);
        CHECK(sessions.size() == 2);
        CHECK(sessions.back().title == "[unreadable session]");
    }
    fs::remove(path);
    const fs::path outside = temp.path / "outside.json";
    { std::ofstream file(outside); file << "Do not overwrite"; }
    fs::create_symlink(outside, path);
    CHECK(throws([&] { SessionStore symlink(directory, temp.path, cfg, false, id); }));
    CHECK(fs::file_size(outside) == 16);
}

void testConcurrentDraftAndOutputSaves() {
    TempDir temp;
    const auto directory = temp.path / "sessions";
    const auto cfg = testConfig();
    {
        SessionStore session(directory, temp.path, cfg);
        TranscriptStore transcript;
        session.beginTurn("Original request", cfg, transcript.snapshot());
        std::jthread writer([&] {
            for (int i = 0; i < 8; ++i) {
                transcript.push({TranscriptEntry::Kind::Assistant, "assistant",
                                 "Output " + std::to_string(i)});
                session.checkpoint({message(Message::Role::User, "Original request"),
                                    message(Message::Role::Assistant, "Output " + std::to_string(i))},
                                   transcript.snapshot());
            }
        });
        for (int i = 0; i < 8; ++i) session.saveDraft("Draft " + std::to_string(i));
        writer.join();
    }
    SessionStore resumed(directory, temp.path, cfg);
    CHECK(resumed.state().draft == "Draft 7");
    CHECK(resumed.state().messages.back().text == "Output 7");
    CHECK(resumed.state().transcript.size() == 8);
}

}  // namespace

int main() {
    try {
        testRoundTripAndContinuation();
        testSessionSelectionAndIsolation();
        testAbruptExitAndToolRecovery();
        testCancellationAndTranscriptClear();
        testDamagedFilesAndFailedSaves();
        testConcurrentDraftAndOutputSaves();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "Session tests passed.\n";
    return 0;
}
