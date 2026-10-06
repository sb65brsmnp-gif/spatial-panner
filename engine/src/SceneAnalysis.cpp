#include "sp/SceneAnalysis.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace sp {

SceneAnalysis analyzeScene(const Scene& scene, double duration, double dt, float pathStep) {
    SceneAnalysis a;
    if (duration <= 0) duration = scene.duration > 0 ? scene.duration : 60.0;
    if (dt <= 0) dt = std::max(0.05, duration / 6000.0);
    pathStep = std::max(pathStep, 0.005f);
    a.duration = duration;
    a.dt = dt;
    if (scene.room.type == RoomType::Mesh) a.roomMesh = scene.room.mesh;

    for (const auto& p : scene.listener.paths) {
        SampledPathView v;
        SampledPath sp(p);
        if (!sp.empty()) {
            v.length = sp.length();
            const int n = std::max(2, static_cast<int>(std::ceil(v.length / pathStep)) + 1);
            const int capped = std::min(n, 20000);
            v.points.reserve(capped);
            for (int i = 0; i < capped; ++i) v.points.push_back(sp.positionAt(v.length * i / (capped - 1)));
        }
        a.paths.push_back(std::move(v));
    }

    const PoseEvaluator eval(scene, duration);
    const SampledPath* active = eval.activePath();
    if (active) a.activePath = scene.listener.activePath;
    const int steps = static_cast<int>(std::floor(duration / dt + 1e-9)) + 1;
    a.poses.reserve(steps);
    for (int i = 0; i < steps; ++i) {
        const double t = i * dt;
        const Pose pose = eval.evaluate(t);
        PoseSample s;
        s.time = t;
        s.position = pose.position;
        float yaw, pitch, roll;
        pose.orientation.toYawPitchRoll(yaw, pitch, roll);
        s.yawDeg = radToDeg(yaw);
        s.pitchDeg = radToDeg(pitch);
        s.rollDeg = radToDeg(roll);
        s.distance = active ? eval.distanceAlongPath(t) : 0.0f;
        s.speed = pose.velocity.length();
        if (active && a.arrivalTime < 0 && !scene.listener.loopPath &&
            scene.listener.positionMode == PositionMode::Speed && s.distance >= active->length() - 1e-3f)
            a.arrivalTime = t;
        a.poses.push_back(s);
    }

    int moving = 0;
    for (size_t i = 0; i < scene.layers.size(); ++i) moving += eval.layerMotion(static_cast<int>(i)).active() ? 1 : 0;
    // Coarser in time than the listener when many layers move (keeps the JSON small).
    const double ldt = std::max(dt, duration * moving / 30000.0);
    for (size_t i = 0; i < scene.layers.size(); ++i) {
        const LayerMotionEvaluator& m = eval.layerMotion(static_cast<int>(i));
        if (!m.active()) continue;
        LayerTrack tr;
        tr.layer = static_cast<int>(i);
        tr.length = m.length();
        const Vec3 base = scene.layers[i].position - m.path().positionAt(0);
        const int n = std::min(std::max(2, static_cast<int>(std::ceil(tr.length / pathStep)) + 1), 20000);
        for (int k = 0; k < n; ++k) tr.points.push_back(base + m.path().positionAt(tr.length * k / (n - 1)));
        tr.dt = ldt;
        const int ls = static_cast<int>(std::floor(duration / ldt + 1e-9)) + 1;
        tr.samples.reserve(ls);
        for (int k = 0; k < ls; ++k) {
            const MotionState st = m.evaluate(k * ldt);
            tr.samples.push_back({scene.layers[i].position + st.offset, st.yawDeg, st.distance});
        }
        a.layers.push_back(std::move(tr));
    }
    return a;
}

std::string analysisToJson(const SceneAnalysis& a) {
    using nlohmann::json;
    auto r = [](float v) { return std::round(static_cast<double>(v) * 1000.0) / 1000.0; };  // mm / millidegree precision keeps the JSON small
    json j;
    j["duration"] = a.duration;
    j["dt"] = a.dt;
    j["active_path"] = a.activePath;
    j["arrival_time"] = a.arrivalTime;
    json paths = json::array();
    for (const auto& p : a.paths) {
        json pts = json::array();
        for (const auto& v : p.points) pts.push_back({r(v.x), r(v.y), r(v.z)});
        paths.push_back({{"length", r(p.length)}, {"points", pts}});
    }
    j["paths"] = paths;
    // Poses as flat rows: [x, y, z, yaw, pitch, roll, distance, speed].
    json poses = json::array();
    for (const auto& s : a.poses)
        poses.push_back({r(s.position.x), r(s.position.y), r(s.position.z), r(s.yawDeg), r(s.pitchDeg), r(s.rollDeg),
                         r(s.distance), r(s.speed)});
    j["poses"] = poses;
    json layers = json::array();
    for (const auto& tr : a.layers) {
        json pts = json::array();
        for (const auto& v : tr.points) pts.push_back({r(v.x), r(v.y), r(v.z)});
        json smp = json::array();
        for (const auto& s : tr.samples) smp.push_back({r(s.position.x), r(s.position.y), r(s.position.z), r(s.yawDeg), r(s.distance)});
        layers.push_back({{"layer", tr.layer}, {"length", r(tr.length)}, {"points", pts}, {"dt", tr.dt}, {"samples", smp}});
    }
    j["layers"] = layers;
    if (!a.roomMesh.empty()) {
        json verts = json::array();
        for (const auto& v : a.roomMesh.vertices) verts.push_back({r(v.x), r(v.y), r(v.z)});
        j["room_mesh"] = {{"vertices", verts}, {"triangles", a.roomMesh.triangles}};
    }
    return j.dump();
}

}  // namespace sp
