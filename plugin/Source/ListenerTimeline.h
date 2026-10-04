// The listener's controls at a timeline time, the same in every instance.
//
// The scene's own data (paths, speed curve, head keys) is a pure function of
// time, but host automation is not: a layer instance cannot see the scene
// instance's automation lanes, and the "speed" control has to be integrated
// over time (position = integral of speed x multiplier). So the scene
// instance records what it played into the session's history bins, and every
// instance (the scene instance too) evaluates the controls from there:
//
//   * speed multiplier: distance along the path at time t is
//       sum over bins before t of multiplier(bin) * distance the speed curve
//       covers in that bin
//     which telescopes to the engine's own speed-curve distance when the
//     multiplier is 1 everywhere. Bins never played use the current value.
//   * position along path, yaw, pitch, roll offsets: the recorded value at t
//     (linear between bins), or the current value while the host is stopped.
//   * active path: the recorded value at t.
//
// The result is handed to the renderer as a path fraction (the renderer runs
// the listener in "position along path" mode), so the engine needs nothing new.
#pragma once

#include <memory>
#include <vector>

#include "SharedSession.h"
#include "sp/Pose.h"
#include "sp/Scene.h"

namespace spplug {

class ListenerTimeline {
public:
    ListenerTimeline() = default;
    // Message thread. `scene` is the scene as the editor wrote it (its own
    // position mode, speed curve and active path).
    explicit ListenerTimeline(const sp::Scene& scene);

    // Audio thread. Call once per host block before evaluate(): picks up
    // history the scene instance wrote since the last call.
    void refresh(const SharedSession& s, bool useHistory);

    // Audio thread. Controls for the renderer at timeline time `t`.
    // `playing`: the host transport runs (stopped = current knob values).
    // `useHistory`: a scene instance owns the session; otherwise the scene's
    // own data with no automation.
    sp::ListenerControls evaluate(const SharedSession& s, double t, bool playing, bool useHistory);

    // Distance along the active path at `t` (metres, before looping/clamping).
    double distanceAt(const SharedSession& s, double t, bool useHistory);

    // The pose the renderer will use (for the editor's listener avatar).
    sp::Pose pose(double t, const sp::ListenerControls& c) const { return poses_.evaluate(t, c); }

    bool alongPathMode() const { return alongPath_; }

private:
    double binDistance(int bin) const;  // distance the speed curve covers in a bin

    sp::SampledSpeed speed_;
    double pathStart_ = 0;
    bool loop_ = false;
    bool alongPath_ = false;
    float sceneFraction_ = 0;
    int sceneActivePath_ = 0;
    std::vector<float> pathLengths_;
    sp::PoseEvaluator poses_;   // the scene in AlongPath mode, as the renderer runs it

    // Speed integration cache, per chunk of history bins.
    struct Chunk {
        double written = 0;     // sum of multiplier x distance over played bins
        double unwritten = 0;   // distance over bins never played (x current multiplier)
        uint32_t version = 0;
        bool valid = false;
    };
    const Chunk& chunk(const SharedSession& s, int c);
    std::vector<Chunk> chunks_;
    int prefChunk_ = 0;             // chunks [0, prefChunk_) are summed in:
    double prefWritten_ = 0, prefUnwritten_ = 0;
    uint32_t epoch_ = 0;
    bool usedHistory_ = false;
};

}  // namespace spplug
