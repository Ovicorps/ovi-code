#include "ovicode/plugins.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <set>
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

std::optional<PluginParameterType> parameterType(
    OviCodePluginParameterTypeV2 type) {
    switch (type) {
        case OVI_CODE_PLUGIN_PARAMETER_STRING:
            return PluginParameterType::String;
        case OVI_CODE_PLUGIN_PARAMETER_SECRET:
            return PluginParameterType::Secret;
        case OVI_CODE_PLUGIN_PARAMETER_INTEGER:
            return PluginParameterType::Integer;
        case OVI_CODE_PLUGIN_PARAMETER_NUMBER:
            return PluginParameterType::Number;
        case OVI_CODE_PLUGIN_PARAMETER_BOOLEAN:
            return PluginParameterType::Boolean;
        case OVI_CODE_PLUGIN_PARAMETER_CHOICE:
            return PluginParameterType::Choice;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> normalizePluginParameterValue(
    const PluginParameter& parameter, const std::string& value) {
    switch (parameter.type) {
        case PluginParameterType::String:
        case PluginParameterType::Secret:
            return value;
        case PluginParameterType::Integer: {
            long long parsed = 0;
            const char* begin = value.data();
            const char* end = begin + value.size();
            const auto result = std::from_chars(begin, end, parsed);
            if (value.empty() || result.ec != std::errc{} || result.ptr != end)
                return std::nullopt;
            return value;
        }
        case PluginParameterType::Number: {
            if (value.empty()) return std::nullopt;
            char* end = nullptr;
            errno = 0;
            const double parsed = std::strtod(value.c_str(), &end);
            if (errno == ERANGE || end != value.c_str() + value.size() ||
                !std::isfinite(parsed)) {
                return std::nullopt;
            }
            return value;
        }
        case PluginParameterType::Boolean: {
            std::string normalized;
            normalized.reserve(value.size());
            for (unsigned char c : value)
                normalized.push_back(static_cast<char>(std::tolower(c)));
            if (normalized == "true" || normalized == "1" ||
                normalized == "yes" || normalized == "on") {
                return std::string("true");
            }
            if (normalized == "false" || normalized == "0" ||
                normalized == "no" || normalized == "off") {
                return std::string("false");
            }
            return std::nullopt;
        }
        case PluginParameterType::Choice:
            if (std::find(parameter.options.begin(), parameter.options.end(),
                          value) != parameter.options.end()) {
                return value;
            }
            return std::nullopt;
    }
    return std::nullopt;
}

int LoadedTechniquePlugin::run(const OviCodeTechniqueHostV1& host_v1,
                               const OviCodeTechniqueHostV2& host_v2,
                               const std::string& user_input) const {
    try {
        if (descriptor_v2 && descriptor_v2->run_turn)
            return descriptor_v2->run_turn(state, &host_v2,
                                           user_input.c_str());
        if (descriptor_v1 && descriptor_v1->run_turn)
            return descriptor_v1->run_turn(state, &host_v1,
                                           user_input.c_str());
    } catch (...) {
    }
    return 0;
}

TechniquePluginRegistry::TechniquePluginRegistry() = default;

TechniquePluginRegistry::~TechniquePluginRegistry() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        auto& plugin = **it;
        auto destroy = plugin.descriptor_v2 ? plugin.descriptor_v2->destroy
                                            : plugin.descriptor_v1
                                                  ? plugin.descriptor_v1->destroy
                                                  : nullptr;
        if (destroy) {
            try {
                destroy(plugin.state);
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

        const OviCodeTechniquePluginV1* descriptor_v1 = nullptr;
        const OviCodeTechniquePluginV2* descriptor_v2 = nullptr;
        dlerror();
        auto get_plugin_v2 = reinterpret_cast<OviCodeGetTechniquePluginV2>(
            dlsym(handle, OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL_V2));
        const char* v2_symbol_error = dlerror();
        if (!v2_symbol_error && get_plugin_v2) {
            try {
                descriptor_v2 = get_plugin_v2();
            } catch (...) {
            }
            if (!descriptor_v2 ||
                descriptor_v2->api_version !=
                    OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION_V2 ||
                !descriptor_v2->id || !descriptor_v2->label ||
                !descriptor_v2->description || !descriptor_v2->run_turn ||
                descriptor_v2->parameter_count > 256 ||
                (descriptor_v2->parameter_count > 0 &&
                 !descriptor_v2->parameters)) {
                errors_.push_back("Invalid V2 technique plugin descriptor in " +
                                  path.string());
                dlclose(handle);
                continue;
            }
        } else {
            dlerror();
            auto get_plugin_v1 = reinterpret_cast<OviCodeGetTechniquePluginV1>(
                dlsym(handle, OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL));
            const char* v1_symbol_error = dlerror();
            if (v1_symbol_error || !get_plugin_v1) {
                errors_.push_back(
                    "Missing " +
                    std::string(OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL_V2) + " or " +
                    OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL + " in " + path.string());
                dlclose(handle);
                continue;
            }
            try {
                descriptor_v1 = get_plugin_v1();
            } catch (...) {
            }
            if (!descriptor_v1 ||
                descriptor_v1->api_version !=
                    OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION ||
                !descriptor_v1->id || !descriptor_v1->label ||
                !descriptor_v1->description || !descriptor_v1->run_turn) {
                errors_.push_back("Invalid V1 technique plugin descriptor in " +
                                  path.string());
                dlclose(handle);
                continue;
            }
        }

        std::string id = descriptor_v2 ? descriptor_v2->id : descriptor_v1->id;
        if (!validPluginId(id) || isBuiltInId(id) || find(id)) {
            errors_.push_back("Invalid or duplicate technique plugin id '" + id +
                              "' in " + path.string());
            dlclose(handle);
            continue;
        }

        auto plugin = std::make_unique<LoadedTechniquePlugin>();
        plugin->id = id;
        plugin->label = descriptor_v2 ? descriptor_v2->label
                                      : descriptor_v1->label;
        plugin->description = descriptor_v2 ? descriptor_v2->description
                                            : descriptor_v1->description;
        plugin->descriptor_v1 = descriptor_v1;
        plugin->descriptor_v2 = descriptor_v2;

        bool parameters_valid = true;
        std::set<std::string> parameter_keys;
        if (descriptor_v2) {
            for (uint32_t i = 0; i < descriptor_v2->parameter_count; ++i) {
                const auto& raw = descriptor_v2->parameters[i];
                const auto type = parameterType(raw.type);
                if (!raw.key || !raw.label || !raw.description || !type ||
                    !validPluginId(raw.key) ||
                    !parameter_keys.insert(raw.key).second ||
                    (raw.type == OVI_CODE_PLUGIN_PARAMETER_CHOICE &&
                     (!raw.options || raw.option_count == 0 ||
                      raw.option_count > 256))) {
                    parameters_valid = false;
                    break;
                }
                PluginParameter parameter;
                parameter.key = raw.key;
                parameter.label = raw.label;
                parameter.description = raw.description;
                parameter.type = *type;
                parameter.default_value = raw.default_value
                                              ? raw.default_value
                                              : std::string{};
                if (raw.type == OVI_CODE_PLUGIN_PARAMETER_CHOICE) {
                    std::set<std::string> option_values;
                    for (uint32_t option = 0; option < raw.option_count;
                         ++option) {
                        if (!raw.options[option] ||
                            !option_values.insert(raw.options[option]).second) {
                            parameters_valid = false;
                            break;
                        }
                        parameter.options.emplace_back(raw.options[option]);
                    }
                }
                if (!parameters_valid ||
                    !normalizePluginParameterValue(parameter,
                                                   parameter.default_value)) {
                    parameters_valid = false;
                    break;
                }
                plugin->parameters.push_back(std::move(parameter));
            }
        }
        if (!parameters_valid) {
            errors_.push_back("Invalid parameter descriptor in plugin '" + id +
                              "' from " + path.string());
            dlclose(handle);
            continue;
        }

        void* state = nullptr;
        auto create = descriptor_v2 ? descriptor_v2->create
                                    : descriptor_v1->create;
        if (create) {
            try {
                state = create();
            } catch (...) {
                errors_.push_back("Technique plugin create failed for '" + id +
                                  "'");
                dlclose(handle);
                continue;
            }
        }

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
