// This track's Path Speed automation, recorded as it plays.
//
// A layer timed by speed travels the integral of its speed curve times the
// Path Speed multiplier. The multiplier is host automation on this track, so
// the distance at a timeline time depends on every value played before it.
// Like the listener's (ListenerTimeline), the values are recorded into bins
// as the host plays, and the distance at time t is
//
//   sum over bins before t of multiplier(bin) * distance the curve covers in that bin
//
// where bins never played use the current value. With the multiplier at 1
// everywhere it is exactly the layer's own speed-curve distance. Unlike the
// listener's, the history is this instance's own (no other track needs it)
// and is saved with the track.
//
// Threading: record(), distanceAt(), curveChanged() on the audio thread;
// clear() and decode() from any other thread (they take effect at the next
// audio block); encode() from any thread.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "sp/Pose.h"

namespace spplug {

class LayerPathHistory {
public:
    static constexpr double kBinSeconds = 0.05;
    static constexpr int kMaxBins = 2 * 3600 * 20;   // two hours
    static constexpr int kChunkBins = 40;            // two seconds
    static constexpr int kNumChunks = (kMaxBins + kChunkBins - 1) / kChunkBins;

    LayerPathHistory();

    // Audio thread: applies a pending clear() or decode(). Call once per block
    // before record() and distanceAt().
    void beginBlock();
    // Audio thread: the multiplier was `m` over [t0, t1). A bin holds the
    // time average of what was played in it (so a ramp within one bin is
    // integrated right); it is stored once the playhead leaves it.
    void record(double t0, double t1, float m);
    // Audio thread: metres travelled by timeline time t (before the path's
    // end rule folds it), and the speed there; `current` stands in for bins
    // never played.
    double distanceAt(const sp::LayerMotionEvaluator& e, double t, float current);
    float speedAt(const sp::LayerMotionEvaluator& e, double t, float current) const;
    // Audio thread: the speed curve changed, so cached sums are stale.
    void curveChanged();

    void clear();
    std::string encode() const;
    void decode(const std::string& text);

    static int binOf(double t) { return t <= 0 ? 0 : static_cast<int>(t / kBinSeconds); }

private:
    float bin(int b) const { return bins_[static_cast<size_t>(b)].load(std::memory_order_relaxed); }
    void chunkSums(const sp::LayerMotionEvaluator& e, int c, double& wd, double& d);
    void invalidateChunk(int c);
    void flush();

    // The bin being played: running sum of multiplier x seconds, and seconds.
    int accBin_ = -1;
    double accSum_ = 0, accDur_ = 0;

    std::unique_ptr<std::atomic<float>[]> bins_;   // NaN: never played
    // Per chunk: sum of multiplier x curve distance, and of curve distance,
    // over its played bins. Prefix sums over chunks [0, k) are valid for
    // k <= prefValid_.
    std::vector<double> chunkWD_, chunkD_, prefWD_, prefD_;
    std::vector<char> chunkValid_;
    int prefValid_ = 0;
    std::atomic<bool> clearPending_{false};
    std::atomic<bool> invalidatePending_{false};
};

}  // namespace spplug
