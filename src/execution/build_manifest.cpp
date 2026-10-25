#include "codestudio/build_manifest.hpp"
#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace codestudio {
namespace {
void fields(const Json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("Build manifest must contain objects");
    for (const auto& [name, unused] : object.items())
        if (std::ranges::find(allowed, name) == allowed.end())
            throw std::invalid_argument("Unknown build manifest field: " + name);
}
}

Json normalize_build_manifest(const Json& manifest) {
    fields(manifest, {"version", "inputs", "exclude", "mounts", "steps", "environment", "outputs",
                      "artifact_directory", "limits"});
    if (!manifest.contains("version") || !manifest["version"].is_number_integer() || manifest["version"] != 2)
        throw std::invalid_argument("Unsupported build manifest version");
    if (!manifest.contains("steps"))
        throw std::invalid_argument("Build manifest requires explicit steps");
    return manifest;
}
}
