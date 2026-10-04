#include "ListenerTimeline.h"

#include <cmath>

namespace spplug {

namespace {
constexpr int kChunkBins = SharedSession::kChunkBins;
constexpr int kNumChunks = SharedSession::kNumChunks;
constexpr double kBin = SharedSession::kBinSeconds;
}  // namespace

ListenerTimeline::ListenerTimeline(const sp::Scene& scene) {
    const auto& L = scene.listener;
    double lastKey = 1;
    for (const auto& k : L.speed.keys) lastKey = std::max(lastKey, k.time);
    // Past its last key the table extrapolates with the final speed.
    speed_ = sp::SampledSpeed(L.speed, lastKey + 1);
    pathStart_ = L.pathStartTime;
    loop_ = L.loopPath;
    alongPath_ = L.positionMode == sp::PositionMode::AlongPath;
    sceneFraction_ = L.pathFraction;
    sceneActivePath_ = L.activePath;
    for (const auto& p : L.paths) pathLengths_.push_back(sp::SampledPath(p).length());
    sp::Scene s = scene;
    s.listener.positionMode = sp::PositionMode::AlongPath;
    poses_ = sp::PoseEvaluator(s, 1.0);
    chunks_.resize(kNumChunks);
}

double ListenerTimeline::binDistance(int bin) const {
    const double t0 = bin * kBin - pathStart_, t1 = (bin + 1) * kBin - pathStart_;
    return speed_.distanceAt(t1) - speed_.distanceAt(t0);
}

void ListenerTimeline::refresh(const SharedSession& s, bool useHistory) {
    if (chunks_.empty()) return;
    const uint32_t e = s.historyEpoch();
    if (e != epoch_ || useHistory != usedHistory_) {
        for (auto& c : chunks_) c.valid = false;
        epoch_ = e;
        usedHistory_ = useHistory;
        prefChunk_ = 0;
        prefWritten_ = prefUnwritten_ = 0;
        return;
    }
    // Drop the running prefix if the scene instance rewrote any bin in it.
    for (int c = 0; c < prefChunk_; ++c) {
        const Chunk& ch = chunks_[static_cast<size_t>(c)];
        if (!ch.valid || ch.version != s.chunkVersion(c)) {
            prefChunk_ = 0;
            prefWritten_ = prefUnwritten_ = 0;
            return;
        }
    }
}

const ListenerTimeline::Chunk& ListenerTimeline::chunk(const SharedSession& s, int c) {
    Chunk& ch = chunks_[static_cast<size_t>(c)];
    const uint32_t v = s.chunkVersion(c);
    if (ch.valid && ch.version == v) return ch;
    ch.written = ch.unwritten = 0;
    const int b0 = c * kChunkBins, b1 = std::min(b0 + kChunkBins, SharedSession::kMaxBins);
    // Bins wholly before the path starts cover no distance.
    if (b1 * kBin > pathStart_) {
        for (int b = b0; b < b1; ++b) {
            const double d = binDistance(b);
            if (d == 0) continue;
            const float m = s.binValue(kSpeed, b);
            if (std::isnan(m)) ch.unwritten += d;
            else ch.written += static_cast<double>(m) * d;
        }
    }
    ch.version = v;
    ch.valid = true;
    return ch;
}

double ListenerTimeline::distanceAt(const SharedSession& s, double t, bool useHistory) {
    const double tl = t - pathStart_;
    if (tl <= 0) return 0;
    if (!useHistory || chunks_.empty()) return speed_.distanceAt(tl);

    const double live = s.live(kSpeed);
    const int bin = SharedSession::binOf(t);
    const int lastChunk = std::min(bin / kChunkBins, kNumChunks);
    // Whole chunks before t: cached sums (see refresh()).
    if (lastChunk < prefChunk_) prefChunk_ = 0, prefWritten_ = prefUnwritten_ = 0;
    for (; prefChunk_ < lastChunk; ++prefChunk_) {
        const Chunk& ch = chunk(s, prefChunk_);
        prefWritten_ += ch.written;
        prefUnwritten_ += ch.unwritten;
    }
    const double written = prefWritten_, unwritten = prefUnwritten_;
    double dist = written + live * unwritten;
    if (bin >= SharedSession::kMaxBins) {
        // Beyond the history: the current multiplier.
        const double tMax = SharedSession::kMaxBins * kBin - pathStart_;
        return dist + live * (speed_.distanceAt(tl) - speed_.distanceAt(std::max(0.0, tMax)));
    }
    // Bins of the current chunk before t, then the part of t's own bin.
    for (int b = lastChunk * kChunkBins; b < bin; ++b) {
        const float m = s.binValue(kSpeed, b);
        dist += (std::isnan(m) ? live : m) * binDistance(b);
    }
    const float m = s.binValue(kSpeed, bin);
    dist += (std::isnan(m) ? live : m) * (speed_.distanceAt(tl) - speed_.distanceAt(bin * kBin - pathStart_));
    return dist;
}

sp::ListenerControls ListenerTimeline::evaluate(const SharedSession& s, double t, bool playing, bool useHistory) {
    sp::ListenerControls c;
    auto value = [&](int p, float fallback) -> float {
        if (!useHistory) return fallback;
        if (!playing) return s.live(p);
        // Linear between bin centres; unplayed bins take the current value.
        const double f = t / kBin - 0.5;
        const int b0 = static_cast<int>(std::floor(f));
        const float u = static_cast<float>(f - b0);
        float a = s.binValue(p, b0), b = s.binValue(p, b0 + 1);
        const float liveV = s.live(p);
        if (std::isnan(a)) a = std::isnan(b) ? liveV : b;
        if (std::isnan(b)) b = a;
        return a + (b - a) * u;
    };
    c.yawOffsetDeg = value(kYaw, 0);
    c.pitchOffsetDeg = value(kPitch, 0);
    c.rollOffsetDeg = value(kRoll, 0);

    int active = sceneActivePath_;
    if (useHistory) {
        float a = playing ? s.binValue(kActivePath, SharedSession::binOf(t)) : std::numeric_limits<float>::quiet_NaN();
        if (std::isnan(a)) a = s.live(kActivePath);
        if (a >= 0) active = static_cast<int>(std::lround(a));
    }
    if (active >= 0 && active < static_cast<int>(pathLengths_.size())) c.activePath = active;

    float fraction = 0;
    if (alongPath_) {
        fraction = value(kPosition, sceneFraction_);
    } else {
        const float len = active >= 0 && active < static_cast<int>(pathLengths_.size()) ? pathLengths_[static_cast<size_t>(active)] : 0.0f;
        if (len > 0) {
            double d = distanceAt(s, t, useHistory);
            if (loop_) {
                d = std::fmod(d, static_cast<double>(len));
                if (d < 0) d += len;
            }
            fraction = static_cast<float>(std::min(d, static_cast<double>(len)) / len);
        }
    }
    c.pathFraction = std::max(0.0f, std::min(1.0f, fraction));
    return c;
}

}  // namespace spplug
