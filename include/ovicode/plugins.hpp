#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ovicode/technique_plugin.h"

namespace ovicode {

struct LoadedTechniquePlugin {
    std::string id;
    std::string label;
    std::string description;

    int run(const OviCodeTechniqueHostV1& host,
            const std::string& user_input) const;

  private:
    friend class TechniquePluginRegistry;
    const OviCodeTechniquePluginV1* descriptor = nullptr;
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
