#include "ovicode/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace ovicode {

namespace fs = std::filesystem;
using json = nlohmann::json;

const std::vector<ProviderInfo>& knownProviders() {
    static const std::vector<ProviderInfo> providers = {
        {Provider::OpenRouter, "openrouter", "OpenRouter",
         "OPENROUTER_API_KEY", "sk-or-v1-..."},
        {Provider::OpenAI, "openai", "OpenAI", "OPENAI_API_KEY", "sk-..."},
        {Provider::Anthropic, "anthropic", "Anthropic",
         "ANTHROPIC_API_KEY", "sk-ant-..."},
        {Provider::XAI, "xai", "xAI (Grok)", "XAI_API_KEY", "xai-..."},
    };
    return providers;
}

const ProviderInfo& providerInfo(Provider provider) {
    for (const auto& info : knownProviders()) {
        if (info.provider == provider) return info;
    }
    return knownProviders().front();
}

std::string providerLabel(Provider provider) {
    return providerInfo(provider).label;
}

Provider providerFromString(const std::string& value) {
    if (value == "openai") return Provider::OpenAI;
    if (value == "anthropic") return Provider::Anthropic;
    if (value == "xai" || value == "grok") return Provider::XAI;
    return Provider::OpenRouter;
}

std::string providerToString(Provider provider) {
    return providerInfo(provider).id;
}

std::string techniqueLabel(Technique t) {
    switch (t) {
        case Technique::Standard:       return "Standard ReAct";
        case Technique::ChainOfThought: return "Chain of Thought";
        case Technique::RalphLoop:      return "Ralph Loop";
        case Technique::Rag:            return "RAG (keyword retrieval)";
    }
    return "Standard ReAct";
}

std::string techniqueDescription(Technique t) {
    switch (t) {
        case Technique::Standard:
            return "Plain ReAct loop: model picks tools until it answers.";
        case Technique::ChainOfThought:
            return "Adds a 'think step by step' preamble before the ReAct loop.";
        case Technique::RalphLoop:
            return "Re-issues the same goal N times — useful for iterative refactors.";
        case Technique::Rag:
            return "Greps the workspace for query keywords and prepends matches to context.";
    }
    return "";
}

std::string techniqueToString(Technique t) {
    switch (t) {
        case Technique::Standard:       return "standard";
        case Technique::ChainOfThought: return "cot";
        case Technique::RalphLoop:      return "ralph";
        case Technique::Rag:            return "rag";
    }
    return "standard";
}

Technique techniqueFromString(const std::string& s) {
    if (s == "cot")    return Technique::ChainOfThought;
    if (s == "ralph")  return Technique::RalphLoop;
    if (s == "rag")    return Technique::Rag;
    return Technique::Standard;
}

const std::vector<Technique>& allTechniques() {
    static const std::vector<Technique> v = {
        Technique::Standard,
        Technique::ChainOfThought,
        Technique::RalphLoop,
        Technique::Rag,
    };
    return v;
}

std::string themeLabel(Theme t) {
    switch (t) {
        case Theme::Auto:  return "System (auto-detect)";
        case Theme::Dark:  return "Dark";
        case Theme::Light: return "Light";
        case Theme::Plain: return "Plain (no colors)";
    }
    return "System (auto-detect)";
}

std::string themeDescription(Theme t) {
    switch (t) {
        case Theme::Auto:
            return "Follow the OS / terminal: macOS Appearance, then $COLORFGBG, then Dark.";
        case Theme::Dark:
            return "High-contrast accents on a fixed dark canvas.";
        case Theme::Light:
            return "High-contrast accents on a fixed light canvas.";
        case Theme::Plain:
            return "High-contrast monochrome with no accent colors.";
    }
    return "";
}

std::string themeToString(Theme t) {
    switch (t) {
        case Theme::Auto:  return "system";
        case Theme::Dark:  return "dark";
        case Theme::Light: return "light";
        case Theme::Plain: return "plain";
    }
    return "system";
}

Theme themeFromString(const std::string& s) {
    if (s == "dark")  return Theme::Dark;
    if (s == "light") return Theme::Light;
    if (s == "plain") return Theme::Plain;
    // "auto" accepted for back-compat with configs written before the rename.
    return Theme::Auto;
}

const std::vector<Theme>& allThemes() {
    static const std::vector<Theme> v = {
        Theme::Auto,
        Theme::Dark,
        Theme::Light,
        Theme::Plain,
    };
    return v;
}

std::string reasoningEffortLabel(ReasoningEffort effort) {
    switch (effort) {
        case ReasoningEffort::Auto:    return "Auto";
        case ReasoningEffort::Max:     return "Max";
        case ReasoningEffort::XHigh:   return "Extra high";
        case ReasoningEffort::High:    return "High";
        case ReasoningEffort::Medium:  return "Medium";
        case ReasoningEffort::Low:     return "Low";
        case ReasoningEffort::Minimal: return "Minimal";
        case ReasoningEffort::None:    return "None";
    }
    return "Auto";
}

std::string reasoningEffortToString(ReasoningEffort effort) {
    switch (effort) {
        case ReasoningEffort::Auto:    return "auto";
        case ReasoningEffort::Max:     return "max";
        case ReasoningEffort::XHigh:   return "xhigh";
        case ReasoningEffort::High:    return "high";
        case ReasoningEffort::Medium:  return "medium";
        case ReasoningEffort::Low:     return "low";
        case ReasoningEffort::Minimal: return "minimal";
        case ReasoningEffort::None:    return "none";
    }
    return "auto";
}

ReasoningEffort reasoningEffortFromString(const std::string& value) {
    if (value == "max") return ReasoningEffort::Max;
    if (value == "xhigh") return ReasoningEffort::XHigh;
    if (value == "high") return ReasoningEffort::High;
    if (value == "medium") return ReasoningEffort::Medium;
    if (value == "low") return ReasoningEffort::Low;
    if (value == "minimal") return ReasoningEffort::Minimal;
    if (value == "none") return ReasoningEffort::None;
    return ReasoningEffort::Auto;
}

const std::vector<ModelInfo>& knownModelInfo(Provider provider) {
    using E = ReasoningEffort;
    static const std::vector<ModelInfo> openrouter = {
        {"anthropic/claude-sonnet-4.6",
         {E::Max, E::High, E::Medium, E::Low}, E::Auto},
        {"anthropic/claude-opus-5",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low}, E::High},
        {"anthropic/claude-opus-5-fast",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low}, E::High, false},
        {"anthropic/claude-fable-5",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low}, E::High, false},
        {"~anthropic/claude-fable-latest",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low}, E::High, false},
        {"anthropic/claude-opus-4.6",
         {E::Max, E::High, E::Medium, E::Low}, E::Auto},
        {"anthropic/claude-haiku-4.5", {}, E::Auto},
        {"openai/gpt-5.6-sol",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Medium,
         false},
        {"openai/gpt-5.6-sol-pro",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Medium,
         false},
        {"openai/gpt-5.6-terra",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Medium,
         false},
        {"openai/gpt-5.6-terra-pro",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Medium,
         false},
        {"openai/gpt-5.6-luna",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Medium,
         false},
        {"openai/gpt-5.6-luna-pro",
         {E::Max, E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Medium,
         false},
        {"openai/gpt-5.4",
         {E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Auto, false},
        {"openai/gpt-5.4-mini",
         {E::XHigh, E::High, E::Medium, E::Low, E::None}, E::Auto, false},
        {"google/gemini-3.1-pro-preview",
         {E::High, E::Medium, E::Low}, E::Medium},
        {"google/gemini-3-flash-preview",
         {E::High, E::Medium, E::Low, E::Minimal}, E::Auto},
        {"deepseek/deepseek-v3.2", {}, E::Auto},
        {"qwen/qwen3-coder", {}, E::Auto},
        {"openai/gpt-oss-120b",
         {E::High, E::Medium, E::Low}, E::Medium},
    };
    static const std::vector<ModelInfo> openai = {
        {"gpt-5.4", {E::XHigh, E::High, E::Medium, E::Low, E::None},
         E::Auto, false},
        {"gpt-5.4-mini", {E::XHigh, E::High, E::Medium, E::Low, E::None},
         E::Auto, false},
        {"gpt-5.4-nano", {E::High, E::Medium, E::Low, E::Minimal, E::None},
         E::Auto, false},
    };
    static const std::vector<ModelInfo> anthropic = {
        {"claude-sonnet-4-6", {E::Max, E::High, E::Medium, E::Low},
         E::Auto, false},
        {"claude-opus-4-6", {E::Max, E::High, E::Medium, E::Low},
         E::Auto, false},
        {"claude-haiku-4-5-20251001", {}, E::Auto},
    };
    static const std::vector<ModelInfo> xai = {
        {"grok-4.3", {E::High, E::Medium, E::Low, E::None}, E::Auto, false},
        {"grok-4.1-fast-reasoning", {}, E::Auto, false},
        {"grok-4.1-fast-non-reasoning", {}, E::Auto},
        {"grok-code-fast-1", {}, E::Auto, false},
    };
    switch (provider) {
        case Provider::OpenRouter: return openrouter;
        case Provider::OpenAI:     return openai;
        case Provider::Anthropic:  return anthropic;
        case Provider::XAI:        return xai;
    }
    return openrouter;
}

const ModelInfo* findModelInfo(Provider provider, const std::string& model) {
    for (const auto& info : knownModelInfo(provider)) {
        if (info.id == model) return &info;
    }
    return nullptr;
}

std::vector<ReasoningEffort> reasoningEffortOptions(
    Provider provider, const std::string& model) {
    std::vector<ReasoningEffort> options{ReasoningEffort::Auto};
    const ModelInfo* info = findModelInfo(provider, model);
    if (info) {
        options.insert(options.end(), info->supported_efforts.begin(),
                       info->supported_efforts.end());
    }
    return options;
}

ReasoningEffort effectiveReasoningEffort(Provider provider,
                                         const std::string& model,
                                         ReasoningEffort configured) {
    const ModelInfo* info = findModelInfo(provider, model);
    if (!info || info->supported_efforts.empty()) return ReasoningEffort::Auto;
    if (configured == ReasoningEffort::Auto) return info->auto_effort;
    for (ReasoningEffort supported : info->supported_efforts) {
        if (supported == configured) return configured;
    }
    return info->auto_effort;
}

const std::vector<std::string>& knownModels(Provider provider) {
    static const auto all_models = [] {
        std::vector<std::vector<std::string>> result;
        result.reserve(knownProviders().size());
        for (const auto& provider_info : knownProviders()) {
            const auto& info = knownModelInfo(provider_info.provider);
            std::vector<std::string> ids;
            ids.reserve(info.size());
            for (const auto& model : info) ids.push_back(model.id);
            result.push_back(std::move(ids));
        }
        return result;
    }();
    for (size_t i = 0; i < knownProviders().size(); ++i) {
        if (knownProviders()[i].provider == provider) return all_models[i];
    }
    return all_models.front();
}

const std::string& defaultModel(Provider provider) {
    return knownModels(provider).front();
}

ProviderAccount& providerAccount(Config& config, Provider provider) {
    switch (provider) {
        case Provider::OpenRouter: return config.openrouter;
        case Provider::OpenAI:     return config.openai;
        case Provider::Anthropic:  return config.anthropic;
        case Provider::XAI:        return config.xai;
    }
    return config.openrouter;
}

const ProviderAccount& providerAccount(const Config& config,
                                       Provider provider) {
    switch (provider) {
        case Provider::OpenRouter: return config.openrouter;
        case Provider::OpenAI:     return config.openai;
        case Provider::Anthropic:  return config.anthropic;
        case Provider::XAI:        return config.xai;
    }
    return config.openrouter;
}

std::string& selectedModel(Config& config) {
    return providerAccount(config, config.provider).model;
}

const std::string& selectedModel(const Config& config) {
    return providerAccount(config, config.provider).model;
}

std::string effectiveApiKey(const Config& config, Provider provider) {
    auto normalize = [](const std::string& value) {
        size_t first = 0;
        while (first < value.size() &&
               std::isspace(static_cast<unsigned char>(value[first]))) {
            ++first;
        }
        size_t last = value.size();
        while (last > first &&
               std::isspace(static_cast<unsigned char>(value[last - 1]))) {
            --last;
        }
        std::string key = value.substr(first, last - first);
        if (key.starts_with("Bearer ")) key.erase(0, 7);
        return key;
    };
    const char* environment_value =
        std::getenv(providerInfo(provider).environment_variable.c_str());
    if (environment_value && *environment_value) {
        std::string key = normalize(environment_value);
        if (!key.empty()) return key;
    }
    return normalize(providerAccount(config, provider).api_key);
}

fs::path defaultConfigDir() {
    const char* home = std::getenv("HOME");
    if (home && *home) return fs::path(home) / ".ovi-code";
    return fs::current_path() / ".ovi-code";
}

fs::path defaultConfigPath() { return defaultConfigDir() / "config.json"; }

bool migrateLegacyConfig(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return false;
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    fs::path legacy = fs::path(home) / ".config" / "ovi-code" / "config.json";
    if (!fs::exists(legacy, ec)) return false;
    fs::create_directories(path.parent_path(), ec);
    fs::rename(legacy, path, ec);
    if (ec) {
        // Cross-filesystem rename can fail; fall back to copy + remove.
        ec.clear();
        fs::copy_file(legacy, path, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        fs::remove(legacy, ec);
    }
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    // Best-effort: drop the now-empty legacy directory.
    fs::remove(legacy.parent_path(), ec);
    return true;
}

std::optional<Config> loadConfig(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;
    std::ifstream f(path);
    if (!f) return std::nullopt;
    json j;
    try {
        f >> j;
    } catch (...) {
        return std::nullopt;
    }
    Config c;
    try {
        const std::string provider_id = j.value("provider", "openrouter");
        c.provider = providerFromString(provider_id);

        if (j.contains("accounts") && j["accounts"].is_object()) {
            const auto& accounts = j["accounts"];
            for (const auto& info : knownProviders()) {
                if (!accounts.contains(info.id) ||
                    !accounts[info.id].is_object()) {
                    continue;
                }
                const auto& saved = accounts[info.id];
                auto& account = providerAccount(c, info.provider);
                account.model = saved.value("model", defaultModel(info.provider));
                account.api_key = saved.value("api_key", std::string{});
            }
        } else if (provider_id == "openai" || provider_id == "anthropic" ||
                   provider_id == "xai" || provider_id == "grok") {
            // Configs from the earlier direct-provider implementation used one
            // active api_key/model pair.
            auto& account = providerAccount(c, c.provider);
            account.model = j.value("model", defaultModel(c.provider));
            account.api_key = j.value("api_key", std::string{});
        } else {
            // Migrate the OpenRouter-only format used by recent releases.
            c.provider = Provider::OpenRouter;
            c.openrouter.model =
                j.value("model", defaultModel(Provider::OpenRouter));
            c.openrouter.api_key = j.value(
                "openrouter_api_key",
                provider_id == "openrouter"
                    ? j.value("api_key", std::string{})
                    : std::string{});

            // Older unsupported direct providers were routed through
            // OpenRouter. Preserve that behavior for those persisted configs.
            if (c.openrouter.model.find('/') == std::string::npos) {
                if (provider_id == "gemini") {
                    c.openrouter.model = "google/" + c.openrouter.model;
                } else if (provider_id == "cerebras" &&
                           c.openrouter.model == "gpt-oss-120b") {
                    c.openrouter.model = "openai/" + c.openrouter.model;
                }
            }
        }

        for (const auto& info : knownProviders()) {
            auto& account = providerAccount(c, info.provider);
            if (account.model.empty()) account.model = defaultModel(info.provider);
        }
        c.technique_id = j.value("technique", std::string("standard"));
        c.technique = techniqueFromString(c.technique_id);
        c.theme = themeFromString(j.value("theme", "plain"));
        c.reasoning_effort = reasoningEffortFromString(
            j.value("reasoning_effort", std::string("auto")));
        c.max_tool_iterations = j.value("max_tool_iterations", 12);
        c.ralph_iterations = j.value("ralph_iterations", 3);
        c.plugin_iterations = j.value("plugin_iterations", 3);
        c.max_tokens = j.value("max_tokens", 4096);
        c.max_context_bytes =
            j.value("max_context_bytes", std::size_t{2 * 1024 * 1024});
        c.max_request_bytes =
            j.value("max_request_bytes", std::size_t{4 * 1024 * 1024});
        c.max_context_bytes = std::clamp<std::size_t>(
            c.max_context_bytes, 64 * 1024, 64 * 1024 * 1024);
        c.max_request_bytes = std::clamp<std::size_t>(
            c.max_request_bytes, 64 * 1024, 128 * 1024 * 1024);
        c.temperature = j.value("temperature", 0.2);
        if (j.contains("plugin_parameters") &&
            j["plugin_parameters"].is_object()) {
            for (const auto& [plugin_id, values] :
                 j["plugin_parameters"].items()) {
                if (!values.is_object()) continue;
                for (const auto& [key, value] : values.items()) {
                    if (value.is_string()) {
                        c.plugin_parameters[plugin_id][key] =
                            value.get<std::string>();
                    }
                }
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    return c;
}

bool saveConfig(const Config& cfg, const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    json j = {
        {"version", 2},
        {"provider", providerToString(cfg.provider)},
        {"accounts", {
            {"openrouter", {
                {"model", cfg.openrouter.model},
                {"api_key", cfg.openrouter.api_key},
            }},
            {"openai", {
                {"model", cfg.openai.model},
                {"api_key", cfg.openai.api_key},
            }},
            {"anthropic", {
                {"model", cfg.anthropic.model},
                {"api_key", cfg.anthropic.api_key},
            }},
            {"xai", {
                {"model", cfg.xai.model},
                {"api_key", cfg.xai.api_key},
            }},
        }},
        {"technique", cfg.technique_id.empty()
                          ? techniqueToString(cfg.technique)
                          : cfg.technique_id},
        {"theme", themeToString(cfg.theme)},
        {"reasoning_effort", reasoningEffortToString(cfg.reasoning_effort)},
        {"max_tool_iterations", cfg.max_tool_iterations},
        {"ralph_iterations", cfg.ralph_iterations},
        {"plugin_iterations", cfg.plugin_iterations},
        {"max_tokens", cfg.max_tokens},
        {"max_context_bytes", cfg.max_context_bytes},
        {"max_request_bytes", cfg.max_request_bytes},
        {"temperature", cfg.temperature},
        {"plugin_parameters", cfg.plugin_parameters},
    };
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2);
    f.close();
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    return true;
}

}  // namespace ovicode
