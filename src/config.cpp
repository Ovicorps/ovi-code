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

const std::vector<std::string>& knownModels() {
    static const std::vector<std::string> models = {
        "anthropic/claude-sonnet-4.6",
        "anthropic/claude-opus-4.6",
        "anthropic/claude-haiku-4.5",
        "openai/gpt-5.4",
        "openai/gpt-5.4-mini",
        "google/gemini-3.1-pro-preview",
        "google/gemini-3-flash-preview",
        "deepseek/deepseek-v3.2",
        "qwen/qwen3-coder",
        "openai/gpt-oss-120b",
    };
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
    c.technique = techniqueFromString(j.value("technique", "standard"));
    c.theme = themeFromString(j.value("theme", "plain"));
    c.max_tool_iterations = j.value("max_tool_iterations", 12);
    c.ralph_iterations = j.value("ralph_iterations", 3);
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
        {"technique", techniqueToString(cfg.technique)},
        {"theme", themeToString(cfg.theme)},
        {"max_tool_iterations", cfg.max_tool_iterations},
        {"ralph_iterations", cfg.ralph_iterations},
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
