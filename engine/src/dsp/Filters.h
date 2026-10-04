// Small IIR building blocks. All coefficients are plain floats so they can be
// swapped per sub-block without allocation.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "sp/Math.h"

namespace sp::dsp {

// Direct-form-II transposed biquad.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    void setIdentity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    void copyCoefficients(const Biquad& o) { b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2; }
};

// First-order section, direct form II transposed.
struct FirstOrder {
    float b0 = 1, b1 = 0, a1 = 0;
    float z1 = 0;

    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y;
        return y;
    }
    void reset() { z1 = 0; }
    void setIdentity() { b0 = 1; b1 = a1 = 0; }
    void copyCoefficients(const FirstOrder& o) { b0 = o.b0; b1 = o.b1; a1 = o.a1; }

    // One-pole lowpass, -3 dB at fc.
    void setLowpass(float fc, float fs) {
        fc = clamp(fc, 1.0f, 0.49f * fs);
        const float w = std::tan(kPi * fc / fs);
        const float n = 1.0f / (1.0f + w);
        b0 = b1 = w * n;
        a1 = (w - 1.0f) * n;
    }

    // First-order low shelf: gain `g` (linear) below fc, unity above.
    void setLowShelf(float fc, float g, float fs) {
        // Analog prototype H(s) = (s + g*w0) / (s + w0) scaled so that
        // H(0) = g, H(inf) = 1. Bilinear transform.
        fc = clamp(fc, 1.0f, 0.49f * fs);
        g = std::max(g, 1e-4f);
        const float w0 = std::tan(kPi * fc / fs);  // normalised
        // H(z) = ((1 + g w0) + (g w0 - 1) z^-1) / ((1 + w0) + (w0 - 1) z^-1)
        const float n = 1.0f / (1.0f + w0);
        b0 = (1.0f + g * w0) * n;
        b1 = (g * w0 - 1.0f) * n;
        a1 = (w0 - 1.0f) * n;
    }

    // First-order high shelf: unity below fc, gain `g` above.
    void setHighShelf(float fc, float g, float fs) {
        // H(s) = (g s + w0) / (s + w0): H(0) = 1, H(inf) = g.
        fc = clamp(fc, 1.0f, 0.49f * fs);
        g = std::max(g, 1e-4f);
        const float w0 = std::tan(kPi * fc / fs);
        const float n = 1.0f / (1.0f + w0);
        b0 = (g + w0) * n;
        b1 = (w0 - g) * n;
        a1 = (w0 - 1.0f) * n;
    }
};

// Three-band tone shaper: a gain plus a first-order low shelf and high shelf.
// Used for wall materials and for the reverb's per-band decay.
struct ShelfPair {
    FirstOrder low, high;
    float gain = 1;

    inline float process(float x) { return high.process(low.process(x)) * gain; }
    void reset() { low.reset(); high.reset(); }
    void setIdentity() { low.setIdentity(); high.setIdentity(); gain = 1; }

    // Target magnitudes (linear) in the low, mid and high bands.
    void set(float lowGain, float midGain, float highGain, float fs, float lowFc = 350.0f, float highFc = 1800.0f) {
        gain = std::max(midGain, 0.0f);
        const float m = std::max(midGain, 1e-4f);
        low.setLowShelf(lowFc, std::max(lowGain, 0.0f) / m, fs);
        high.setHighShelf(highFc, std::max(highGain, 0.0f) / m, fs);
    }
};

// Schroeder allpass (for reverb input diffusion).
class Allpass {
public:
    void init(int length, float g) {
        buf_.assign(static_cast<size_t>(length), 0.0f);
        g_ = g;
        pos_ = 0;
    }
    inline float process(float x) {
        const float d = buf_[pos_];
        const float v = x - g_ * d;
        buf_[pos_] = v;
        if (++pos_ >= static_cast<int>(buf_.size())) pos_ = 0;
        return d + g_ * v;
    }
    void reset() { std::fill(buf_.begin(), buf_.end(), 0.0f); pos_ = 0; }

private:
    std::vector<float> buf_;
    float g_ = 0.5f;
    int pos_ = 0;
};

}  // namespace sp::dsp
