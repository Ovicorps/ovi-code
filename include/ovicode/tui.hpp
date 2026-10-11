#pragma once

#include <filesystem>

#include "ovicode/config.hpp"
#include "ovicode/plugins.hpp"
#include "ovicode/session.hpp"

namespace ovicode {

// Returns true if the user completed setup, false if they aborted.
bool runSetupWizard(Config& cfg, const TechniquePluginRegistry& plugins);

// Main chat REPL screen. Loops until the user quits. Settings are reached from
// here as an in-screen modal overlay (see makeSettingsPanel in tui.cpp).
void runChatScreen(Config& cfg, const std::filesystem::path& workspace,
                   const std::filesystem::path& agent_config_dir,
                    const TechniquePluginRegistry& plugins,
                    ConfigFileSync& config_file, SessionStore& session);

}  // namespace ovicode
