#include "LayerPathHistory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace spplug {

namespace {
constexpr float kUnwritten = std::numeric_limits<float>::quiet_NaN();
}

LayerPathHistory::LayerPathHistory()
    : bins_(new std::atomic<float>[kMaxBins]),
      chunkWD_(kNumChunks, 0.0), chunkD_(kNumChunks, 0.0), prefWD_(kNumChunks + 1, 0.0), prefD_(kNumChunks + 1, 0.0),
      chunkValid_(kNumChunks, 0) {
    for (int b = 0; b < kMaxBins; ++b) bins_[static_cast<size_t>(b)].store(kUnwritten, std::memory_order_relaxed);
}

void LayerPathHistory::beginBlock() {
    if (clearPending_.exchange(false, std::memory_order_acquire)) {
        accBin_ = -1;
        accSum_ = accDur_ = 0;
        for (int b = 0; b < kMaxBins; ++b) bins_[static_cast<size_t>(b)].store(kUnwritten, std::memory_order_relaxed);
        curveChanged();
    }
    if (invalidatePending_.exchange(false, std::memory_order_acquire)) curveChanged();
}

void LayerPathHistory::curveChanged() {
    std::fill(chunkValid_.begin(), chunkValid_.end(), 0);
    prefValid_ = 0;
}

void LayerPathHistory::invalidateChunk(int c) {
    chunkValid_[static_cast<size_t>(c)] = 0;
    prefValid_ = std::min(prefValid_, c);
}

void LayerPathHistory::flush() {
    if (accBin_ >= 0 && accDur_ > 0) {
        // Played only in part (a jump into or out of it): the rest keeps
        // what was played there before.
        const float old = bin(accBin_);
        const double rest = kBinSeconds - accDur_;
        const float m = static_cast<float>(rest > 1e-9 && !std::isnan(old) ? (accSum_ + old * rest) / kBinSeconds : accSum_ / accDur_);
        if (bin(accBin_) != m) {   // NaN never equals: a first write always lands
            bins_[static_cast<size_t>(accBin_)].store(m, std::memory_order_relaxed);
            invalidateChunk(accBin_ / kChunkBins);
        }
    }
    accBin_ = -1;
    accSum_ = accDur_ = 0;
}

void LayerPathHistory::record(double t0, double t1, float m) {
    t0 = std::max(0.0, t0);
    if (!(t1 > t0) || !std::isfinite(m)) return;
    for (int b = binOf(t0); b < kMaxBins && b * kBinSeconds < t1; ++b) {
        const double lo = std::max(t0, b * kBinSeconds), hi = std::min(t1, (b + 1) * kBinSeconds);
        if (hi <= lo) continue;
        if (b != accBin_) { flush(); accBin_ = b; }
        accSum_ += m * (hi - lo);
        accDur_ += hi - lo;
        if (hi >= (b + 1) * kBinSeconds - 1e-9) flush();   // played to its end
    }
}

void LayerPathHistory::chunkSums(const sp::LayerMotionEvaluator& e, int c, double& wd, double& d) {
    const auto ci = static_cast<size_t>(c);
    if (!chunkValid_[ci]) {
        double w = 0, s = 0;
        const int b0 = c * kChunkBins, b1 = std::min(kMaxBins, b0 + kChunkBins);
        double prev = e.travel(b0 * kBinSeconds);
        for (int b = b0; b < b1; ++b) {
            const double next = e.travel((b + 1) * kBinSeconds);
            const float m = bin(b);
            if (!std::isnan(m)) {
                w += m * (next - prev);
                s += next - prev;
            }
            prev = next;
        }
        chunkWD_[ci] = w;
        chunkD_[ci] = s;
        chunkValid_[ci] = 1;
    }
    wd = chunkWD_[ci];
    d = chunkD_[ci];
}

double LayerPathHistory::distanceAt(const sp::LayerMotionEvaluator& e, double t, float current) {
    if (t <= 0) return 0;
    const double total = e.travel(t);
    const int b = binOf(t);
    if (b >= kMaxBins) {
        // Past the recording: the whole history, then the current value.
        const double recorded = distanceAt(e, kMaxBins * kBinSeconds - 1e-6, current);
        return recorded + current * (total - e.travel(kMaxBins * kBinSeconds - 1e-6));
    }
    const int c = b / kChunkBins;
    // Whole chunks before the one t is in.
    while (prefValid_ < c) {
        double wd = 0, d = 0;
        chunkSums(e, prefValid_, wd, d);
        const auto k = static_cast<size_t>(prefValid_);
        prefWD_[k + 1] = prefWD_[k] + wd;
        prefD_[k + 1] = prefD_[k] + d;
        ++prefValid_;
    }
    double wd = prefWD_[static_cast<size_t>(c)], d = prefD_[static_cast<size_t>(c)];
    // Bins of this chunk before t's, then t's own up to t.
    double prev = e.travel(c * kChunkBins * kBinSeconds);
    for (int k = c * kChunkBins; k <= b; ++k) {
        const double next = k < b ? e.travel((k + 1) * kBinSeconds) : total;
        const float m = bin(k);
        if (!std::isnan(m)) {
            wd += m * (next - prev);
            d += next - prev;
        }
        prev = next;
    }
    return current * (total - d) + wd;
}

float LayerPathHistory::speedAt(const sp::LayerMotionEvaluator& e, double t, float current) const {
    const int b = binOf(t);
    const float m = b < kMaxBins ? bin(b) : kUnwritten;
    return e.speedAt(t) * (std::isnan(m) ? current : m);
}

void LayerPathHistory::clear() {
    invalidatePending_.store(false, std::memory_order_relaxed);
    clearPending_.store(true, std::memory_order_release);
}

// Runs of equal values: "first count value-bits" per line, written bins only.
std::string LayerPathHistory::encode() const {
    std::ostringstream os;
    int b = 0;
    while (b < kMaxBins) {
        const float v = bin(b);
        if (std::isnan(v)) { ++b; continue; }
        int n = 1;
        while (b + n < kMaxBins && bin(b + n) == v) ++n;
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        os << b << ' ' << n << ' ' << std::hex << bits << std::dec << '\n';
        b += n;
    }
    return os.str();
}

void LayerPathHistory::decode(const std::string& text) {
    clearPending_.store(false, std::memory_order_relaxed);
    for (int b = 0; b < kMaxBins; ++b) bins_[static_cast<size_t>(b)].store(kUnwritten, std::memory_order_relaxed);
    std::istringstream is(text);
    int b = 0, n = 0;
    uint32_t bits = 0;
    while (is >> b >> n >> std::hex >> bits >> std::dec) {
        if (b < 0 || n <= 0) continue;
        float v = 0;
        std::memcpy(&v, &bits, sizeof v);
        if (!std::isfinite(v)) continue;
        for (int k = b; k < std::min(b + n, kMaxBins); ++k) bins_[static_cast<size_t>(k)].store(v, std::memory_order_relaxed);
    }
    invalidatePending_.store(true, std::memory_order_release);
}

}  // namespace spplug
