#include "ovicode/plugins.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif

#include "ovicode/config.hpp"

namespace ovicode {

namespace fs = std::filesystem;

namespace {

bool validPluginId(const std::string& id) {
    if (id.empty()) return false;
    for (unsigned char c : id) {
        if (!std::islower(c) && !std::isdigit(c) && c != '-' && c != '_' &&
            c != '.') {
            return false;
        }
    }
    return std::isalnum(static_cast<unsigned char>(id.front())) != 0;
}

bool isSharedLibrary(const fs::path& path) {
#if defined(__APPLE__)
    return path.extension() == ".dylib" || path.extension() == ".so";
#elif defined(__linux__)
    return path.extension() == ".so";
#else
    (void)path;
    return false;
#endif
}

bool isBuiltInId(const std::string& id) {
    for (Technique technique : allTechniques()) {
        if (techniqueToString(technique) == id) return true;
    }
    return false;
}

}  // namespace

int LoadedTechniquePlugin::run(const OviCodeTechniqueHostV1& host,
                               const std::string& user_input) const {
    if (!descriptor || !descriptor->run_turn) return 0;
    try {
        return descriptor->run_turn(state, &host, user_input.c_str());
    } catch (...) {
        return 0;
    }
}

TechniquePluginRegistry::TechniquePluginRegistry() = default;

TechniquePluginRegistry::~TechniquePluginRegistry() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        auto& plugin = **it;
        if (plugin.descriptor && plugin.descriptor->destroy) {
            try {
                plugin.descriptor->destroy(plugin.state);
            } catch (...) {
            }
        }
#if defined(__APPLE__) || defined(__linux__)
        if (plugin.library_handle) dlclose(plugin.library_handle);
#endif
    }
}

void TechniquePluginRegistry::loadDirectory(const fs::path& directory) {
    std::error_code ec;
    if (!fs::exists(directory, ec)) return;
    if (ec || !fs::is_directory(directory, ec)) {
        errors_.push_back("Plugin path is not a directory: " +
                          directory.string());
        return;
    }

    std::vector<fs::path> candidates;
    for (fs::directory_iterator it(directory, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_regular_file(ec) && isSharedLibrary(it->path())) {
            candidates.push_back(it->path());
        }
    }
    if (ec) {
        errors_.push_back("Cannot read plugin directory " + directory.string() +
                          ": " + ec.message());
        return;
    }
    std::sort(candidates.begin(), candidates.end());

#if defined(__APPLE__) || defined(__linux__)
    for (const auto& path : candidates) {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char* load_error = dlerror();
            errors_.push_back("Cannot load " + path.string() + ": " +
                              (load_error ? load_error : "unknown error"));
            continue;
        }

        dlerror();
        auto get_plugin = reinterpret_cast<OviCodeGetTechniquePluginV1>(
            dlsym(handle, OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL));
        const char* symbol_error = dlerror();
        if (symbol_error || !get_plugin) {
            errors_.push_back("Missing " +
                              std::string(OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL) +
                              " in " + path.string());
            dlclose(handle);
            continue;
        }

        const OviCodeTechniquePluginV1* descriptor = nullptr;
        try {
            descriptor = get_plugin();
        } catch (...) {
        }
        if (!descriptor ||
            descriptor->api_version != OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION ||
            !descriptor->id || !descriptor->label || !descriptor->description ||
            !descriptor->run_turn) {
            errors_.push_back("Invalid technique plugin descriptor in " +
                              path.string());
            dlclose(handle);
            continue;
        }

        std::string id = descriptor->id;
        if (!validPluginId(id) || isBuiltInId(id) || find(id)) {
            errors_.push_back("Invalid or duplicate technique plugin id '" + id +
                              "' in " + path.string());
            dlclose(handle);
            continue;
        }

        void* state = nullptr;
        if (descriptor->create) {
            try {
                state = descriptor->create();
            } catch (...) {
                errors_.push_back("Technique plugin create failed for '" + id +
                                  "'");
                dlclose(handle);
                continue;
            }
        }

        auto plugin = std::make_unique<LoadedTechniquePlugin>();
        plugin->id = std::move(id);
        plugin->label = descriptor->label;
        plugin->description = descriptor->description;
        plugin->descriptor = descriptor;
        plugin->state = state;
        plugin->library_handle = handle;
        plugins_.push_back(std::move(plugin));
    }
#else
    if (!candidates.empty()) {
        errors_.push_back("Runtime technique plugins are unsupported on this "
                          "platform");
    }
#endif
}

const LoadedTechniquePlugin* TechniquePluginRegistry::find(
    const std::string& id) const {
    for (const auto& plugin : plugins_) {
        if (plugin->id == id) return plugin.get();
    }
    return nullptr;
}

const std::vector<std::unique_ptr<LoadedTechniquePlugin>>&
TechniquePluginRegistry::plugins() const {
    return plugins_;
}

const std::vector<std::string>& TechniquePluginRegistry::errors() const {
    return errors_;
}

}  // namespace ovicode
