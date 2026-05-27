#include "ovicode/config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

namespace ovicode {

namespace fs = std::filesystem;
using json = nlohmann::json;

const std::vector<ProviderInfo>& knownProviders() {
    static const std::vector<ProviderInfo> v = {
        {Provider::Anthropic, "Anthropic (Claude)", "claude-sonnet-4-6",
         "https://api.anthropic.com", true},
        {Provider::OpenAI, "OpenAI", "gpt-4o", "https://api.openai.com", true},
        {Provider::Gemini, "Google Gemini", "gemini-2.0-flash",
         "https://generativelanguage.googleapis.com", true},
        {Provider::Ollama, "Local (Ollama)", "llama3.1",
         "http://localhost:11434", false},
    };
    return v;
}

const ProviderInfo& providerInfo(Provider p) {
    for (const auto& info : knownProviders()) {
        if (info.provider == p) return info;
    }
    throw std::runtime_error("unknown provider");
}

std::string providerLabel(Provider p) { return providerInfo(p).label; }

std::string providerToString(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "anthropic";
        case Provider::OpenAI:    return "openai";
        case Provider::Gemini:    return "gemini";
        case Provider::Ollama:    return "ollama";
    }
    return "anthropic";
}

Provider providerFromString(const std::string& s) {
    if (s == "anthropic") return Provider::Anthropic;
    if (s == "openai")    return Provider::OpenAI;
    if (s == "gemini")    return Provider::Gemini;
    if (s == "ollama")    return Provider::Ollama;
    return Provider::Anthropic;
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

const std::vector<std::string>& knownModels(Provider p) {
    static const std::vector<std::string> anthropic = {
        "claude-opus-4-7",
        "claude-sonnet-4-6",
        "claude-haiku-4-5-20251001",
        "claude-3-5-sonnet-latest",
    };
    static const std::vector<std::string> openai = {
        "gpt-4o",
        "gpt-4o-mini",
        "gpt-4.1",
        "gpt-4.1-mini",
        "o4-mini",
    };
    static const std::vector<std::string> gemini = {
        "gemini-2.0-flash",
        "gemini-2.0-pro",
        "gemini-1.5-pro",
        "gemini-1.5-flash",
    };
    static const std::vector<std::string> ollama = {
        "llama3.1",
        "llama3.2",
        "qwen2.5-coder",
        "deepseek-coder-v2",
        "mistral",
    };
    switch (p) {
        case Provider::Anthropic: return anthropic;
        case Provider::OpenAI:    return openai;
        case Provider::Gemini:    return gemini;
        case Provider::Ollama:    return ollama;
    }
    return anthropic;
}

fs::path defaultConfigDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "ovi-code";
    const char* home = std::getenv("HOME");
    if (home && *home) return fs::path(home) / ".config" / "ovi-code";
    return fs::current_path() / ".ovi-code";
}

fs::path defaultConfigPath() { return defaultConfigDir() / "config.json"; }

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
    c.provider = providerFromString(j.value("provider", "anthropic"));
    c.model = j.value("model", providerInfo(c.provider).default_model);
    c.api_key = j.value("api_key", "");
    c.base_url = j.value("base_url", providerInfo(c.provider).default_base_url);
    c.technique = techniqueFromString(j.value("technique", "standard"));
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
        {"provider", providerToString(cfg.provider)},
        {"model", cfg.model},
        {"api_key", cfg.api_key},
        {"base_url", cfg.base_url},
        {"technique", techniqueToString(cfg.technique)},
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
