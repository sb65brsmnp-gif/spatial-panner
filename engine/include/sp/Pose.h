// Listener pose as a pure function of timeline time (spec section 10).
//
// Nothing in here keeps state between calls: pose(t) depends only on the
// scene and t, which is what lets many plugin instances agree on the pose,
// and lets scrubbing and offline bounces match realtime playback.
#pragma once

#include <memory>
#include <vector>

#include "sp/Math.h"
#include "sp/Scene.h"

namespace sp {

struct Pose {
    Vec3 position;
    Quat orientation;  // head frame -> world frame
    Vec3 velocity;     // m/s, world frame (for diagnostics; the renderer derives Doppler from distance)
};

// Live controls the host or UI can change while playing. They are applied on
// top of the scene (spec section 12, "automatable parameters").
struct ListenerControls {
    float speedMultiplier = 1.0f;
    float yawOffsetDeg = 0;
    float pitchOffsetDeg = 0;
    float rollOffsetDeg = 0;
    std::optional<float> pathFraction;  // overrides Listener::pathFraction in AlongPath mode
    std::optional<int> activePath;
};

// A path sampled by arc length. Build once per scene, then query cheaply.
class SampledPath {
public:
    SampledPath() = default;
    explicit SampledPath(const Path& path, float tolerance = 0.002f);

    bool empty() const { return samples_.size() < 2; }
    float length() const { return length_; }

    // Position and unit tangent at arc-length s (clamped to [0, length]).
    Vec3 positionAt(float s) const;
    Vec3 tangentAt(float s) const;

    // Evaluate the underlying geometry directly: segment index + local u in [0,1].
    static Vec3 evaluateSegment(const PathSegment& seg, float u);

private:
    struct Sample { float s; Vec3 p; };
    std::vector<Sample> samples_;
    float length_ = 0;
    size_t findIndex(float s) const;
};

// Speed curve integrated into distance travelled.
class SampledSpeed {
public:
    SampledSpeed() = default;
    explicit SampledSpeed(const SpeedCurve& curve, double duration, double dt = 1.0 / 200.0);

    float speedAt(double t) const;      // m/s
    double distanceAt(double t) const;  // metres travelled since t = 0

private:
    SpeedCurve curve_;
    std::vector<double> distance_;  // cumulative, at multiples of dt_
    double dt_ = 0.005;
    static float evalSpeed(const SpeedCurve& c, double t);
};

// Precomputed tables for a scene's listener. Build once, query per block.
class PoseEvaluator {
public:
    PoseEvaluator() = default;
    // `duration` bounds the speed integration table (seconds).
    PoseEvaluator(const Scene& scene, double duration);

    Pose evaluate(double time, const ListenerControls& controls = {}) const;

    // Arc length along the active path at `time` (Speed mode) or from the fraction.
    float distanceAlongPath(double time, const ListenerControls& controls = {}) const;
    const SampledPath* activePath(const ListenerControls& controls = {}) const;

private:
    Scene scene_;  // copy: the evaluator owns its inputs
    std::vector<SampledPath> paths_;
    SampledSpeed speed_;
    double duration_ = 0;

    Quat orientationAt(double time, const Vec3& position, const Vec3& tangent,
                       const ListenerControls& controls) const;
};

float applyEasing(Easing e, float u);

}  // namespace sp
