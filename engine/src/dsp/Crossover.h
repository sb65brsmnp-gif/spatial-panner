// Bass management for loudspeaker layouts: a crossover takes the lows out
// of every spatial speaker's feed and sends their sum to the subwoofer(s).
//
// Filter shapes: Butterworth 12 dB/oct (one second-order section),
// Butterworth 24 dB/oct (two sections, Q 0.5412 and 1.3066), Linkwitz-Riley
// 24 dB/oct (two Butterworth-2 sections, Q 0.7071: the lows and highs sum
// flat in magnitude) and Linkwitz-Riley 48 dB/oct (two Butterworth-4
// cascades).
#pragma once

#include <vector>

#include "dsp/Filters.h"
#include "sp/Math.h"
#include "sp/SpeakerLayout.h"

namespace sp::dsp {

class BassCrossover {
public:
    // Builds the filters for `layout` at `fs`. Does nothing (and process() is
    // a no-op) unless bass management is on and the layout has a subwoofer.
    void init(const SpeakerLayout& layout, float fs) {
        active_ = false;
        sections_.clear();
        q_.clear();
        isSub_.assign(static_cast<size_t>(layout.numChannels()), 0);
        for (int i = 0; i < layout.numChannels(); ++i) isSub_[static_cast<size_t>(i)] = layout.speakers[static_cast<size_t>(i)].lfe ? 1 : 0;
        if (!layout.bass.enabled || layout.numSubwoofers() == 0 || layout.numSpatialChannels() == 0) return;
        using F = BassManagement::Filter;
        switch (layout.bass.filter) {
            case F::Butterworth12: q_ = {0.70710678f}; break;
            case F::Butterworth24: q_ = {0.54119610f, 1.30656296f}; break;
            case F::LinkwitzRiley24: q_ = {0.70710678f, 0.70710678f}; break;
            case F::LinkwitzRiley48: q_ = {0.54119610f, 1.30656296f, 0.54119610f, 1.30656296f}; break;
        }
        const size_t n = static_cast<size_t>(layout.numChannels()), k = q_.size();
        // Per channel: k highpass sections (kept in the feed) and k lowpass
        // sections (summed to the subs).
        sections_.assign(n * 2 * k, Biquad{});
        for (size_t c = 0; c < n; ++c)
            for (size_t s = 0; s < k; ++s) {
                sections_[(c * 2) * k + s].setHighpass(layout.bass.cutoffHz, q_[s], fs);
                sections_[(c * 2 + 1) * k + s].setLowpass(layout.bass.cutoffHz, q_[s], fs);
            }
        subGain_ = dbToGain(layout.bass.subGainDb);
        low_.assign(64, 0.0f);
        active_ = true;
    }

    bool active() const { return active_; }

    // `ch[c]` is channel c's block of `n` samples, filtered in place: spatial
    // channels lose their lows, each subwoofer channel gets the summed lows
    // added to what it carried (nothing from the spatialiser, by design).
    void process(float* const* ch, int numChannels, int n) {
        if (!active_) return;
        if (low_.size() < static_cast<size_t>(n)) low_.assign(static_cast<size_t>(n), 0.0f);
        std::fill(low_.begin(), low_.begin() + n, 0.0f);
        const size_t k = q_.size();
        const int nc = std::min<int>(numChannels, static_cast<int>(isSub_.size()));
        for (int c = 0; c < nc; ++c) {
            if (isSub_[static_cast<size_t>(c)]) continue;
            float* x = ch[c];
            Biquad* hp = &sections_[(static_cast<size_t>(c) * 2) * k];
            Biquad* lp = &sections_[(static_cast<size_t>(c) * 2 + 1) * k];
            for (int i = 0; i < n; ++i) {
                float lo = x[i], hi = x[i];
                for (size_t s = 0; s < k; ++s) { lo = lp[s].process(lo); hi = hp[s].process(hi); }
                x[i] = hi;
                low_[static_cast<size_t>(i)] += lo;
            }
        }
        for (int c = 0; c < nc; ++c) {
            if (!isSub_[static_cast<size_t>(c)]) continue;
            float* x = ch[c];
            for (int i = 0; i < n; ++i) x[i] += low_[static_cast<size_t>(i)] * subGain_;
        }
    }

    void reset() {
        for (auto& s : sections_) s.reset();
    }

private:
    bool active_ = false;
    std::vector<Biquad> sections_;   // [channel][hp|lp][section]
    std::vector<float> q_;
    std::vector<char> isSub_;
    std::vector<float> low_;
    float subGain_ = 1;
};

}  // namespace sp::dsp
