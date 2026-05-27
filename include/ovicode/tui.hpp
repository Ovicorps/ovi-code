#pragma once

#include <filesystem>

#include "ovicode/config.hpp"

namespace ovicode {

// Returns true if the user completed setup, false if they aborted.
bool runSetupWizard(Config& cfg);

// Main chat REPL screen. Loops until the user quits.
void runChatScreen(Config& cfg, const std::filesystem::path& workspace,
                   const std::filesystem::path& agent_config_dir);

// Settings panel — lets the user change provider, model, technique, etc.
// Returns true if anything changed.
bool runSettingsScreen(Config& cfg);

}  // namespace ovicode
