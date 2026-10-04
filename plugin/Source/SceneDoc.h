// The scene document as the plugin keeps it: the editor's JSON (the engine's
// scene format plus editor-only keys), with each layer bound to a host track
// by "host_id", the id of the layer instance on that track.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "SharedSession.h"
#include "sp/Scene.h"

namespace spplug::doc {

using json = nlohmann::json;

json defaultScene();
json defaultLayer(int index, const std::string& name, const std::array<float, 3>& position);

// Gives every live layer instance a layer in the document: new tracks get a
// new layer (placed on a ring around the listener's start, or on top of the
// layer they were duplicated from), and bound layers follow their track's
// name. Never removes layers. Returns true when the document changed.
bool reconcile(json& doc, const std::vector<SharedSession::LayerInfo>& live);

// What the engine plays: solo turns into mute for the others.
json engineView(const json& doc);

int layerIndex(const json& doc, const std::string& hostId);

// The scene one layer instance renders: the layer bound to `hostId` (or a
// stand-in in front of the listener when there is none yet), the room, the
// listener and the environment. Throws on malformed documents.
sp::Scene layerScene(const json& doc, const std::string& hostId, bool* found = nullptr);

}  // namespace spplug::doc
