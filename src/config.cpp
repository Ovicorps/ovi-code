#include "ovicode/config.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace ovicode {

namespace fs = std::filesystem;
using json = nlohmann::json;

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

const std::vector<ModelInfo>& knownModelInfo() {
    using E = ReasoningEffort;
    static const std::vector<ModelInfo> models = {
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
    return models;
}

const ModelInfo* findModelInfo(const std::string& model) {
    for (const auto& info : knownModelInfo()) {
        if (info.id == model) return &info;
    }
    return nullptr;
}

std::vector<ReasoningEffort> reasoningEffortOptions(const std::string& model) {
    std::vector<ReasoningEffort> options{ReasoningEffort::Auto};
    const ModelInfo* info = findModelInfo(model);
    if (info) {
        options.insert(options.end(), info->supported_efforts.begin(),
                       info->supported_efforts.end());
    }
    return options;
}

ReasoningEffort effectiveReasoningEffort(const std::string& model,
                                         ReasoningEffort configured) {
    const ModelInfo* info = findModelInfo(model);
    if (!info || info->supported_efforts.empty()) return ReasoningEffort::Auto;
    if (configured == ReasoningEffort::Auto) return info->auto_effort;
    for (ReasoningEffort supported : info->supported_efforts) {
        if (supported == configured) return configured;
    }
    return info->auto_effort;
}

const std::vector<std::string>& knownModels() {
    static const std::vector<std::string> models = [] {
        std::vector<std::string> ids;
        ids.reserve(knownModelInfo().size());
        for (const auto& info : knownModelInfo()) ids.push_back(info.id);
        return ids;
    }();
    return models;
}

const std::string& defaultModel() { return knownModels().front(); }

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
    c.model = j.value("model", defaultModel());
    c.openrouter_api_key = j.value("openrouter_api_key", std::string{});

    // Older configs stored provider-native model names. Prefix the known ones
    // so they remain usable through OpenRouter after migration.
    if (c.model.find('/') == std::string::npos) {
        const std::string provider = j.value("provider", "");
        if (provider == "anthropic") c.model = "anthropic/" + c.model;
        else if (provider == "openai") c.model = "openai/" + c.model;
        else if (provider == "gemini") c.model = "google/" + c.model;
        else if (provider == "cerebras" && c.model == "gpt-oss-120b")
            c.model = "openai/" + c.model;
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
    c.temperature = j.value("temperature", 0.2);
    return c;
}

bool saveConfig(const Config& cfg, const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    json j = {
        {"model", cfg.model},
        {"openrouter_api_key", cfg.openrouter_api_key},
        {"technique", cfg.technique_id.empty()
                          ? techniqueToString(cfg.technique)
                          : cfg.technique_id},
        {"theme", themeToString(cfg.theme)},
        {"reasoning_effort", reasoningEffortToString(cfg.reasoning_effort)},
        {"max_tool_iterations", cfg.max_tool_iterations},
        {"ralph_iterations", cfg.ralph_iterations},
        {"plugin_iterations", cfg.plugin_iterations},
        {"max_tokens", cfg.max_tokens},
        {"temperature", cfg.temperature},
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
