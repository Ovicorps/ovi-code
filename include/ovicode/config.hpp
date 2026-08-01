#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ovicode {

enum class Technique {
    Standard,
    ChainOfThought,
    RalphLoop,
    Rag,
};

enum class Theme {
    Auto,   // Follow system / terminal (auto-detected)
    Dark,
    Light,
    Plain,  // High-contrast monochrome
};

enum class ReasoningEffort {
    Auto,
    Max,
    XHigh,
    High,
    Medium,
    Low,
    Minimal,
    None,
};

struct ModelInfo {
    std::string id;
    std::vector<ReasoningEffort> supported_efforts;
    // Explicit effort for mandatory/default-enabled reasoning models. Auto
    // leaves optional reasoning entirely model-managed.
    ReasoningEffort auto_effort = ReasoningEffort::Auto;
    bool supports_temperature = true;
};

std::string techniqueLabel(Technique t);
std::string techniqueDescription(Technique t);
Technique techniqueFromString(const std::string& s);
std::string techniqueToString(Technique t);
const std::vector<Technique>& allTechniques();

std::string themeLabel(Theme t);
std::string themeDescription(Theme t);
Theme themeFromString(const std::string& s);
std::string themeToString(Theme t);
const std::vector<Theme>& allThemes();

std::string reasoningEffortLabel(ReasoningEffort effort);
std::string reasoningEffortToString(ReasoningEffort effort);
ReasoningEffort reasoningEffortFromString(const std::string& value);
const std::vector<ModelInfo>& knownModelInfo();
const ModelInfo* findModelInfo(const std::string& model);
std::vector<ReasoningEffort> reasoningEffortOptions(const std::string& model);
ReasoningEffort effectiveReasoningEffort(const std::string& model,
                                         ReasoningEffort configured);
const std::vector<std::string>& knownModels();
const std::string& defaultModel();

struct Config {
    std::string model = defaultModel();
    std::string openrouter_api_key;
    Technique technique = Technique::Standard;
    std::string technique_id = "standard";
    Theme theme = Theme::Plain;
    ReasoningEffort reasoning_effort = ReasoningEffort::Auto;
    int max_tool_iterations = 12;
    int ralph_iterations = 3;
    int plugin_iterations = 3;
    int max_tokens = 4096;
    double temperature = 0.2;
};

std::filesystem::path defaultConfigDir();
std::filesystem::path defaultConfigPath();
std::optional<Config> loadConfig(const std::filesystem::path& path);
bool saveConfig(const Config& cfg, const std::filesystem::path& path);

// If `path` doesn't exist but a config from the prior ~/.config/ovi-code/
// location does, move it to `path` (creating parent dirs). Returns true if a
// migration happened.
bool migrateLegacyConfig(const std::filesystem::path& path);

}  // namespace ovicode
