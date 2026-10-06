// Listener pose as a pure function of timeline time (spec section 10).
//
// Nothing in here keeps state between calls: pose(t) depends only on the
// scene and t, which is what lets many plugin instances agree on the pose,
// and lets scrubbing and offline bounces match realtime playback.
#pragma once

#include <memory>
#include <optional>
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

// ------------------------------------------------------------ Layer motion

// Live overrides of a layer's travel (the plugin's per-track automation).
struct MotionOverride {
    // Speed timing: metres travelled since the start of the path (before the
    // end rule folds it onto the path) and the speed there, instead of the
    // layer's own speed curve.
    std::optional<double> distance;
    float speed = 0;
    // Position timing: 0..1 along the path instead of LayerMotion::fraction.
    std::optional<float> fraction;
};

struct MotionState {
    Vec3 offset;        // from the layer's own position to where it is now
    float yawDeg = 0;   // turn about +Y since the start of the path (turnAlongPath), else 0
    float gain = 1;     // < 1 only around the jump back to the start of an open looping path
    float distance = 0; // metres along the path (0..length)
    bool forward = true;  // travelling in the path's direction
};

// A layer's path and timing, built once, queried per sub-block. Like the
// listener's pose, a pure function of timeline time (and the overrides).
class LayerMotionEvaluator {
public:
    LayerMotionEvaluator() = default;
    explicit LayerMotionEvaluator(const LayerMotion& motion);

    bool active() const { return active_; }
    float length() const { return path_.length(); }
    const LayerMotion& motion() const { return m_; }
    const SampledPath& path() const { return path_; }

    // Speed timing: metres travelled by time t (0 before the start time) and
    // the speed there, from the layer's own speed curve.
    double travel(double t) const;
    float speedAt(double t) const;

    MotionState evaluate(double t, const MotionOverride& o = {}) const;

private:
    LayerMotion m_;
    SampledPath path_;
    SampledSpeed speed_;
    bool active_ = false;
    bool closed_ = false;       // the path ends where it starts: looping does not jump
    float startHeading_ = 0;    // degrees, the direction of travel at the start

    float headingAt(float s, bool forward) const;
    float keyFraction(double t) const;
    bool keyDirection(double t) const;
};

// The level automation's value at `t` in dB (0 with no keys).
float levelKeysDb(const std::vector<LevelKey>& keys, double t);
// Linear gain of the level automation at `t` (0 at or below kSilentDb).
float levelKeysGain(const std::vector<LevelKey>& keys, double t);

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

    // Layer i's path and timing (an inactive evaluator when it has no path).
    const LayerMotionEvaluator& layerMotion(int i) const;
    // Where layer i is at `time` (its own timing, no overrides).
    Vec3 layerPosition(int i, double time) const;

private:
    Scene scene_;  // copy: the evaluator owns its inputs
    std::vector<SampledPath> paths_;
    std::vector<LayerMotionEvaluator> layers_;
    SampledSpeed speed_;
    double duration_ = 0;

    Quat orientationAt(double time, const Vec3& position, const Vec3& tangent,
                       const ListenerControls& controls) const;
};

float applyEasing(Easing e, float u);

}  // namespace sp
