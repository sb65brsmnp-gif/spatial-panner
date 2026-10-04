#include "dsp/Fdn.h"

#include <algorithm>
#include <cmath>

namespace sp::dsp {

namespace {

bool isPrime(int n) {
    if (n < 2) return false;
    for (int d = 2; d * d <= n; ++d)
        if (n % d == 0) return false;
    return true;
}

int nearestPrime(int n) {
    for (int d = 0;; ++d) {
        if (isPrime(n + d)) return n + d;
        if (n - d > 2 && isPrime(n - d)) return n - d;
    }
}

}  // namespace

void Fdn::init(const FdnParams& p) {
    nCh_ = ambiChannels(std::min(p.ambiOrder, kMaxAmbiOrder));
    const float fs = p.sampleRate;

    // Line lengths spread between 0.6x and 1.6x the mean free path, mutually
    // prime, so the echo density grows quickly without a periodic ring.
    const float base = std::max(p.meanFreePathSeconds * fs, 64.0f);
    for (int i = 0; i < kLines; ++i) {
        const float frac = static_cast<float>(i) / (kLines - 1);
        const int target = static_cast<int>(base * (0.6f + 1.0f * std::pow(frac, 0.8f)));
        len_[i] = nearestPrime(std::max(target, 16));
        buf_[i].assign(static_cast<size_t>(len_[i]), 0.0f);
        pos_[i] = 0;
        inputSign_[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    }

    // Per-line absorption so each band decays by 60 dB in its RT60:
    // gain per pass g = 10^(-3 * L / (fs * T60)).
    auto lineGain = [&](int len, float t60) {
        t60 = std::max(t60, 0.05f);
        return std::pow(10.0f, -3.0f * static_cast<float>(len) / (fs * t60));
    };
    for (int i = 0; i < kLines; ++i) {
        absorb_[i].set(lineGain(len_[i], p.rt60Low), lineGain(len_[i], p.rt60Mid), lineGain(len_[i], p.rt60High), fs,
                       400.0f, 2500.0f);
        absorb_[i].reset();
    }

    // Input diffusion.
    ap1_.init(nearestPrime(static_cast<int>(0.0047f * fs)), 0.6f);
    ap2_.init(nearestPrime(static_cast<int>(0.0131f * fs)), 0.55f);

    const int pre = static_cast<int>(std::max(p.preDelaySeconds, 0.0f) * fs);
    preDelay_.assign(static_cast<size_t>(pre), 0.0f);
    prePos_ = 0;

    // Output directions: a Fibonacci sphere, so the field is uniform.
    for (int i = 0; i < kLines; ++i) {
        const float y = 1.0f - 2.0f * (i + 0.5f) / kLines;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = i * 2.3999632f;  // golden angle
        const Vec3 dir{r * std::cos(phi), y, r * std::sin(phi)};
        sh_[i].fill(0.0f);
        encodeDirection(dir, std::min(p.ambiOrder, kMaxAmbiOrder), sh_[i].data());
    }

    // Normalise: run an impulse through a copy and measure the W-channel energy
    // so that a unit impulse in gives unit-energy omni reverb out.
    outputScale_ = 1.0f;
    Fdn probe = *this;
    const int n = static_cast<int>(fs * std::min(std::max({p.rt60Low, p.rt60Mid, p.rt60High}) * 1.5f + 0.1f, 12.0f));
    double energy = 0;
    float amb[kMaxAmbiChannels];
    for (int i = 0; i < n; ++i) {
        std::fill(amb, amb + kMaxAmbiChannels, 0.0f);
        probe.process(i == 0 ? 1.0f : 0.0f, 1.0f, amb);
        energy += static_cast<double>(amb[0]) * amb[0];
    }
    measuredEnergy_ = static_cast<float>(energy);
    outputScale_ = energy > 1e-12 ? static_cast<float>(1.0 / std::sqrt(energy)) : 1.0f;
    reset();
}

void Fdn::reset() {
    for (int i = 0; i < kLines; ++i) {
        std::fill(buf_[i].begin(), buf_[i].end(), 0.0f);
        pos_[i] = 0;
        absorb_[i].reset();
    }
    std::fill(preDelay_.begin(), preDelay_.end(), 0.0f);
    prePos_ = 0;
    ap1_.reset();
    ap2_.reset();
}

}  // namespace sp::dsp
