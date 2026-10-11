#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ovicode {

enum class Provider {
    OpenRouter,
    OpenAI,
    Anthropic,
    XAI,
};

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

struct ProviderInfo {
    Provider provider;
    std::string id;
    std::string label;
    std::string environment_variable;
    std::string key_placeholder;
};

const std::vector<ProviderInfo>& knownProviders();
const ProviderInfo& providerInfo(Provider provider);
std::string providerLabel(Provider provider);
Provider providerFromString(const std::string& value);
std::string providerToString(Provider provider);

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
const std::vector<ModelInfo>& knownModelInfo(Provider provider);
const ModelInfo* findModelInfo(Provider provider, const std::string& model);
std::vector<ReasoningEffort> reasoningEffortOptions(Provider provider,
                                                     const std::string& model);
ReasoningEffort effectiveReasoningEffort(Provider provider,
                                         const std::string& model,
                                         ReasoningEffort configured);
const std::vector<std::string>& knownModels(Provider provider);
const std::string& defaultModel(Provider provider);

struct ProviderAccount {
    std::string model;
    std::string api_key;
};

struct Config {
    Provider provider = Provider::OpenRouter;
    ProviderAccount openrouter{defaultModel(Provider::OpenRouter), {}};
    ProviderAccount openai{defaultModel(Provider::OpenAI), {}};
    ProviderAccount anthropic{defaultModel(Provider::Anthropic), {}};
    ProviderAccount xai{defaultModel(Provider::XAI), {}};
    Technique technique = Technique::Standard;
    std::string technique_id = "standard";
    Theme theme = Theme::Plain;
    ReasoningEffort reasoning_effort = ReasoningEffort::Auto;
    int max_tool_iterations = 12;
    int ralph_iterations = 3;
    int plugin_iterations = 3;
    int max_tokens = 4096;
    std::size_t max_context_bytes = 2 * 1024 * 1024;
    std::size_t max_request_bytes = 4 * 1024 * 1024;
    double temperature = 0.2;
    // plugin id -> parameter key -> serialized value
    std::map<std::string, std::map<std::string, std::string>> plugin_parameters;
};

ProviderAccount& providerAccount(Config& config, Provider provider);
const ProviderAccount& providerAccount(const Config& config,
                                       Provider provider);
std::string& selectedModel(Config& config);
const std::string& selectedModel(const Config& config);
std::string effectiveApiKey(const Config& config, Provider provider);

std::filesystem::path defaultConfigDir();
std::filesystem::path defaultConfigPath();
std::optional<Config> loadConfig(const std::filesystem::path& path);
bool saveConfig(const Config& cfg, const std::filesystem::path& path);

// If `path` doesn't exist but a config from the prior ~/.config/ovi-code/
// location does, move it to `path` (creating parent dirs). Returns true if a
// migration happened.
bool migrateLegacyConfig(const std::filesystem::path& path);

}  // namespace ovicode
