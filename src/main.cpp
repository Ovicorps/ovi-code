#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "ovicode/agent_md.hpp"
#include "ovicode/config.hpp"
#include "ovicode/plugins.hpp"
#include "ovicode/tui.hpp"

namespace {

namespace fs = std::filesystem;

enum class Command {
    Run,
    Init,
    Plugins,
    Uninstall,
};

struct Args {
    fs::path workspace;
    fs::path agent_config_dir;
    fs::path config_path;
    fs::path plugin_dir;
    Command command = Command::Run;
    bool show_help = false;
    bool reconfigure = false;
    bool show_splash = true;
};

bool stdoutIsTty() { return isatty(STDOUT_FILENO) != 0; }

void printSplash() {
    std::cout << "\n"
              << "             .:--=++****++=-:.                                        .:-=**:.\n"
              << "         .-+#%@@@@@@@@@@@@@@@%#+-.                                   .%@@@@@@%\n"
              << "      .-*%@@@@@@@@@@@@@@@@@@@@@@@%+:                                -@@@@@@@@@\n"
              << "    .=%@@@@@@@@@@@@@@@@@@@@@@@@@@@@@#:                             -@@@@@@@@@@\n"
              << "   -%@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@+.                          -@@@@@@@@@%\n"
              << "  .+@@@@@@@@@@@@%#+=--:---+*%@@@@@@@@@@@%:                       .#@@@@@@@@@@\n"
              << "  .*@@@@@@@@@@@*-.           .-#@@@@@@@@@@@-                    .#@@@@@@@@@@\n"
              << "  +@@@@@@@@@@#:                .=@@@@@@@@@@@:                   *@@@@@@@@@@=\n"
              << "  :@@@@@@@@@@*.                   :%@@@@@@@@@%.                +@@@@@@@@@@+\n"
              << "  *@@@@@@@@@#.                     :@@@@@@@@@@#.              =@@@@@@@@@@*\n"
              << "  .@@@@@@@@@@-                       -@@@@@@@@@@=            -@@@@@@@@@@*.\n"
              << "  -@@@@@@@@@%.                        *@@@@@@@@@%.          -@@@@@@@@@@#.\n"
              << "  -@@@@@@@@@%.                        .%@@@@@@@@@+         :@@@@@@@@@@%.\n"
              << "  :@@@@@@@@@@:                         =@@@@@@@@@@.       :%@@@@@@@@@%:\n"
              << "  .#@@@@@@@@@*                         .%@@@@@@@@@+      .%@@@@@@@@@@:\n"
              << "   -@@@@@@@@@@=                         =@@@@@@@@@@.    .%@@@@@@@@@@-\n"
              << "   .*@@@@@@@@@@*.                       .%@@@@@@@@@+   .#@@@@@@@@@@-\n"
              << "    .#@@@@@@@@@@@*-..                    =@@@@@@@@@@: .#@@@@@@@@@@=\n"
              << "     .*@@@@@@@@@@@@@%#***=:              .%@@@@@@@@@#-%@@@@@@@@@@=\n"
              << "       =%@@@@@@@@@@@@@@@@@@*.             :@@@@@@@@@@@@@@@@@@@@@=\n"
              << "        .=%@@@@@@@@@@@@@@@@@=              =@@@@@@@@@@@@@@@@@@@=\n"
              << "          .-*%@@@@@@@@@@@@@@:               -%@@@@@@@@@@@@@@@#:\n"
              << "             .:=*#%@@@@@@%*:                 .=%@@@@@@@@@@@*-.\n"
              << "                  ..:::::.                     .:=+*###*=-.\n"
              << "\n"
              << "OVI-CODE\n"
              << "Tiny code-editing TUI agent\n"
              << "Read | Edit | Reason | Refactor | Run\n"
              << "An Ovicorps tool\n"
              << "\n"
              << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
}

void printHelp() {
    std::cout <<
        "OVI-CODE — tiny code-editing TUI agent\n"
        "\n"
        "Usage: ovi-code [command] [options]\n"
        "\n"
        "Commands:\n"
        "  init                       Run setup, save config, and exit\n"
        "  plugins                    List runtime technique plugins\n"
        "  uninstall                  Remove saved config and exit\n"
        "\n"
        "Options:\n"
        "  --workspace <dir>          Workspace root (default: cwd)\n"
        "  --agent-config-dir <dir>   Where to read agent.md / tools.md /\n"
        "                             skills.md from (default: workspace)\n"
        "  --config <file>            Path to ovi-code config.json\n"
        "                             (default: ~/.ovi-code/config.json)\n"
        "  --plugin-dir <dir>         Runtime C++ technique plugins\n"
        "                             (default: ~/.ovi-code/plugins)\n"
        "  --setup                    Force the setup wizard to run\n"
        "  --init                     Alias for the init command\n"
        "  --list-plugins             Alias for the plugins command\n"
        "  --uninstall                Alias for the uninstall command\n"
        "  --no-splash                Skip the ASCII splash screen\n"
        "  -h, --help                 Show this help\n"
        "\n"
        "Environment overrides:\n"
        "  OPENROUTER_API_KEY         Overrides the saved OpenRouter API key.\n"
        "  OPENAI_API_KEY             Overrides the saved OpenAI API key.\n"
        "  ANTHROPIC_API_KEY          Overrides the saved Anthropic API key.\n"
        "  XAI_API_KEY                Overrides the saved xAI/Grok API key.\n"
        "  OVI_CODE_PLUGIN_DIR        Overrides the technique plugin directory.\n"
        "\n"
        "Keyboard shortcuts:\n"
        "  Insert mode                Type; Enter adds a line; Esc enters Normal mode\n"
        "  Normal movement            h/j/k/l, w/b, 0/$, gg/G\n"
        "  Normal editing             i/a/o/O/I/A, x, dd, yy, p/P\n"
        "  Enter                      Send the prompt from Normal mode\n"
        "  Ctrl+W                     Toggle input/output focus\n"
        "  Output focus               j/k scroll; g/G jump to top/bottom\n"
        "  z                          Minimize/restore the input panel\n"
        "  s / t / c / q              Settings / technique / clear / quit\n"
        "\n"
        "Settings shortcuts:\n"
        "  j/k or arrows              Change the focused list selection\n"
        "  Tab                        Next field; cycles a focused list\n"
        "  Enter                      Activate the focused button\n"
        "  i / a                      Edit the focused field\n"
        "  Esc                        Return to Normal mode, then cancel\n"
        "  q                          Cancel from Normal mode\n";
}

Args parseArgs(int argc, char** argv) {
    Args a;
    a.workspace = fs::current_path();
    a.config_path = ovicode::defaultConfigPath();
    auto selectCommand = [&](Command command, const std::string& argument) {
        if (a.command != Command::Run) {
            std::cerr << "Only one command may be specified; unexpected: "
                      << argument << "\n";
            std::exit(2);
        }
        a.command = command;
    };
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto take = [&](const char* /*flag*/) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << s << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "-h" || s == "--help") {
            a.show_help = true;
        } else if (s == "init" || s == "--init") {
            selectCommand(Command::Init, s);
        } else if (s == "plugins" || s == "--list-plugins") {
            selectCommand(Command::Plugins, s);
        } else if (s == "uninstall" || s == "--uninstall") {
            selectCommand(Command::Uninstall, s);
        } else if (s == "--workspace") {
            a.workspace = fs::path(take("--workspace"));
        } else if (s == "--agent-config-dir") {
            a.agent_config_dir = fs::path(take("--agent-config-dir"));
        } else if (s == "--config") {
            a.config_path = fs::path(take("--config"));
        } else if (s == "--plugin-dir") {
            a.plugin_dir = fs::path(take("--plugin-dir"));
        } else if (s == "--setup") {
            a.reconfigure = true;
        } else if (s == "--no-splash") {
            a.show_splash = false;
        } else {
            std::cerr << "Unknown argument: " << s << "\n";
            printHelp();
            std::exit(2);
        }
    }
    if (a.agent_config_dir.empty()) a.agent_config_dir = a.workspace;
    if (a.plugin_dir.empty()) {
        if (const char* env = std::getenv("OVI_CODE_PLUGIN_DIR"); env && *env) {
            a.plugin_dir = env;
        } else {
            a.plugin_dir = ovicode::defaultConfigDir() / "plugins";
        }
    }
    return a;
}

int uninstallConfig(const fs::path& path) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        std::cerr << "Failed to inspect config " << path << ": "
                  << ec.message() << "\n";
        return 1;
    }
    if (!exists) {
        std::cout << "No saved configuration found at " << path << "\n";
        return 0;
    }
    if (fs::is_directory(path, ec)) {
        std::cerr << "Refusing to remove config path because it is a directory: "
                  << path << "\n";
        return 1;
    }
    if (!fs::remove(path, ec) || ec) {
        std::cerr << "Failed to remove config " << path << ": "
                  << ec.message() << "\n";
        return 1;
    }

    if (path == ovicode::defaultConfigPath()) {
        std::error_code cleanup_ec;
        fs::remove(path.parent_path(), cleanup_ec);  // Remove only if empty.
    }
    std::cout << "Removed saved configuration: " << path << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    if (args.show_help) {
        printHelp();
        return 0;
    }
    if (args.command == Command::Uninstall) {
        return uninstallConfig(args.config_path);
    }

    if (args.command == Command::Run && args.show_splash && stdoutIsTty()) {
        printSplash();
    }

    std::error_code ec;
    args.workspace = fs::absolute(args.workspace, ec);
    args.agent_config_dir = fs::absolute(args.agent_config_dir, ec);
    args.plugin_dir = fs::absolute(args.plugin_dir, ec);

    ovicode::TechniquePluginRegistry plugins;
    plugins.loadDirectory(args.plugin_dir);
    if (args.command == Command::Plugins) {
        std::cout << "Technique plugin directory: " << args.plugin_dir << "\n";
        for (const auto& plugin : plugins.plugins()) {
            std::cout << "  " << plugin->id << "\t" << plugin->label << " — "
                      << plugin->description << "\n";
        }
        if (plugins.plugins().empty()) std::cout << "  (no plugins loaded)\n";
        for (const auto& error : plugins.errors()) {
            std::cerr << "Plugin error: " << error << "\n";
        }
        return plugins.errors().empty() ? 0 : 1;
    }

    ovicode::migrateLegacyConfig(args.config_path);
    auto maybe_cfg = ovicode::loadConfig(args.config_path);
    ovicode::Config cfg = maybe_cfg.value_or(ovicode::Config{});
    const bool init_only = args.command == Command::Init;

    if (!maybe_cfg.has_value() ||
        ovicode::effectiveApiKey(cfg, cfg.provider).empty() ||
        args.reconfigure || init_only) {
        if (!ovicode::runSetupWizard(cfg, plugins)) {
            std::cerr << "Setup cancelled.\n";
            return 1;
        }
        if (!ovicode::saveConfig(cfg, args.config_path)) {
            std::cerr << (init_only ? "Failed" : "Warning: failed")
                      << " to save config to " << args.config_path << "\n";
            if (init_only) return 1;
        }
    }
    if (init_only) {
        std::cout << "Configuration saved to " << args.config_path << "\n";
        return 0;
    }

    if (ovicode::selectedModel(cfg).empty()) {
        ovicode::selectedModel(cfg) = ovicode::defaultModel(cfg.provider);
    }

    ovicode::runChatScreen(cfg, args.workspace, args.agent_config_dir, plugins);

    // Persist any settings changed during the session.
    ovicode::saveConfig(cfg, args.config_path);
    return 0;
}
