#include "sp/PanProbe.h"

#include <algorithm>
#include <cmath>

#include "dsp/Panning.h"

namespace sp {

namespace {
float smoothstep(float x) {
    x = std::max(0.0f, std::min(1.0f, x));
    return x * x * (3 - 2 * x);
}
}  // namespace

void applyPins(const std::vector<Pin>& pins, const Vec3& position, float* gains, int numChannels) {
    if (pins.empty() || numChannels <= 0) return;
    // Each pin's say grows from nothing at its radius to everything at its
    // centre; together they never say more than everything.
    float total = 0;
    std::vector<float> mix(static_cast<size_t>(numChannels), 0.0f);
    for (const auto& p : pins) {
        const float d = (position - p.position).length();
        const float w = smoothstep(1.0f - d / std::max(p.radius, 1e-3f));
        if (w <= 0) continue;
        total += w;
        for (int c = 0; c < numChannels; ++c) {
            const float db = c < static_cast<int>(p.gainsDb.size()) ? p.gainsDb[static_cast<size_t>(c)] : -120.0f;
            mix[static_cast<size_t>(c)] += w * (db <= -119.0f ? 0.0f : dbToGain(db));
        }
    }
    if (total <= 0) return;
    const float share = std::min(total, 1.0f);   // how much the pins decide
    for (int c = 0; c < numChannels; ++c) {
        const float pinned = mix[static_cast<size_t>(c)] / total;
        gains[c] = (1 - share) * gains[c] + share * pinned;
    }
}

std::vector<std::vector<float>> panProbe(const PanProbeRequest& req) {
    const int n = req.layout.numChannels();
    std::vector<std::vector<float>> out;
    if (n == 0) return out;
    dsp::VbapPanner vbap;
    vbap.init(req.layout, 1, 0.0f);
    // Alignment, as the renderer: the farthest speaker is the reference.
    float maxD = 0;
    for (const auto& spk : req.layout.speakers) maxD = std::max(maxD, spk.distance);
    std::vector<float> align(static_cast<size_t>(n), 1.0f);
    for (int i = 0; i < n; ++i) {
        const auto& spk = req.layout.speakers[static_cast<size_t>(i)];
        const float d = spk.distance > 0 ? spk.distance : maxD;
        align[static_cast<size_t>(i)] = (maxD > 0 ? d / maxD : 1.0f) * dbToGain(spk.trimDb);
    }
    const float refGain = std::pow(std::max(req.referenceDistance, 0.01f), req.rolloff);
    out.reserve(req.points.size());
    std::vector<float> g(static_cast<size_t>(n));
    for (const auto& p : req.points) {
        const float dist = p.length();
        const Vec3 dir = dist > 1e-6f ? p * (1.0f / dist) : Vec3{0, 0, -1};
        vbap.gains(dir, g.data());
        applyPins(req.pins, p, g.data(), n);
        const float distGain = refGain / std::pow(std::max(dist, req.minDistance), req.rolloff);
        std::vector<float> row(static_cast<size_t>(n));
        for (int c = 0; c < n; ++c)
            row[static_cast<size_t>(c)] = req.layout.speakers[static_cast<size_t>(c)].lfe ? 0.0f
                                        : g[static_cast<size_t>(c)] * distGain * align[static_cast<size_t>(c)];
        out.push_back(std::move(row));
    }
    return out;
}

}  // namespace sp
