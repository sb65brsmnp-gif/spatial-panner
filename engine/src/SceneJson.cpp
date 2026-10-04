#include "sp/SceneJson.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sp {

using nlohmann::json;

namespace {

// ---- enums as strings

template <typename E>
struct EnumNames;

#define SP_ENUM(E, ...)                                                     \
    template <> struct EnumNames<E> {                                      \
        static const std::vector<std::pair<E, const char*>>& names() {      \
            static const std::vector<std::pair<E, const char*>> n = {__VA_ARGS__}; \
            return n;                                                       \
        }                                                                   \
    };

SP_ENUM(RoomType, {RoomType::None, "none"}, {RoomType::Box, "box"}, {RoomType::Outdoor, "outdoor"}, {RoomType::Mesh, "mesh"})
SP_ENUM(SegmentType, {SegmentType::Line, "line"}, {SegmentType::CubicBezier, "bezier"},
        {SegmentType::CatmullRom, "catmull_rom"}, {SegmentType::Arc, "arc"})
SP_ENUM(Easing, {Easing::Linear, "linear"}, {Easing::SmoothStep, "smooth"}, {Easing::EaseIn, "ease_in"},
        {Easing::EaseOut, "ease_out"}, {Easing::Hold, "hold"})
SP_ENUM(HeadMode, {HeadMode::AlongPath, "along_path"}, {HeadMode::LookAt, "look_at"}, {HeadMode::Keyframed, "keyframed"})
SP_ENUM(PositionMode, {PositionMode::Speed, "speed"}, {PositionMode::AlongPath, "along_path"})

template <typename E>
std::string enumToString(E e) {
    for (const auto& kv : EnumNames<E>::names())
        if (kv.first == e) return kv.second;
    return EnumNames<E>::names().front().second;
}

template <typename E>
E enumFromString(const std::string& s, const char* field) {
    for (const auto& kv : EnumNames<E>::names())
        if (s == kv.second) return kv.first;
    std::string valid;
    for (const auto& kv : EnumNames<E>::names()) valid += std::string(valid.empty() ? "" : ", ") + kv.second;
    throw std::runtime_error("Unknown value '" + s + "' for " + field + " (expected one of: " + valid + ")");
}

template <typename E>
E getEnum(const json& j, const char* key, E def) {
    if (!j.contains(key)) return def;
    return enumFromString<E>(j.at(key).get<std::string>(), key);
}

// ---- vectors

json vecToJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

Vec3 vecFromJson(const json& j, const char* field) {
    if (j.is_array() && j.size() == 3) return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    if (j.is_object()) return {j.value("x", 0.0f), j.value("y", 0.0f), j.value("z", 0.0f)};
    throw std::runtime_error(std::string("Expected [x, y, z] for ") + field);
}

Vec3 getVec(const json& j, const char* key, const Vec3& def) {
    return j.contains(key) ? vecFromJson(j.at(key), key) : def;
}

// ---- materials

json materialToJson(const Material& m) {
    json j;
    j["name"] = m.name;
    j["absorption"] = m.absorption;
    j["scattering"] = m.scattering;
    j["transmission"] = m.transmission;
    return j;
}

Material materialFromJson(const json& j) {
    if (j.is_string()) return materials::byName(j.get<std::string>());
    Material m = materials::byName(j.value("name", "plaster"));
    if (j.contains("absorption")) {
        const auto& a = j.at("absorption");
        if (!a.is_array() || a.size() != kNumBands)
            throw std::runtime_error("Material absorption needs 6 values (125, 250, 500, 1k, 2k, 4k Hz)");
        for (int i = 0; i < kNumBands; ++i) m.absorption[i] = a[i].get<float>();
        if (j.contains("name")) m.name = j.at("name").get<std::string>(); else m.name = "custom";
    }
    m.scattering = j.value("scattering", m.scattering);
    if (j.contains("transmission")) {
        const auto& t = j.at("transmission");
        if (!t.is_array() || t.size() != 3) throw std::runtime_error("Material transmission needs 3 values (low, mid, high)");
        for (int i = 0; i < 3; ++i) m.transmission[i] = t[i].get<float>();
    }
    return m;
}

// ---- geometry

json meshToJson(const MeshGeometry& g) {
    json j;
    json verts = json::array();
    for (const auto& v : g.vertices) verts.push_back(vecToJson(v));
    j["vertices"] = verts;
    j["triangles"] = g.triangles;
    j["material_indices"] = g.materialIndices;
    json mats = json::array();
    for (const auto& m : g.materials) mats.push_back(materialToJson(m));
    j["materials"] = mats;
    return j;
}

MeshGeometry meshFromJson(const json& j) {
    MeshGeometry g;
    if (j.contains("materials"))
        for (const auto& m : j.at("materials")) g.materials.push_back(materialFromJson(m));
    if (g.materials.empty()) g.materials.push_back(materials::byName("plaster"));
    if (j.contains("vertices"))
        for (const auto& v : j.at("vertices")) g.vertices.push_back(vecFromJson(v, "vertices"));
    if (j.contains("triangles")) {
        for (const auto& t : j.at("triangles")) {
            if (!t.is_array() || t.size() != 3) throw std::runtime_error("Mesh triangles need 3 vertex indices each");
            std::array<int, 3> tri{t[0].get<int>(), t[1].get<int>(), t[2].get<int>()};
            for (int k : tri)
                if (k < 0 || k >= static_cast<int>(g.vertices.size())) throw std::runtime_error("Mesh triangle index out of range");
            g.triangles.push_back(tri);
        }
    }
    if (j.contains("material_indices"))
        for (const auto& m : j.at("material_indices")) g.materialIndices.push_back(m.get<int>());
    g.materialIndices.resize(g.triangles.size(), 0);
    for (int& m : g.materialIndices)
        if (m < 0 || m >= static_cast<int>(g.materials.size())) m = 0;
    return g;
}

json objectToJson(const SceneObject& o) {
    json j;
    j["name"] = o.name;
    j["min"] = vecToJson(o.minCorner);
    j["max"] = vecToJson(o.maxCorner);
    j["material"] = materialToJson(o.material);
    return j;
}

SceneObject objectFromJson(const json& j) {
    SceneObject o;
    o.name = j.value("name", "");
    if (j.contains("box")) {
        // {"box": {"min": [..], "max": [..]}} or {"box": {"center": [..], "size": [..]}}
        const auto& b = j.at("box");
        if (b.contains("center")) {
            const Vec3 c = vecFromJson(b.at("center"), "center"), sz = vecFromJson(b.at("size"), "size");
            o.minCorner = c - sz * 0.5f;
            o.maxCorner = c + sz * 0.5f;
        } else {
            o.minCorner = getVec(b, "min", o.minCorner);
            o.maxCorner = getVec(b, "max", o.maxCorner);
        }
    } else if (j.contains("center")) {
        const Vec3 c = vecFromJson(j.at("center"), "center"), sz = vecFromJson(j.at("size"), "size");
        o.minCorner = c - sz * 0.5f;
        o.maxCorner = c + sz * 0.5f;
    } else {
        o.minCorner = getVec(j, "min", o.minCorner);
        o.maxCorner = getVec(j, "max", o.maxCorner);
    }
    if (j.contains("material")) o.material = materialFromJson(j.at("material"));
    return o;
}

// ---- layers

json layerToJson(const Layer& l) {
    json j;
    j["name"] = l.name;
    j["audio"] = l.audioFile;
    j["position"] = vecToJson(l.position);
    j["level_db"] = l.levelDb;
    j["mute"] = l.mute;
    j["doppler"] = l.dopplerAmount;
    j["spread_deg"] = l.spreadDeg;
    j["directivity"] = l.directivity;
    j["directivity_forward"] = vecToJson(l.directivityForward);
    j["reference_distance"] = l.referenceDistance;
    j["min_distance"] = l.minDistance;
    j["rolloff"] = l.rolloff;
    j["reverb_send_db"] = l.reverbSendDb;
    j["reflection_order"] = l.reflectionOrder;
    j["occlusion"] = l.occlusion;
    j["occlusion_radius"] = l.occlusionRadius;
    j["start_time"] = l.startTime;
    j["loop"] = l.loop;
    j["channels"] = l.channels;
    if (l.channels == 2)
        j["stereo"] = json{{"width", l.stereo.width}, {"rotation", l.stereo.rotationDeg},
                           {"elevation", l.stereo.elevationDeg}, {"mono", l.stereo.mono}};
    return j;
}

Layer layerFromJson(const json& j) {
    Layer l;
    l.name = j.value("name", "");
    l.audioFile = j.value("audio", "");
    l.position = getVec(j, "position", l.position);
    l.levelDb = j.value("level_db", l.levelDb);
    l.mute = j.value("mute", l.mute);
    l.dopplerAmount = j.value("doppler", l.dopplerAmount);
    l.spreadDeg = j.value("spread_deg", l.spreadDeg);
    l.directivity = j.value("directivity", l.directivity);
    l.directivityForward = getVec(j, "directivity_forward", l.directivityForward);
    l.referenceDistance = j.value("reference_distance", l.referenceDistance);
    l.minDistance = j.value("min_distance", l.minDistance);
    l.rolloff = j.value("rolloff", l.rolloff);
    l.reverbSendDb = j.value("reverb_send_db", l.reverbSendDb);
    l.reflectionOrder = j.value("reflection_order", l.reflectionOrder);
    l.occlusion = j.value("occlusion", l.occlusion);
    l.occlusionRadius = j.value("occlusion_radius", l.occlusionRadius);
    l.startTime = j.value("start_time", l.startTime);
    l.loop = j.value("loop", l.loop);
    l.channels = j.value("channels", l.channels);
    if (l.channels != 1 && l.channels != 2) throw std::runtime_error("layer \"" + l.name + "\": channels must be 1 or 2");
    if (j.contains("stereo") && j["stereo"].is_object()) {
        const json& st = j["stereo"];
        l.stereo.width = st.value("width", l.stereo.width);
        l.stereo.rotationDeg = st.value("rotation", l.stereo.rotationDeg);
        l.stereo.elevationDeg = st.value("elevation", l.stereo.elevationDeg);
        l.stereo.mono = st.value("mono", l.stereo.mono);
    }
    return l;
}

// ---- room

const char* wallKey(int w) {
    static const char* keys[kNumWalls] = {"left", "right", "floor", "ceiling", "front", "back"};
    return keys[w];
}

json roomToJson(const Room& r) {
    json j;
    j["type"] = enumToString(r.type);
    j["size"] = vecToJson(r.size);
    j["origin"] = vecToJson(r.origin);
    json mats;
    for (int w = 0; w < kNumWalls; ++w) mats[wallKey(w)] = materialToJson(r.materials[w]);
    j["materials"] = mats;
    j["reflection_order"] = r.reflectionOrder;
    j["reflections_level_db"] = r.reflectionsLevelDb;
    j["reverb_level_db"] = r.reverbLevelDb;
    j["reverb_time_scale"] = r.reverbTimeScale;
    j["reflections"] = r.reflectionsEnabled;
    j["reverb"] = r.reverbEnabled;
    if (!r.impulseResponse.file.empty()) {
        const auto& ir = r.impulseResponse;
        j["impulse_response"] = json{{"file", ir.file}, {"gain_db", ir.gainDb}, {"channels", ir.channels}, {"enabled", ir.enabled}};
    }
    if (r.type == RoomType::Mesh) {
        if (!r.meshFile.empty()) j["mesh"] = json{{"file", r.meshFile}};
        else j["mesh"] = meshToJson(r.mesh);
    }
    if (!r.objects.empty()) {
        json objs = json::array();
        for (const auto& o : r.objects) objs.push_back(objectToJson(o));
        j["objects"] = objs;
    }
    return j;
}

Room roomFromJson(const json& j, const std::string& baseDir) {
    Room r;
    r.type = getEnum(j, "type", r.type);
    r.size = getVec(j, "size", r.size);
    r.origin = getVec(j, "origin", r.origin);
    if (j.contains("materials")) {
        const auto& m = j.at("materials");
        if (m.is_string() || (m.is_object() && m.contains("name") && !m.contains(wallKey(0)))) {
            const Material all = materialFromJson(m);
            for (auto& w : r.materials) w = all;
        } else {
            // "walls" sets all four side walls; a named wall overrides it.
            if (m.contains("walls")) {
                const Material walls = materialFromJson(m.at("walls"));
                for (int w : {WallNegX, WallPosX, WallNegZ, WallPosZ}) r.materials[w] = walls;
            }
            for (int w = 0; w < kNumWalls; ++w)
                if (m.contains(wallKey(w))) r.materials[w] = materialFromJson(m.at(wallKey(w)));
        }
    }
    r.reflectionOrder = j.value("reflection_order", r.reflectionOrder);
    r.reflectionsLevelDb = j.value("reflections_level_db", r.reflectionsLevelDb);
    r.reverbLevelDb = j.value("reverb_level_db", r.reverbLevelDb);
    r.reverbTimeScale = j.value("reverb_time_scale", r.reverbTimeScale);
    r.reflectionsEnabled = j.value("reflections", r.reflectionsEnabled);
    r.reverbEnabled = j.value("reverb", r.reverbEnabled);
    if (j.contains("impulse_response")) {
        // {"file": "hall.wav", "gain_db": 0, "channels": 0, "enabled": true}, or just the file name.
        const auto& ir = j.at("impulse_response");
        auto& out = r.impulseResponse;
        if (ir.is_string()) {
            out.file = ir.get<std::string>();
        } else if (ir.is_object()) {
            out.file = ir.value("file", "");
            out.gainDb = ir.value("gain_db", out.gainDb);
            out.channels = ir.value("channels", out.channels);
            out.enabled = ir.value("enabled", out.enabled);
        }
        if (out.channels != 0 && out.channels != 1 && out.channels != 2 && out.channels != 4)
            throw std::runtime_error("impulse_response.channels must be 0 (from the file), 1, 2 or 4");
        if (!out.file.empty() && !baseDir.empty() && !std::filesystem::path(out.file).is_absolute())
            out.file = (std::filesystem::path(baseDir) / out.file).lexically_normal().string();
    }
    if (j.contains("mesh")) {
        const auto& m = j.at("mesh");
        // {"file": "room.obj", "materials": {"usemtl-name": material, ...}} or inline geometry.
        if (m.is_string() || m.contains("file")) {
            r.meshFile = m.is_string() ? m.get<std::string>() : m.at("file").get<std::string>();
            std::map<std::string, Material> named;
            if (m.is_object() && m.contains("materials"))
                for (auto it = m.at("materials").begin(); it != m.at("materials").end(); ++it)
                    named[it.key()] = materialFromJson(it.value());
            const Material fallback = r.materials[WallNegX];
            auto materialFor = [&](const std::string& name) -> Material {
                auto it = named.find(name);
                if (it != named.end()) return it->second;
                if (name.empty()) return fallback;
                // Known material names work directly; anything else gets the wall material.
                const auto known = materials::names();
                if (std::find(known.begin(), known.end(), name) != known.end()) return materials::byName(name);
                return fallback;
            };
            std::string path = r.meshFile;
            if (!baseDir.empty() && !std::filesystem::path(path).is_absolute()) path = (std::filesystem::path(baseDir) / path).string();
            r.mesh = loadObjMesh(path, materialFor);
        } else {
            r.mesh = meshFromJson(m);
        }
        if (r.type != RoomType::Mesh && !j.contains("type")) r.type = RoomType::Mesh;
    }
    if (j.contains("objects"))
        for (const auto& o : j.at("objects")) r.objects.push_back(objectFromJson(o));
    if (r.type == RoomType::Mesh && !r.mesh.empty()) {
        // Keep size/origin in step with the mesh for anything that reads the bounds.
        const Vec3 mn = r.mesh.minCorner(), mx = r.mesh.maxCorner();
        r.size = mx - mn;
        r.origin = {(mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f};
    }
    return r;
}

// ---- paths

json pathToJson(const Path& p) {
    json j;
    j["name"] = p.name;
    j["closed"] = p.closed;
    json segs = json::array();
    for (const auto& s : p.segments) {
        json js;
        js["type"] = enumToString(s.type);
        json pts = json::array();
        for (const auto& v : s.points) pts.push_back(vecToJson(v));
        js["points"] = pts;
        if (s.type == SegmentType::Arc) {
            js["turns"] = s.arcTurns;
            js["clockwise"] = s.arcClockwise;
        }
        segs.push_back(js);
    }
    j["segments"] = segs;
    return j;
}

Path pathFromJson(const json& j) {
    Path p;
    p.name = j.value("name", "");
    p.closed = j.value("closed", false);
    if (j.contains("segments")) {
        for (const auto& js : j.at("segments")) {
            PathSegment s;
            s.type = getEnum(js, "type", SegmentType::Line);
            if (js.contains("points"))
                for (const auto& v : js.at("points")) s.points.push_back(vecFromJson(v, "points"));
            s.arcTurns = js.value("turns", 0);
            s.arcClockwise = js.value("clockwise", false);
            p.segments.push_back(s);
        }
    } else if (j.contains("points")) {
        // Shorthand: a single Catmull-Rom through the points.
        PathSegment s;
        s.type = getEnum(j, "type", SegmentType::CatmullRom);
        for (const auto& v : j.at("points")) s.points.push_back(vecFromJson(v, "points"));
        p.segments.push_back(s);
    }
    return p;
}

// ---- listener

json listenerToJson(const Listener& l) {
    json j;
    json paths = json::array();
    for (const auto& p : l.paths) paths.push_back(pathToJson(p));
    j["paths"] = paths;
    j["active_path"] = l.activePath;
    j["position_mode"] = enumToString(l.positionMode);
    json keys = json::array();
    for (const auto& k : l.speed.keys)
        keys.push_back({{"time", k.time}, {"speed", k.speed}, {"easing", enumToString(k.easing)}});
    j["speed"] = keys;
    j["path_start_time"] = l.pathStartTime;
    j["path_fraction"] = l.pathFraction;
    j["loop_path"] = l.loopPath;
    j["static_position"] = vecToJson(l.staticPosition);
    j["head_radius"] = l.headRadius;
    json h;
    h["mode"] = enumToString(l.head.mode);
    h["banking"] = l.head.banking;
    h["look_at_point"] = vecToJson(l.head.lookAtPoint);
    h["look_at_layer"] = l.head.lookAtLayer;
    json hk = json::array();
    for (const auto& k : l.head.keys)
        hk.push_back({{"time", k.time}, {"yaw", k.yawDeg}, {"pitch", k.pitchDeg}, {"roll", k.rollDeg},
                      {"easing", enumToString(k.easing)}});
    h["keys"] = hk;
    h["yaw_offset"] = l.head.yawOffsetDeg;
    h["pitch_offset"] = l.head.pitchOffsetDeg;
    h["roll_offset"] = l.head.rollOffsetDeg;
    j["head"] = h;
    return j;
}

Listener listenerFromJson(const json& j) {
    Listener l;
    if (j.contains("paths"))
        for (const auto& p : j.at("paths")) l.paths.push_back(pathFromJson(p));
    if (j.contains("path")) l.paths.push_back(pathFromJson(j.at("path")));
    l.activePath = j.value("active_path", 0);
    l.positionMode = getEnum(j, "position_mode", l.positionMode);
    if (j.contains("speed")) {
        const auto& s = j.at("speed");
        if (s.is_number()) {
            l.speed.keys = {{0.0, s.get<float>(), Easing::Linear}};
        } else {
            for (const auto& k : s) {
                SpeedKey sk;
                sk.time = k.value("time", 0.0);
                sk.speed = k.value("speed", 1.4f);
                sk.easing = getEnum(k, "easing", Easing::Linear);
                l.speed.keys.push_back(sk);
            }
            std::sort(l.speed.keys.begin(), l.speed.keys.end(),
                      [](const SpeedKey& a, const SpeedKey& b) { return a.time < b.time; });
        }
    }
    l.pathStartTime = j.value("path_start_time", 0.0);
    l.pathFraction = j.value("path_fraction", 0.0f);
    l.loopPath = j.value("loop_path", false);
    l.staticPosition = getVec(j, "static_position", l.staticPosition);
    l.headRadius = j.value("head_radius", l.headRadius);
    if (j.contains("head")) {
        const auto& h = j.at("head");
        l.head.mode = getEnum(h, "mode", l.head.mode);
        l.head.banking = h.value("banking", false);
        l.head.lookAtPoint = getVec(h, "look_at_point", l.head.lookAtPoint);
        l.head.lookAtLayer = h.value("look_at_layer", -1);
        if (h.contains("keys")) {
            for (const auto& k : h.at("keys")) {
                HeadKey hk;
                hk.time = k.value("time", 0.0);
                hk.yawDeg = k.value("yaw", 0.0f);
                hk.pitchDeg = k.value("pitch", 0.0f);
                hk.rollDeg = k.value("roll", 0.0f);
                hk.easing = getEnum(k, "easing", Easing::SmoothStep);
                l.head.keys.push_back(hk);
            }
            std::sort(l.head.keys.begin(), l.head.keys.end(),
                      [](const HeadKey& a, const HeadKey& b) { return a.time < b.time; });
        }
        l.head.yawOffsetDeg = h.value("yaw_offset", 0.0f);
        l.head.pitchOffsetDeg = h.value("pitch_offset", 0.0f);
        l.head.rollOffsetDeg = h.value("roll_offset", 0.0f);
    }
    return l;
}

json environmentToJson(const Environment& e) {
    return {{"speed_of_sound", e.speedOfSound}, {"temperature_c", e.temperatureC},
            {"humidity", e.relativeHumidity}, {"pressure_kpa", e.pressureKPa}, {"air_absorption", e.airAbsorption}};
}

Environment environmentFromJson(const json& j) {
    Environment e;
    e.speedOfSound = j.value("speed_of_sound", e.speedOfSound);
    e.temperatureC = j.value("temperature_c", e.temperatureC);
    e.relativeHumidity = j.value("humidity", e.relativeHumidity);
    e.pressureKPa = j.value("pressure_kpa", e.pressureKPa);
    e.airAbsorption = j.value("air_absorption", e.airAbsorption);
    return e;
}

}  // namespace

Scene sceneFromJson(const std::string& text, const std::string& baseDir) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Scene JSON parse error: ") + e.what());
    }
    try {
        Scene s;
        s.name = j.value("name", "");
        s.duration = j.value("duration", 0.0);
        if (j.contains("layers"))
            for (const auto& l : j.at("layers")) s.layers.push_back(layerFromJson(l));
        if (j.contains("room")) s.room = roomFromJson(j.at("room"), baseDir);
        if (j.contains("listener")) s.listener = listenerFromJson(j.at("listener"));
        if (j.contains("environment")) s.environment = environmentFromJson(j.at("environment"));
        return s;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("Scene JSON error: ") + e.what());
    }
}

std::string sceneToJson(const Scene& s, int indent) {
    json j;
    j["name"] = s.name;
    j["duration"] = s.duration;
    json layers = json::array();
    for (const auto& l : s.layers) layers.push_back(layerToJson(l));
    j["layers"] = layers;
    j["room"] = roomToJson(s.room);
    j["listener"] = listenerToJson(s.listener);
    j["environment"] = environmentToJson(s.environment);
    return j.dump(indent);
}

Scene loadSceneFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open scene file '" + path + "'");
    std::stringstream ss;
    ss << in.rdbuf();
    return sceneFromJson(ss.str(), std::filesystem::absolute(std::filesystem::path(path)).parent_path().string());
}

void saveSceneFile(const Scene& scene, const std::string& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write scene file '" + path + "'");
    out << sceneToJson(scene) << "\n";
}

}  // namespace sp
