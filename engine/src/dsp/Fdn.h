// Feedback delay network late reverb with a 3-band decay, output as a
// decorrelated Ambisonics field (each line is a plane wave from its own
// direction).
//
// 32 delay lines spanning 0.6x to 2.8x the room's mean free path, mixed by a
// Hadamard matrix (a fast Walsh-Hadamard butterfly, so the mixing is dense
// but costs 5 adds per line), fed through four input allpasses whose
// intermediate outputs are staggered over the lines, with slow random delay
// modulation on half the lines so no mode sits still long enough to ring.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dsp/Ambisonics.h"
#include "dsp/Filters.h"

namespace sp::dsp {

struct FdnParams {
    float sampleRate = 48000;
    float rt60Low = 1.0f;    // seconds at ~177 Hz (the 125-250 Hz material bands)
    float rt60Mid = 0.8f;    // at ~707 Hz (500 Hz - 1 kHz)
    float rt60High = 0.5f;   // at ~2.8 kHz (2-4 kHz)
    float meanFreePathSeconds = 0.010f;  // sets the delay-line lengths
    float preDelaySeconds = 0.02f;
    int ambiOrder = 3;
    bool modulation = true;  // slow random delay modulation (off for exact repeatability tests)
};

class Fdn {
public:
    static constexpr int kLines = 32;
    static constexpr int kAllpasses = 4;

    void init(const FdnParams& p);
    void reset();

    // Mono input sample in; adds the reverb field into `ambiOut`
    // (ambiChannels(order) values) scaled by `gain`.
    inline void process(float in, float gain, float* ambiOut) {
        // Pre-delay, then input diffusion: four allpasses in series; line i is
        // fed from stage (i mod 4), so the lines see the input at staggered
        // times and the early echo density builds faster.
        float x = preDelay_.empty() ? in : preDelay_[prePos_];
        if (!preDelay_.empty()) {
            preDelay_[prePos_] = in;
            if (++prePos_ >= static_cast<int>(preDelay_.size())) prePos_ = 0;
        }
        float stage[kAllpasses];
        for (int k = 0; k < kAllpasses; ++k) {
            x = ap_[k].process(x);
            stage[k] = x;
        }

        // Read the (modulated) line outputs. The fractional part is a
        // first-order allpass (Thiran) interpolator: its magnitude is exactly
        // 1 at every frequency, which matters inside a feedback loop that a
        // sample passes through hundreds of times (linear interpolation's
        // -3 dB at fs/4 per pass would turn the tail dull and throw the
        // per-band decay off). The fraction is kept in [0.5, 1.5) where the
        // allpass is well behaved.
        float out[kLines];
        for (int i = 0; i < kLines; ++i) {
            Line& L = lines_[i];
            if (L.modDepth > 0) {
                if (--L.modCount <= 0) nextModSegment(L);
                L.modCur += L.modStep;
            }
            const float d = static_cast<float>(L.length) + L.modCur;
            const int di = static_cast<int>(d - 0.5f);
            const float f = d - static_cast<float>(di);
            const float eta = (1.0f - f) / (1.0f + f);
            const int idx = (L.write - di) & L.mask;
            const float xa = L.buf[idx], xb = L.buf[(idx - 1) & L.mask];
            const float y = eta * (xa - L.apState) + xb;
            L.apState = y;
            out[i] = y;
        }

        // Hadamard feedback (fast Walsh-Hadamard transform, scaled to be unitary).
        float fb[kLines];
        for (int i = 0; i < kLines; ++i) fb[i] = out[i];
        for (int h = 1; h < kLines; h <<= 1)
            for (int i = 0; i < kLines; i += h << 1)
                for (int j = i; j < i + h; ++j) {
                    const float u = fb[j], v = fb[j + h];
                    fb[j] = u + v;
                    fb[j + h] = u - v;
                }
        for (int i = 0; i < kLines; ++i) {
            Line& L = lines_[i];
            const float v = fb[i] * kHadamardScale + stage[i & (kAllpasses - 1)] * L.inputGain;
            L.buf[L.write] = L.absorb.process(v);
            L.write = (L.write + 1) & L.mask;
        }

        // Encode each line as a plane wave from its own direction.
        const float g = gain * outputScale_;
        for (int i = 0; i < kLines; ++i) {
            const float v = out[i] * g;
            const float* sh = lines_[i].sh.data();
            for (int c = 0; c < nCh_; ++c) ambiOut[c] += v * sh[c];
        }
    }

    // Energy of the omnidirectional (W) impulse response before outputScale_;
    // used by init() to normalise so that W has unit energy.
    float measuredEnergy() const { return measuredEnergy_; }

    // Nominal delay of line i in samples (for tests).
    int lineLength(int i) const { return lines_[i].length; }

private:
    static constexpr float kHadamardScale = 0.17677669529f;  // 1 / sqrt(32)

    struct Line {
        std::vector<float> buf;
        int mask = 0, write = 0;
        int length = 0;         // nominal delay, samples
        float inputGain = 0;    // +-1 pattern, zero-sum across the lines
        ShelfPair absorb;
        std::array<float, kMaxAmbiChannels> sh{};
        // Slow random modulation: piecewise-linear wander of the delay
        // between random targets within +-modDepth samples.
        float modDepth = 0, modCur = 0, modStep = 0;
        int modCount = 0;
        float apState = 0;      // allpass interpolator's previous output
    };

    void nextModSegment(Line& L);
    float random01();

    int nCh_ = 16;
    float fs_ = 48000;
    std::array<Line, kLines> lines_;
    std::vector<float> preDelay_;
    int prePos_ = 0;
    std::array<Allpass, kAllpasses> ap_;
    float outputScale_ = 1.0f;
    float measuredEnergy_ = 1.0f;
    uint32_t rng_ = 0x9e3779b9u;
};

}  // namespace sp::dsp
