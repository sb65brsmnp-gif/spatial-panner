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
    if (!a.roomMesh.empty()) {
        json verts = json::array();
        for (const auto& v : a.roomMesh.vertices) verts.push_back({r(v.x), r(v.y), r(v.z)});
        j["room_mesh"] = {{"vertices", verts}, {"triangles", a.roomMesh.triangles}};
    }
    return j.dump();
}

}  // namespace sp
