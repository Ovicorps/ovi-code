#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ovicode {

enum class Provider {
    Anthropic,
    OpenAI,
    Gemini,
    Ollama,
};

enum class Technique {
    Standard,
    ChainOfThought,
    RalphLoop,
    Rag,
};

struct ProviderInfo {
    Provider provider;
    std::string label;
    std::string default_model;
    std::string default_base_url;
    bool needs_api_key;
};

const std::vector<ProviderInfo>& knownProviders();
const ProviderInfo& providerInfo(Provider p);
std::string providerLabel(Provider p);
Provider providerFromString(const std::string& s);
std::string providerToString(Provider p);

std::string techniqueLabel(Technique t);
std::string techniqueDescription(Technique t);
Technique techniqueFromString(const std::string& s);
std::string techniqueToString(Technique t);
const std::vector<Technique>& allTechniques();

const std::vector<std::string>& knownModels(Provider p);

struct Config {
    Provider provider = Provider::Anthropic;
    std::string model;
    std::string api_key;
    std::string base_url;
    Technique technique = Technique::Standard;
    int max_tool_iterations = 12;
    int ralph_iterations = 3;
    int max_tokens = 4096;
    double temperature = 0.2;
};

std::filesystem::path defaultConfigDir();
std::filesystem::path defaultConfigPath();
std::optional<Config> loadConfig(const std::filesystem::path& path);
bool saveConfig(const Config& cfg, const std::filesystem::path& path);

}  // namespace ovicode
