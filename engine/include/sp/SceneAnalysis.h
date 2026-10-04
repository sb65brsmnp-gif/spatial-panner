// Scene sampled for display: what the 3D editor draws (path curves, the
// listener's pose over time, where it is at each second). Everything comes
// from the same SampledPath / PoseEvaluator the renderer uses, so what the
// editor shows is what the engine plays.
#pragma once

#include <string>
#include <vector>

#include "sp/Pose.h"
#include "sp/Scene.h"

namespace sp {

struct SampledPathView {
    float length = 0;             // metres
    std::vector<Vec3> points;     // the curve, sampled every `pathStep` metres (ends included)
};

struct PoseSample {
    double time = 0;
    Vec3 position;
    float yawDeg = 0, pitchDeg = 0, rollDeg = 0;  // head orientation (yaw positive left, pitch up)
    float distance = 0;           // metres along the active path
    float speed = 0;              // m/s along the path
};

struct SceneAnalysis {
    double duration = 0;          // seconds covered by `poses`
    double dt = 0;                // spacing of `poses`
    int activePath = -1;          // -1 = no usable path (listener stands at static_position)
    std::vector<SampledPathView> paths;  // one per scene path, same order
    std::vector<PoseSample> poses;       // at t = 0, dt, 2 dt, ... duration
    double arrivalTime = -1;      // when the listener reaches the end of the active path, -1 = never
};

// `duration` <= 0 uses scene.duration, or 60 s when that is 0 too.
// `dt` <= 0 picks 0.05 s, coarser for long scenes (at most ~6000 samples).
SceneAnalysis analyzeScene(const Scene& scene, double duration = 0, double dt = 0, float pathStep = 0.05f);

std::string analysisToJson(const SceneAnalysis& analysis);

}  // namespace sp
