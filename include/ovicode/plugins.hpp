#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ovicode/technique_plugin.h"

namespace ovicode {

enum class PluginParameterType {
    String,
    Secret,
    Integer,
    Number,
    Boolean,
    Choice,
};

struct PluginParameter {
    std::string key;
    std::string label;
    std::string description;
    PluginParameterType type = PluginParameterType::String;
    std::string default_value;
    std::vector<std::string> options;
};

// Returns a normalized value, or nullopt when `value` is invalid for the
// parameter's declared type.
std::optional<std::string> normalizePluginParameterValue(
    const PluginParameter& parameter, const std::string& value);

struct LoadedTechniquePlugin {
    std::string id;
    std::string label;
    std::string description;
    std::vector<PluginParameter> parameters;

    int run(const OviCodeTechniqueHostV1& host_v1,
            const OviCodeTechniqueHostV2& host_v2,
            const std::string& user_input) const;

  private:
    friend class TechniquePluginRegistry;
    const OviCodeTechniquePluginV1* descriptor_v1 = nullptr;
    const OviCodeTechniquePluginV2* descriptor_v2 = nullptr;
    void* state = nullptr;
    void* library_handle = nullptr;
};

class TechniquePluginRegistry {
  public:
    TechniquePluginRegistry();
    ~TechniquePluginRegistry();
    TechniquePluginRegistry(const TechniquePluginRegistry&) = delete;
    TechniquePluginRegistry& operator=(const TechniquePluginRegistry&) = delete;

    void loadDirectory(const std::filesystem::path& directory);
    const LoadedTechniquePlugin* find(const std::string& id) const;
    const std::vector<std::unique_ptr<LoadedTechniquePlugin>>& plugins() const;
    const std::vector<std::string>& errors() const;

  private:
    std::vector<std::unique_ptr<LoadedTechniquePlugin>> plugins_;
    std::vector<std::string> errors_;
};

}  // namespace ovicode
