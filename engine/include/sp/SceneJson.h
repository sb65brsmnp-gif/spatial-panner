// Scene <-> JSON. The same files are shared between the standalone app, the
// plugin and the command-line renderer.
#pragma once

#include <string>

#include "sp/Scene.h"

namespace sp {

// Throws std::runtime_error with a readable message on malformed input.
// `baseDir` resolves relative file references inside the scene (a Mesh
// room's OBJ file); loadSceneFile passes the scene file's directory.
Scene sceneFromJson(const std::string& jsonText, const std::string& baseDir = "");
std::string sceneToJson(const Scene& scene, int indent = 2);

Scene loadSceneFile(const std::string& path);
void saveSceneFile(const Scene& scene, const std::string& path);

}  // namespace sp
