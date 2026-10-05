#include "SceneDoc.h"

#include <cmath>

#include "sp/SceneJson.h"

namespace spplug::doc {

namespace {
const char* kPalette[] = {"#4f9cf9", "#f97f4f", "#5fd38d", "#e05fd3", "#f2c94c", "#56ccf2", "#eb5757", "#9b8cff",
                          "#6fcf97", "#f2994a", "#bb6bd9", "#2fd1c5"};

double round2(double v) { return std::round(v * 100.0) / 100.0; }
}  // namespace

// Same defaults as the editor's defaultScene() (ui/src/model/scene.ts).
json defaultScene() {
    const auto mat = [](const char* n) { return json{{"name", n}}; };
    return json{
        {"name", "Logic session"},
        {"duration", 0},
        {"layers", json::array()},
        {"room",
         {{"type", "outdoor"},  // outdoors (ground only); the box is ready for "room (box)"
          {"size", {12, 3.5, 16}},
          {"origin", {0, 0, 0}},
          {"materials",
           {{"left", mat("plaster")}, {"right", mat("plaster")}, {"floor", mat("wood_floor")}, {"ceiling", mat("plaster")},
            {"front", mat("plaster")}, {"back", mat("plaster")}}},
          {"reflection_order", 2},
          {"reflections_level_db", 0},
          {"reverb_level_db", 0},
          {"reverb_time_scale", 1},
          {"reflections", true},
          {"reverb", true}}},
        {"listener",
         {{"paths", json::array()},
          {"active_path", 0},
          {"position_mode", "speed"},
          {"speed", json::array({json{{"time", 0}, {"speed", 1.4}, {"easing", "linear"}}})},
          {"path_start_time", 0},
          {"path_fraction", 0},
          {"loop_path", false},
          {"static_position", {0, 1.7, 0}},
          {"head_radius", 0.0875},
          {"head",
           {{"mode", "along_path"}, {"banking", false}, {"look_at_point", {0, 1.6, 0}}, {"look_at_layer", -1},
            {"keys", json::array()}, {"yaw_offset", 0}, {"pitch_offset", 0}, {"roll_offset", 0}}}}},
        {"environment",
         {{"speed_of_sound", 343}, {"temperature_c", 20}, {"humidity", 50}, {"pressure_kpa", 101.325}, {"air_absorption", true}}},
        {"editor", {{"draw_height", 1.7}}},
    };
}

json defaultLayer(int index, const std::string& name, const std::array<float, 3>& p) {
    return json{{"name", name.empty() ? "Layer " + std::to_string(index + 1) : name},
                {"audio", ""},
                {"position", {p[0], p[1], p[2]}},
                {"level_db", 0},
                {"mute", false},
                {"doppler", 1},
                {"spread_deg", 0},
                {"directivity", 0},
                {"directivity_forward", {0, 0, 1}},
                {"reference_distance", 1},
                {"min_distance", 0.25},
                {"rolloff", 1},
                {"reverb_send_db", 0},
                {"reflection_order", -1},
                {"start_time", 0},
                {"loop", false},
                {"color", kPalette[index % 12]}};
}

int layerIndex(const json& doc, const std::string& hostId) {
    if (hostId.empty() || !doc.contains("layers") || !doc["layers"].is_array()) return -1;
    const auto& layers = doc["layers"];
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].is_object() && layers[i].value("host_id", std::string()) == hostId) return static_cast<int>(i);
    return -1;
}

// A layer whose channels come from a file the plugin plays itself (an
// Ambisonic recording of a higher order than the track carries) keeps its
// own channel count; every other layer has the track's.
bool fileFed(const json& l) {
    if (sp::ambisonicOrder(l.value("channels", 1)) < 1) return false;
    if (!l.value("audio", std::string()).empty()) return true;
    return l.contains("audio_files") && l["audio_files"].is_array() && !l["audio_files"].empty();
}

// Gives an Ambisonic layer (4, 9 or 16 channels) its sphere settings.
bool completeAmbisonic(json& l) {
    if (sp::ambisonicOrder(l.value("channels", 1)) < 1 || l.contains("ambisonic")) return false;
    l["ambisonic"] = json{{"format", "ambix"}, {"radius", 3.0}, {"yaw", 0.0}, {"pitch", 0.0}, {"roll", 0.0}, {"room_send", false}};
    return true;
}

bool reconcile(json& doc, const std::vector<SharedSession::LayerInfo>& live) {
    if (!doc.contains("layers") || !doc["layers"].is_array()) doc["layers"] = json::array();
    bool changed = false;
    for (const auto& info : live) {
        const int i = layerIndex(doc, info.id);
        if (i >= 0) {
            auto& l = doc["layers"][static_cast<size_t>(i)];
            if (!info.name.empty() && l.value("name", std::string()) != info.name) {
                l["name"] = info.name;
                changed = true;
            }
            // A stereo track plays its layer as a left/right pair, a quad
            // track as a first-order Ambisonic sphere.
            if (!fileFed(l) && l.value("channels", 1) != info.channels) {
                l["channels"] = info.channels;
                changed = true;
            }
            if (completeAmbisonic(l)) changed = true;
            continue;
        }
        auto& layers = doc["layers"];
        const int n = static_cast<int>(layers.size());
        json layer;
        const int src = layerIndex(doc, info.cloneOf);
        if (src >= 0) {
            // A duplicated track: the copy starts where the original is.
            layer = layers[static_cast<size_t>(src)];
            layer["color"] = kPalette[n % 12];
        } else {
            // On a ring around the listener's start, as the editor places new
            // files (golden angle, so any number of layers spread evenly).
            std::array<double, 3> c{0, 1.7, 0};
            try {
                const auto& L = doc.at("listener");
                const int ap = L.value("active_path", 0);
                const auto& paths = L.at("paths");
                if (ap >= 0 && ap < static_cast<int>(paths.size()) && !paths[static_cast<size_t>(ap)]["segments"].empty())
                    c = paths[static_cast<size_t>(ap)]["segments"][0]["points"][0].get<std::array<double, 3>>();
                else
                    c = L.at("static_position").get<std::array<double, 3>>();
            } catch (...) {
            }
            const double a = std::fmod(n * 2.399963, 2 * M_PI);
            const double r = 3 + 0.4 * (n / 6);
            layer = defaultLayer(n, info.name, {static_cast<float>(round2(c[0] - r * std::sin(a))), 1.6f,
                                                static_cast<float>(round2(c[2] - r * std::cos(a)))});
        }
        layer["host_id"] = info.id;
        if (!info.name.empty()) layer["name"] = info.name;
        if (!fileFed(layer)) layer["channels"] = info.channels;
        completeAmbisonic(layer);
        layers.push_back(layer);
        changed = true;
    }
    return changed;
}

json engineView(const json& doc) {
    json d = doc;
    if (!d.contains("layers") || !d["layers"].is_array()) return d;
    bool anySolo = false;
    for (const auto& l : d["layers"]) anySolo = anySolo || (l.is_object() && l.value("solo", false));
    if (anySolo)
        for (auto& l : d["layers"])
            if (l.is_object() && !l.value("solo", false)) l["mute"] = true;
    return d;
}

sp::Scene layerScene(const json& docIn, const std::string& hostId, bool* found) {
    json d = engineView(docIn);
    const int i = layerIndex(d, hostId);
    if (found) *found = i >= 0;
    json layer = i >= 0 ? d["layers"][static_cast<size_t>(i)] : defaultLayer(0, "", {0, 1.6f, -2});
    // "Look at a layer" names an index into the full list: turn it into a point.
    if (d.contains("listener") && d["listener"].contains("head")) {
        auto& h = d["listener"]["head"];
        const int la = h.value("look_at_layer", -1);
        if (la >= 0 && la < static_cast<int>(d["layers"].size())) {
            h["look_at_point"] = d["layers"][static_cast<size_t>(la)].value("position", json::array({0, 1.6, 0}));
        }
        h["look_at_layer"] = -1;
    }
    d["layers"] = json::array({layer});
    return sp::sceneFromJson(d.dump());
}

}  // namespace spplug::doc
