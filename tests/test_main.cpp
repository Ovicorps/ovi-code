#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "ovicode/context.hpp"
#include "ovicode/config.hpp"
#include "ovicode/http.hpp"
#include "ovicode/metrics.hpp"
#include "ovicode/plugins.hpp"
#include "ovicode/providers.hpp"
#include "ovicode/react.hpp"
#include "ovicode/tools.hpp"
#include "ovicode/transcript.hpp"
#include "ovicode/utf8.hpp"

namespace {

namespace fs = std::filesystem;
using namespace ovicode;

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
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("ovi-code-test-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void writeFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << contents;
}

struct EnvironmentGuard {
    std::string name;
    std::optional<std::string> original;

    explicit EnvironmentGuard(std::string variable) : name(std::move(variable)) {
        if (const char* value = std::getenv(name.c_str())) original = value;
        unsetenv(name.c_str());
    }

    ~EnvironmentGuard() {
        if (original) {
            setenv(name.c_str(), original->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

void testUtf8() {
    const std::string text =
        std::string("A") + "\xC3\xA9" + "\xE6\xB5\x8B" +
        "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";
    std::size_t offset = 0;
    std::vector<std::size_t> boundaries{0};
    while (offset < text.size()) {
        offset = utf8::nextBoundary(text, offset);
        boundaries.push_back(offset);
    }
    CHECK(boundaries.size() == 5);
    CHECK(utf8::previousBoundary(text, text.size()) == boundaries[3]);
    CHECK(utf8::clampBoundary(text, 2) == 1);
    CHECK(utf8::displayWidth(text) == 6);
    CHECK(utf8::truncateBytes(text, 3) == text.substr(0, 3));
    CHECK(utf8::displayWidth(utf8::truncateColumns(text, 4)) <= 4);
    CHECK(utf8::isValid(text));
    CHECK(utf8::isValid("\xEF\xBF\xBD"));
    CHECK(!utf8::isValid("\xC3"));
}

void testTranscript() {
    TranscriptLimits limits;
    limits.max_entries = 3;
    limits.keep_entries = 2;
    limits.max_bytes = 1024;
    limits.keep_bytes = 900;
    limits.max_lines = 100;
    limits.keep_lines = 90;
    limits.max_entry_bytes = 64;
    limits.max_entry_lines = 4;
    TranscriptStore store(limits);
    for (int i = 0; i < 4; ++i) {
        store.push({TranscriptEntry::Kind::Assistant, "entry",
                    "body " + std::to_string(i)});
    }
    const auto snapshot = store.snapshot();
    CHECK(snapshot.dropped_entries == 2);
    CHECK(snapshot.entries.size() == 3);
    CHECK(snapshot.entries.front()->entry.title == "elided");
    CHECK(snapshot.entries.back()->entry.body == "body 3");
    CHECK(!store.snapshotIfChanged(snapshot.generation).has_value());

    TranscriptView view;
    view.update(snapshot, 20);
    CHECK(view.totalRows() > 0);
    CHECK(view.visibleRows(0, 2, 0).size() <= 2);
    TranscriptStore long_line_store;
    long_line_store.push({TranscriptEntry::Kind::Assistant, "long",
                          std::string(128 * 1024, 'x')});
    TranscriptView long_line_view;
    long_line_view.update(long_line_store.snapshot(), 8);
    CHECK(long_line_view.totalRows() <= 2048);
    store.clear();
    CHECK(store.snapshot().entries.empty());
}

Message message(Message::Role role, std::string text) {
    Message result;
    result.role = role;
    result.text = std::move(text);
    return result;
}

void testContext() {
    std::vector<Message> messages{
        message(Message::Role::User, "old user"),
        message(Message::Role::Assistant, "old answer"),
        message(Message::Role::User, "current user"),
        message(Message::Role::Assistant, "current answer"),
    };
    const std::vector<Message> current(messages.begin() + 2, messages.end());
    const auto result =
        compactConversation(messages, estimateConversationBytes(current));
    CHECK(result.removed_messages == 2);
    CHECK(!result.exceeds_budget);
    CHECK(messages.size() == 2);
    CHECK(messages.front().role == Message::Role::User);
    CHECK(messages.front().text == "current user");
}

void testProviderSerialization() {
    Config config;
    config.provider = Provider::OpenAI;
    config.openai.model = "gpt-test";
    std::vector<Message> messages{message(Message::Role::User, "hello")};
    const auto& tools = builtinTools();
    const ChatRequest request{"system prompt", messages, tools};
    const auto parsed =
        nlohmann::json::parse(serializeChatRequestBody(config, request));
    CHECK(parsed["model"] == "gpt-test");
    CHECK(parsed["stream"] == false);
    CHECK(parsed["messages"].size() == 2);
    CHECK(parsed["tools"].size() == 3);

    config.provider = Provider::Anthropic;
    config.anthropic.model = "claude-test";
    const auto anthropic =
        nlohmann::json::parse(serializeChatRequestBody(config, request));
    CHECK(anthropic["system"] == "system prompt");
    CHECK(anthropic["messages"][0]["role"] == "user");
}

void testTools() {
    TempDir workspace;
    writeFile(workspace.path / "file.txt", "one\ntwo\nthree\nfour\n");
    writeFile(workspace.path / "build" / "ignored.txt", "ignored");

    ToolCall read{"read", "read_file",
                  {{"path", "file.txt"}, {"start_line", 2}, {"max_lines", 2}}};
    const auto read_result = executeTool(workspace.path, read);
    CHECK(!read_result.is_error);
    CHECK(read_result.content.starts_with("two\nthree\n"));
    CHECK(read_result.content.find("start_line=4") != std::string::npos);
    read.input["max_lines"] = 1;
    read.input["start_line"] = 1;
    const auto one_line = executeTool(workspace.path, read);
    CHECK(one_line.content.starts_with("one\n"));
    CHECK(one_line.content.find("two") == std::string::npos);
    CHECK(one_line.content.find("start_line=2") != std::string::npos);

    ToolCall list{"list", "list_files", {{"path", "."}, {"limit", 10}}};
    const auto list_result = executeTool(workspace.path, list);
    CHECK(!list_result.is_error);
    CHECK(list_result.content.find("file.txt") != std::string::npos);
    CHECK(list_result.content.find("ignored.txt") == std::string::npos);
    list.input["path"] = "file.txt";
    const auto listed_file = executeTool(workspace.path, list);
    CHECK(!listed_file.is_error);
    CHECK(listed_file.content == "file.txt\n");

    fs::path sibling = workspace.path;
    sibling += "-sibling";
    fs::create_directories(sibling);
    writeFile(sibling / "outside.txt", "outside");
    ToolCall escape{"escape", "read_file",
                    {{"path", "../" + sibling.filename().string() +
                                  "/outside.txt"}}};
    CHECK(executeTool(workspace.path, escape).is_error);
    writeFile(workspace.path / "invalid.bin", std::string("bad\xC3", 4));
    ToolCall invalid{"invalid", "read_file", {{"path", "invalid.bin"}}};
    CHECK(executeTool(workspace.path, invalid).is_error);
    writeFile(workspace.path / "long.txt", std::string(160 * 1024, 'x'));
    ToolCall long_read{"long", "read_file", {{"path", "long.txt"}}};
    const auto first_fragment = executeTool(workspace.path, long_read);
    CHECK(!first_fragment.is_error);
    const std::string byte_marker = "start_byte=";
    const auto marker = first_fragment.content.find(byte_marker);
    CHECK(marker != std::string::npos);
    if (marker != std::string::npos) {
        const auto marker_end = first_fragment.content.find(']', marker);
        const auto next_byte = std::stoull(first_fragment.content.substr(
            marker + byte_marker.size(), marker_end - marker - byte_marker.size()));
        CHECK(next_byte > 0);
        long_read.input["start_byte"] = next_byte;
        const auto second_fragment = executeTool(workspace.path, long_read);
        CHECK(!second_fragment.is_error);
        CHECK(second_fragment.content != first_fragment.content);
    }

    TempDir external;
    writeFile(external.path / "secret.txt", "outside-secret-keyword");
    std::error_code symlink_error;
    fs::create_symlink(external.path / "secret.txt",
                       workspace.path / "external-link.txt", symlink_error);
    if (!symlink_error) {
        ToolCall symlink{"symlink", "read_file",
                         {{"path", "external-link.txt"}}};
        CHECK(executeTool(workspace.path, symlink).is_error);
    }
    std::error_code error;
    fs::remove_all(sibling, error);

    for (const auto& definition : builtinTools()) {
        if (definition.name == "read_file") {
            CHECK(definition.input_schema["properties"].contains("start_line"));
        }
        if (definition.name == "list_files") {
            CHECK(definition.input_schema["properties"].contains("offset"));
        }
    }
}

void testConfigBounds() {
    TempDir directory;
    const fs::path path = directory.path / "config.json";
    writeFile(path,
              R"({"provider":"openrouter","max_context_bytes":0,"max_request_bytes":18446744073709551615})");
    const auto config = loadConfig(path);
    CHECK(config.has_value());
    CHECK(config->max_context_bytes == 64 * 1024);
    CHECK(config->max_request_bytes == 128 * 1024 * 1024);
}

void testConfigSynchronization() {
    TempDir directory;
    const fs::path path = directory.path / "custom" / "settings.json";
    Config live;
    for (const auto& info : knownProviders()) {
        providerAccount(live, info.provider).api_key = info.id + "-original";
    }
    live.plugin_parameters["review"]["depth"] = "brief";
    CHECK(saveConfig(live, path));
    ConfigFileSync sync(path, live);
    CHECK(sync.path() == path);

    Config disk = live;
    for (const auto& info : knownProviders()) {
        providerAccount(disk, info.provider).api_key = info.id + "-rotated";
    }
    const auto original_time = fs::last_write_time(path);
    CHECK(saveConfig(disk, path));
    fs::last_write_time(path, original_time);
    CHECK(sync.reload(live));
    CHECK(live == disk);

    // An unchanged key in an already-open Settings form follows file edits.
    Config staged = live;
    staged.theme = Theme::Dark;
    staged.openrouter.model = "custom/model";
    staged.plugin_parameters["review"]["depth"] = "thorough";
    disk.openrouter.api_key = "file-new-key";
    disk.openai.api_key = "other-account-new-key";
    disk.max_tokens = 12345;
    disk.plugin_parameters["review"]["notes"] = "added outside Settings";
    CHECK(saveConfig(disk, path));
    CHECK(sync.save(staged));
    CHECK(staged.openrouter.api_key == "file-new-key");
    CHECK(staged.openai.api_key == "other-account-new-key");
    CHECK(staged.openrouter.model == "custom/model");
    CHECK(staged.theme == Theme::Dark);
    CHECK(staged.max_tokens == 12345);
    CHECK(staged.plugin_parameters["review"]["depth"] == "thorough");
    CHECK(staged.plugin_parameters["review"]["notes"] ==
          "added outside Settings");
    CHECK(loadConfig(path) == staged);
    live = staged;

    // An explicitly edited Settings key wins when Save is pressed later.
    staged = live;
    staged.openrouter.api_key = "settings-new-key";
    disk = live;
    disk.openrouter.api_key = "concurrent-file-key";
    disk.anthropic.api_key = "concurrent-other-account-key";
    CHECK(saveConfig(disk, path));
    CHECK(sync.save(staged));
    CHECK(staged.openrouter.api_key == "settings-new-key");
    CHECK(staged.anthropic.api_key == "concurrent-other-account-key");
    CHECK(loadConfig(path) == staged);
    live = staged;

    staged = live;
    staged.openrouter.api_key.clear();
    CHECK(sync.save(staged));
    CHECK(loadConfig(path)->openrouter.api_key.empty());
    live = staged;

    // Saving another setting on exit must not resurrect an old key.
    live.technique_id = "review-plugin";
    disk = staged;
    disk.openrouter.api_key = "last-file-key";
    disk.technique = Technique::Rag;
    disk.technique_id = "rag";
    CHECK(saveConfig(disk, path));
    CHECK(sync.save(live));
    CHECK(live.openrouter.api_key == "last-file-key");
    CHECK(live.technique == Technique::Standard);
    CHECK(live.technique_id == "review-plugin");
    CHECK(loadConfig(path) == live);

    const auto permissions = fs::status(path).permissions() & fs::perms::all;
    CHECK(permissions == (fs::perms::owner_read | fs::perms::owner_write));
    const auto saved_time = fs::last_write_time(path);
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
    CHECK(sync.save(live));
    CHECK(fs::last_write_time(path) == saved_time);
    CHECK((fs::status(path).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));
}

void testConfigSynchronizationFailures() {
    TempDir directory;
    const fs::path path = directory.path / "config.json";
    Config live;
    live.openrouter.api_key = "original-key";
    CHECK(saveConfig(live, path));
    ConfigFileSync sync(path, live);
    const Config original = live;
    writeFile(path, "{unfinished JSON");
    CHECK(!sync.reload(live));
    CHECK(live == original);
    Config staged = live;
    staged.openrouter.api_key = "settings-key";
    CHECK(!sync.save(staged));
    CHECK(staged.openrouter.api_key == "settings-key");
    std::ifstream file(path);
    std::string contents;
    std::getline(file, contents);
    CHECK(contents == "{unfinished JSON");
    CHECK(saveConfig(original, path));
    CHECK(sync.save(staged));
    CHECK(loadConfig(path)->openrouter.api_key == "settings-key");

    fs::remove(path);
    live = staged;
    CHECK(!sync.reload(live));
    CHECK(live == staged);
    CHECK(sync.save(live));
    CHECK(loadConfig(path) == staged);

    const fs::path blocked = directory.path / "blocked";
    fs::create_directory(blocked);
    CHECK(!saveConfig(live, blocked));
    CHECK(fs::is_directory(blocked));
    for (const auto& entry : fs::directory_iterator(directory.path)) {
        CHECK(entry.path().filename().string().find(".tmp.") ==
              std::string::npos);
    }
}

void testConfigLegacyAndSymlink() {
    TempDir directory;
    const fs::path path = directory.path / "legacy.json";
    writeFile(path,
              R"({"provider":"openrouter","openrouter_api_key":"legacy-old"})");
    Config live = *loadConfig(path);
    ConfigFileSync sync(path, live);
    writeFile(path,
              R"({"provider":"openrouter","openrouter_api_key":"legacy-new"})");
    CHECK(sync.reload(live));
    CHECK(live.openrouter.api_key == "legacy-new");
    live.openrouter.api_key = "settings-migrated-key";
    CHECK(sync.save(live));
    CHECK(loadConfig(path)->openrouter.api_key == "settings-migrated-key");

    const fs::path link = directory.path / "config-link.json";
    fs::create_symlink(path, link);
    live.openrouter.api_key = "symlink-key";
    CHECK(saveConfig(live, link));
    CHECK(fs::is_symlink(link));
    CHECK(loadConfig(path) == live);
    CHECK(loadConfig(link) == live);
}

void testConfigKeyUsedByTurn() {
    EnvironmentGuard environment("OPENROUTER_API_KEY");
    TempDir directory;
    const fs::path path = directory.path / "config.json";
    AgentContext context;
    context.workspace = directory.path;
    context.config.openrouter.api_key = "sk-or-v1-old-test-key";
    CHECK(saveConfig(context.config, path));
    ConfigFileSync sync(path, context.config);
    Config disk = context.config;
    disk.openrouter.api_key = "sk-or-v1-latest-test-key";
    CHECK(saveConfig(disk, path));
    CHECK(sync.reload(context.config));
    int calls = 0;
    context.chat_handler = [&calls](const Config& config, const ChatRequest&,
                                    std::stop_token) {
        ++calls;
        CHECK(effectiveApiKey(config, Provider::OpenRouter) ==
              "sk-or-v1-latest-test-key");
        ChatResponse response;
        response.text = "answer";
        return response;
    };
    std::vector<Message> messages;
    CHECK(runTurn(context, messages, "check latest key", {}) ==
          TurnOutcome::Completed);
    CHECK(calls == 1);

    setenv("OPENROUTER_API_KEY", "sk-or-v1-environment-test-key", 1);
    CHECK(effectiveApiKey(context.config, Provider::OpenRouter) ==
          "sk-or-v1-environment-test-key");
    CHECK(sync.save(context.config));
    CHECK(loadConfig(path)->openrouter.api_key == "sk-or-v1-latest-test-key");
}

void testPluginParameters() {
    PluginParameter boolean;
    boolean.key = "enabled";
    boolean.type = PluginParameterType::Boolean;
    CHECK(normalizePluginParameterValue(boolean, "YES") == "true");
    CHECK(normalizePluginParameterValue(boolean, "off") == "false");
    CHECK(!normalizePluginParameterValue(boolean, "sometimes"));

    PluginParameter choice;
    choice.key = "mode";
    choice.type = PluginParameterType::Choice;
    choice.options = {"quick", "careful"};
    CHECK(normalizePluginParameterValue(choice, "careful") == "careful");
    CHECK(!normalizePluginParameterValue(choice, "other"));

    PluginParameter integer;
    integer.key = "attempts";
    integer.type = PluginParameterType::Integer;
    CHECK(normalizePluginParameterValue(integer, "12") == "12");
    CHECK(!normalizePluginParameterValue(integer, "12x"));

    TempDir directory;
    const fs::path path = directory.path / "config.json";
    Config config;
    config.plugin_parameters["verify-first"]["mode"] = "careful";
    config.plugin_parameters["verify-first"]["enabled"] = "true";
    CHECK(saveConfig(config, path));
    const auto loaded = loadConfig(path);
    CHECK(loaded.has_value());
    CHECK(loaded->plugin_parameters.at("verify-first").at("mode") ==
          "careful");
    CHECK(loaded->plugin_parameters.at("verify-first").at("enabled") ==
          "true");
}

void testHttpCancellation() {
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(server >= 0);
    if (server < 0) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(::bind(server, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == 0);
    CHECK(::listen(server, 1) == 0);
    socklen_t length = sizeof(address);
    CHECK(::getsockname(server, reinterpret_cast<sockaddr*>(&address),
                        &length) == 0);

    std::jthread stall([server](std::stop_token stop) {
        const int client = ::accept(server, nullptr, nullptr);
        if (client >= 0) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!stop.stop_requested() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ::close(client);
        }
        ::close(server);
    });
    std::stop_source source;
    std::jthread cancel([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        source.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto response = ovicode::http::post(
        "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)), {},
        "{}", 10, source.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    stall.request_stop();
    CHECK(response.cancelled);
    CHECK(elapsed < std::chrono::seconds(2));
}

void testTurnOutcomes() {
    TempDir workspace;
    AgentContext context;
    context.workspace = workspace.path;
    context.config.max_context_bytes = 1024 * 1024;
    context.chat_handler = [](const Config&, const ChatRequest& request,
                              std::stop_token) {
        CHECK(!request.messages.empty());
        ChatResponse response;
        response.text = "answer";
        return response;
    };
    std::vector<Message> messages;
    std::vector<TranscriptEntry> entries;
    const auto completed = runTurn(
        context, messages, "question",
        [&](const TranscriptEntry& entry) {
            entries.push_back(entry);
            return true;
        });
    CHECK(completed == TurnOutcome::Completed);
    CHECK(messages.size() == 2);
    CHECK(entries.size() == 2);

    const auto original = messages;
    std::stop_source source;
    source.request_stop();
    const auto cancelled = runTurn(context, messages, "cancel", {},
                                   source.get_token());
    CHECK(cancelled == TurnOutcome::Cancelled);
    CHECK(messages.size() == original.size());
    CHECK(messages.back().text == original.back().text);

    context.chat_handler = [](const Config&, const ChatRequest&,
                              std::stop_token) {
        ChatResponse response;
        response.error = "scripted failure";
        return response;
    };
    CHECK(runTurn(context, messages, "fail", {}) == TurnOutcome::Failed);

    AgentContext compacting;
    compacting.workspace = workspace.path;
    compacting.config.max_context_bytes = 64 * 1024;
    compacting.config.max_request_bytes = 64 * 1024;
    compacting.chat_handler = [](const Config&, const ChatRequest& request,
                                 std::stop_token) {
        bool has_goal = false;
        for (const auto& message : request.messages) {
            if (message.text == "protected-current-goal") has_goal = true;
        }
        CHECK(has_goal);
        ChatResponse response;
        response.text = "compacted answer";
        return response;
    };
    std::vector<Message> old_messages{
        message(Message::Role::User, std::string(40000, 'a')),
        message(Message::Role::Assistant, std::string(40000, 'b')),
    };
    CHECK(runTurn(compacting, old_messages, "protected-current-goal", {}) ==
          TurnOutcome::Completed);

    TempDir rag_workspace;
    writeFile(rag_workspace.path / "inside.txt", "shared-keyword inside-only");
    TempDir rag_external;
    writeFile(rag_external.path / "secret.txt",
              "shared-keyword outside-secret-keyword");
    std::error_code symlink_error;
    fs::create_symlink(rag_external.path / "secret.txt",
                       rag_workspace.path / "secret-link.txt", symlink_error);
    AgentContext rag_context;
    rag_context.workspace = rag_workspace.path;
    rag_context.config.technique = Technique::Rag;
    rag_context.config.technique_id = "rag";
    int rag_calls = 0;
    rag_context.chat_handler = [&rag_calls](const Config&,
                                            const ChatRequest& request,
                                            std::stop_token) {
        ++rag_calls;
        bool found_inside = false;
        for (const auto& message : request.messages) {
            if (message.text.starts_with("Retrieved excerpts")) {
                found_inside =
                    message.text.find("inside-only") != std::string::npos;
                CHECK(message.text.find("outside-secret-keyword") ==
                      std::string::npos);
            }
        }
        if (rag_calls == 1) CHECK(found_inside);
        if (rag_calls == 2) CHECK(!found_inside);
        ChatResponse response;
        response.text = "rag answer";
        return response;
    };
    std::vector<Message> rag_messages;
    CHECK(runTurn(rag_context, rag_messages, "shared-keyword", {}) ==
          TurnOutcome::Completed);
    fs::remove(rag_workspace.path / "inside.txt");
    rag_messages.clear();
    CHECK(runTurn(rag_context, rag_messages, "shared-keyword", {}) ==
          TurnOutcome::Completed);
}

}  // namespace

int main() {
    TempDir metrics_dir;
    const fs::path metrics_file = metrics_dir.path / "metrics.jsonl";
    setenv("OVI_CODE_METRICS_FILE", metrics_file.c_str(), 1);

    testUtf8();
    testTranscript();
    testContext();
    testProviderSerialization();
    testTools();
    testConfigBounds();
    testConfigSynchronization();
    testConfigSynchronizationFailures();
    testConfigLegacyAndSymlink();
    testConfigKeyUsedByTurn();
    testPluginParameters();
    testHttpCancellation();
    testTurnOutcomes();

    ovicode::metrics::flush();
    CHECK(fs::exists(metrics_file));
    std::ifstream metrics(metrics_file);
    std::string line;
    std::getline(metrics, line);
    const auto document = nlohmann::json::parse(line);
    CHECK(document["schema"] == "ovi-code.metrics.v1");
    CHECK(document["counters"]["tool_calls_total"].get<int>() >= 3);
    CHECK(document["distributions"]["tool_duration_us"]["buckets"].size() ==
          10);
    const auto& tool_distribution =
        document["distributions"]["tool_duration_us"];
    CHECK(tool_distribution["buckets"].back()["count"] ==
          tool_distribution["count"]);

    if (failures != 0) {
        std::cerr << failures << " test checks failed\n";
        return 1;
    }
    std::cout << "all core checks passed\n";
    return 0;
}
