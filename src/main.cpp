#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "ovicode/agent_md.hpp"
#include "ovicode/config.hpp"
#include "ovicode/tui.hpp"

namespace {

namespace fs = std::filesystem;

struct Args {
    fs::path workspace;
    fs::path agent_config_dir;
    fs::path config_path;
    bool show_help = false;
    bool reconfigure = false;
    bool show_splash = true;
};

bool stdoutIsTty() { return isatty(STDOUT_FILENO) != 0; }

bool colorAllowed() {
    if (!stdoutIsTty()) return false;
    const char* nc = std::getenv("NO_COLOR");
    return !(nc && *nc);
}

void printSplash(bool with_color) {
    // ANSI Shadow block letters. Source must remain UTF-8.
    const char* kCyan  = with_color ? "\033[1;36m" : "";
    const char* kDim   = with_color ? "\033[2;36m" : "";
    const char* kReset = with_color ? "\033[0m"    : "";
    std::cout << "\n"
              << kCyan
              <<"             .:--=++****++=-:.                                        .:-=**:.\n"
              <<"         .-+#%@@@@@@@@@@@@@@@%#+-.                                   .%@@@@@@%\n"
              <<"      .-*%@@@@@@@@@@@@@@@@@@@@@@@%+:                                -@@@@@@@@@\n"
              <<"    .=%@@@@@@@@@@@@@@@@@@@@@@@@@@@@@#:                             -@@@@@@@@@@\n"
              <<"   -%@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@+.                          -@@@@@@@@@%\n"
              <<"  .+@@@@@@@@@@@@%#+=--:---+*%@@@@@@@@@@@%:                       .#@@@@@@@@@@\n"
              <<"  .*@@@@@@@@@@@*-.           .-#@@@@@@@@@@@-                    .#@@@@@@@@@@\n"
              <<"  +@@@@@@@@@@#:                .=@@@@@@@@@@@:                   *@@@@@@@@@@=\n"
              <<"  :@@@@@@@@@@*.                   :%@@@@@@@@@%.                +@@@@@@@@@@+\n"
              <<"  *@@@@@@@@@#.                     :@@@@@@@@@@#.              =@@@@@@@@@@*\n"
              <<"  .@@@@@@@@@@-                       -@@@@@@@@@@=            -@@@@@@@@@@*.\n"
              <<"  -@@@@@@@@@%.                        *@@@@@@@@@%.          -@@@@@@@@@@#.\n"
              <<"  -@@@@@@@@@%.                        .%@@@@@@@@@+         :@@@@@@@@@@%.\n"
              <<"  :@@@@@@@@@@:                         =@@@@@@@@@@.       :%@@@@@@@@@%:\n"
              <<"  .#@@@@@@@@@*                         .%@@@@@@@@@+      .%@@@@@@@@@@:\n"
              <<"   -@@@@@@@@@@=                         =@@@@@@@@@@.    .%@@@@@@@@@@-\n"
              <<"   .*@@@@@@@@@@*.                       .%@@@@@@@@@+   .#@@@@@@@@@@-\n"
              <<"    .#@@@@@@@@@@@*-..                    =@@@@@@@@@@: .#@@@@@@@@@@=\n"
              <<"     .*@@@@@@@@@@@@@%#***=:              .%@@@@@@@@@#-%@@@@@@@@@@=\n"
              <<"       =%@@@@@@@@@@@@@@@@@@*.             :@@@@@@@@@@@@@@@@@@@@@=\n"
              <<"        .=%@@@@@@@@@@@@@@@@@=              =@@@@@@@@@@@@@@@@@@@=\n"
              <<"          .-*%@@@@@@@@@@@@@@:               -%@@@@@@@@@@@@@@@#:\n"
              <<"             .:=*#%@@@@@@%*:                 .=%@@@@@@@@@@@*-.\n"
              <<"                  ..:::::.                     .:=+*###*=-.\n"
              << "\n"
              << "\n"
              <<"                     AN OVICORPS TOOL, CREATED WITH LOVE\n"
              <<"                     -----------------------------------\n"
              <<"                      OVICORPS 2026, ALL RIGHTS RESERVED\n"
              << "\n"
              << "\n"
              << "   ╔═══════════════════════════════════════════════════════════════════════╗\n"
              << "   ║                                                                       ║\n"
              << "   ║     ██████╗ ██╗   ██╗██╗       ██████╗ ██████╗ ██████╗ ███████╗       ║\n"
              << "   ║    ██╔═══██╗██║   ██║██║      ██╔════╝██╔═══██╗██╔══██╗██╔════╝       ║\n"
              << "   ║    ██║   ██║██║   ██║██║█████╗██║     ██║   ██║██║  ██║█████╗         ║\n"
              << "   ║    ██║   ██║╚██╗ ██╔╝██║╚════╝██║     ██║   ██║██║  ██║██╔══╝         ║\n"
              << "   ║    ╚██████╔╝ ╚████╔╝ ██║      ╚██████╗╚██████╔╝██████╔╝███████╗       ║\n"
              << "   ║     ╚═════╝   ╚═══╝  ╚═╝       ╚═════╝ ╚═════╝ ╚═════╝ ╚══════╝       ║\n"
              << "   ║                                                                       ║\n"
              << "   ║" << kReset << kDim
              << "          Read · Edit · Reason · Refactor · Run                        "
              << kReset << kCyan << "║\n"
              << "   ╚═══════════════════════════════════════════════════════════════════════╝\n"
              << kReset << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
}

void printHelp() {
    std::cout <<
        "OVI-CODE — tiny code-editing TUI agent\n"
        "\n"
        "Usage: ovi-code [options]\n"
        "\n"
        "Options:\n"
        "  --workspace <dir>          Workspace root (default: cwd)\n"
        "  --agent-config-dir <dir>   Where to read agent.md / tools.md /\n"
        "                             skills.md from (default: workspace)\n"
        "  --config <file>            Path to ovi-code config.json\n"
        "                             (default: ~/.ovi-code/config.json)\n"
        "  --setup                    Force the setup wizard to run\n"
        "  --no-splash                Skip the ASCII splash screen\n"
        "  -h, --help                 Show this help\n"
        "\n"
        "Environment overrides:\n"
        "  OVI_CODE_API_KEY           Overrides the saved API key.\n";
}

Args parseArgs(int argc, char** argv) {
    Args a;
    a.workspace = fs::current_path();
    a.config_path = ovicode::defaultConfigPath();
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
        } else if (s == "--workspace") {
            a.workspace = fs::path(take("--workspace"));
        } else if (s == "--agent-config-dir") {
            a.agent_config_dir = fs::path(take("--agent-config-dir"));
        } else if (s == "--config") {
            a.config_path = fs::path(take("--config"));
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
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    if (args.show_help) {
        printHelp();
        return 0;
    }

    if (args.show_splash && stdoutIsTty()) {
        printSplash(colorAllowed());
    }

    std::error_code ec;
    args.workspace = fs::absolute(args.workspace, ec);
    args.agent_config_dir = fs::absolute(args.agent_config_dir, ec);

    ovicode::migrateLegacyConfig(args.config_path);
    auto maybe_cfg = ovicode::loadConfig(args.config_path);
    ovicode::Config cfg = maybe_cfg.value_or(ovicode::Config{});

    if (!maybe_cfg.has_value() || args.reconfigure) {
        if (!ovicode::runSetupWizard(cfg)) {
            std::cerr << "Setup cancelled.\n";
            return 1;
        }
        if (!ovicode::saveConfig(cfg, args.config_path)) {
            std::cerr << "Warning: failed to save config to "
                      << args.config_path << "\n";
        }
    }

    if (const char* envk = std::getenv("OVI_CODE_API_KEY"); envk && *envk) {
        cfg.api_key = envk;
    } else if (const char* legacy = std::getenv("OVICODE_API_KEY");
               legacy && *legacy) {
        cfg.api_key = legacy;
    }

    if (cfg.model.empty()) {
        cfg.model = ovicode::providerInfo(cfg.provider).default_model;
    }
    if (cfg.base_url.empty()) {
        cfg.base_url = ovicode::providerInfo(cfg.provider).default_base_url;
    }

    ovicode::runChatScreen(cfg, args.workspace, args.agent_config_dir);

    // Persist any settings changed during the session.
    ovicode::saveConfig(cfg, args.config_path);
    return 0;
}
