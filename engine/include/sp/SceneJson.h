// Scene <-> JSON. The same files are shared between the standalone app, the
// plugin and the command-line renderer.
#pragma once

#include <string>

#include "sp/Scene.h"

namespace sp {

// Throws std::runtime_error with a readable message on malformed input.
Scene sceneFromJson(const std::string& jsonText);
std::string sceneToJson(const Scene& scene, int indent = 2);

Scene loadSceneFile(const std::string& path);
void saveSceneFile(const Scene& scene, const std::string& path);

}  // namespace sp
