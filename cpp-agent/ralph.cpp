#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <array>
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;

static std::string runCommand(const std::string& command, bool printOutput = false) {
    std::array<char, 256> buffer{};
    std::string result;

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return result;
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        std::string chunk = buffer.data();
        result += chunk;

        if (printOutput) {
            std::cerr << chunk;
        }
    }

    pclose(pipe);
    return result;
}

static std::string readFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        return "";
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
}

static void appendFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::app);
    file << content;
}

static bool fileExists(const fs::path& path) {
    return fs::exists(path) && fs::is_regular_file(path);
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string shellQuote(const std::string& s) {
    std::string quoted = "'";
    for (char c : s) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

static std::string currentDate() {
    return trim(runCommand("date +%Y-%m-%d"));
}

static std::string currentDateTime() {
    return trim(runCommand("date"));
}

static std::string getExecutableDir(const char* argv0) {
    char resolved[4096];

    if (realpath(argv0, resolved)) {
        return fs::path(resolved).parent_path().string();
    }

    return fs::current_path().string();
}

static std::string getBranchNameFromPrd(const fs::path& prdFile) {
    if (!fileExists(prdFile)) {
        return "";
    }

    std::string command =
        "jq -r '.branchName // empty' " +
        shellQuote(prdFile.string()) +
        " 2>/dev/null";

    return trim(runCommand(command));
}

static void initializeProgressFile(const fs::path& progressFile) {
    writeFile(progressFile, "# Ralph Progress Log\n");
    appendFile(progressFile, "Started: " + currentDateTime() + "\n");
    appendFile(progressFile, "---\n");
}

int main(int argc, char* argv[]) {
    std::string tool = "amp";
    int maxIterations = 10;

    std::regex numberRegex("^[0-9]+$");

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--tool") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --tool requires a value.\n";
                return 1;
            }

            tool = argv[++i];
        } else if (arg.rfind("--tool=", 0) == 0) {
            tool = arg.substr(std::string("--tool=").size());
        } else if (std::regex_match(arg, numberRegex)) {
            maxIterations = std::stoi(arg);
        }
    }

    if (tool != "amp" && tool != "claude") {
        std::cerr << "Error: Invalid tool '" << tool
                  << "'. Must be 'amp' or 'claude'.\n";
        return 1;
    }

    fs::path scriptDir = getExecutableDir(argv[0]);
    fs::path prdFile = scriptDir / "prd.json";
    fs::path progressFile = scriptDir / "progress.txt";
    fs::path archiveDir = scriptDir / "archive";
    fs::path lastBranchFile = scriptDir / ".last-branch";

    // Archive previous run if branch changed
    if (fileExists(prdFile) && fileExists(lastBranchFile)) {
        std::string currentBranch = getBranchNameFromPrd(prdFile);
        std::string lastBranch = trim(readFile(lastBranchFile));

        if (!currentBranch.empty() &&
            !lastBranch.empty() &&
            currentBranch != lastBranch) {

            std::string date = currentDate();

            std::string folderName = lastBranch;
            const std::string prefix = "ralph/";
            if (folderName.rfind(prefix, 0) == 0) {
                folderName = folderName.substr(prefix.size());
            }

            fs::path archiveFolder = archiveDir / (date + "-" + folderName);

            std::cout << "Archiving previous run: " << lastBranch << "\n";

            fs::create_directories(archiveFolder);

            if (fileExists(prdFile)) {
                fs::copy_file(
                    prdFile,
                    archiveFolder / prdFile.filename(),
                    fs::copy_options::overwrite_existing
                );
            }

            if (fileExists(progressFile)) {
                fs::copy_file(
                    progressFile,
                    archiveFolder / progressFile.filename(),
                    fs::copy_options::overwrite_existing
                );
            }

            std::cout << "   Archived to: " << archiveFolder.string() << "\n";

            initializeProgressFile(progressFile);
        }
    }

    // Track current branch
    if (fileExists(prdFile)) {
        std::string currentBranch = getBranchNameFromPrd(prdFile);
        if (!currentBranch.empty()) {
            writeFile(lastBranchFile, currentBranch + "\n");
        }
    }

    // Initialize progress file if it doesn't exist
    if (!fileExists(progressFile)) {
        initializeProgressFile(progressFile);
    }

    std::cout << "Starting Ralph - Tool: " << tool
              << " - Max iterations: " << maxIterations << "\n";

    for (int i = 1; i <= maxIterations; ++i) {
        std::cout << "\n";
        std::cout << "===============================================================\n";
        std::cout << "  Ralph Iteration " << i << " of " << maxIterations
                  << " (" << tool << ")\n";
        std::cout << "===============================================================\n";

        std::string command;

        if (tool == "amp") {
            fs::path promptFile = scriptDir / "prompt.md";

            command =
                "cat " + shellQuote(promptFile.string()) +
                " | amp --dangerously-allow-all 2>&1";
        } else {
            fs::path claudeFile = scriptDir / "CLAUDE.md";

            command =
                "claude --dangerously-skip-permissions --print < " +
                shellQuote(claudeFile.string()) +
                " 2>&1";
        }

        std::string output = runCommand(command, true);

        if (output.find("<promise>COMPLETE</promise>") != std::string::npos) {
            std::cout << "\n";
            std::cout << "Ralph completed all tasks!\n";
            std::cout << "Completed at iteration " << i
                      << " of " << maxIterations << "\n";
            return 0;
        }

        std::cout << "Iteration " << i << " complete. Continuing...\n";
        sleep(2);
    }

    std::cout << "\n";
    std::cout << "Ralph reached max iterations (" << maxIterations
              << ") without completing all tasks.\n";
    std::cout << "Check " << progressFile.string() << " for status.\n";

    return 1;
}
