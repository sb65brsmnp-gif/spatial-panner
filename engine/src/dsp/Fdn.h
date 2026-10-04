// Feedback delay network late reverb with a 3-band decay, output as a
// decorrelated Ambisonics field (each line is a plane wave from its own
// direction).
#pragma once

#include <array>
#include <vector>

#include "dsp/Ambisonics.h"
#include "dsp/Filters.h"

namespace sp::dsp {

struct FdnParams {
    float sampleRate = 48000;
    float rt60Low = 1.0f;    // seconds, ~125-250 Hz
    float rt60Mid = 0.8f;    // ~500-2k
    float rt60High = 0.5f;   // ~4k+
    float meanFreePathSeconds = 0.010f;  // sets the delay-line lengths
    float preDelaySeconds = 0.02f;
    int ambiOrder = 3;
};

class Fdn {
public:
    static constexpr int kLines = 16;

    void init(const FdnParams& p);
    void reset();

    // Mono input sample in; adds the reverb field into `ambiOut`
    // (ambiChannels(order) values) scaled by `gain`.
    inline void process(float in, float gain, float* ambiOut) {
        // Pre-delay then input diffusion.
        float x = preDelay_.empty() ? in : preDelay_[prePos_];
        if (!preDelay_.empty()) {
            preDelay_[prePos_] = in;
            if (++prePos_ >= static_cast<int>(preDelay_.size())) prePos_ = 0;
        }
        x = ap2_.process(ap1_.process(x));

        // Read the line outputs.
        float out[kLines];
        float sum = 0;
        for (int i = 0; i < kLines; ++i) {
            out[i] = buf_[i][pos_[i]];
            sum += out[i];
        }
        // Householder feedback: y_i = out_i - (2/N) * sum.
        const float k = 2.0f / kLines * sum;
        for (int i = 0; i < kLines; ++i) {
            const float fb = out[i] - k;
            buf_[i][pos_[i]] = absorb_[i].process(fb + x * inputSign_[i]);
            if (++pos_[i] >= len_[i]) pos_[i] = 0;
        }
        // Encode each line as a plane wave.
        const float g = gain * outputScale_;
        for (int i = 0; i < kLines; ++i) {
            const float v = out[i] * g;
            const float* sh = sh_[i].data();
            for (int c = 0; c < nCh_; ++c) ambiOut[c] += v * sh[c];
        }
    }

    // Energy of the omnidirectional (W) impulse response before outputScale_;
    // used by init() to normalise so that W has unit energy.
    float measuredEnergy() const { return measuredEnergy_; }

private:
    int nCh_ = 16;
    std::array<std::vector<float>, kLines> buf_;
    std::array<int, kLines> len_{}, pos_{};
    std::array<float, kLines> inputSign_{};
    std::array<ShelfPair, kLines> absorb_;
    std::array<std::array<float, kMaxAmbiChannels>, kLines> sh_{};
    std::vector<float> preDelay_;
    int prePos_ = 0;
    Allpass ap1_, ap2_;
    float outputScale_ = 1.0f;
    float measuredEnergy_ = 1.0f;
};

}  // namespace sp::dsp
