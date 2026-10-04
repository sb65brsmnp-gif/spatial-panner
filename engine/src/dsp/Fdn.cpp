#include "dsp/Fdn.h"

#include <algorithm>
#include <cmath>
#include <complex>

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

double magnitude(const FirstOrder& s, double f, double fs) {
    const std::complex<double> z = std::exp(std::complex<double>(0, -2 * M_PI * f / fs));
    return std::abs((static_cast<double>(s.b0) + static_cast<double>(s.b1) * z) / (1.0 + static_cast<double>(s.a1) * z));
}

// Band centres the RT60s refer to (geometric means of the 125-250, 500-1k and
// 2k-4k Hz material bands) and the shelf corners between them.
constexpr double kLowHz = 176.8, kMidHz = 707.1, kHighHz = 2828.4, kLowCorner = 353.6, kHighCorner = 1414.2;

// A ShelfPair whose magnitude hits the three per-pass gains at the band
// centres (first-order shelves are soft, so the plain corner-frequency design
// lands part way between neighbouring bands; a few corrections fix that).
void designAbsorption(ShelfPair& sp, float gLow, float gMid, float gHigh, float fs) {
    float l = gLow, m = gMid, h = gHigh;
    for (int iter = 0; iter < 6; ++iter) {
        sp.set(l, m, h, fs, static_cast<float>(kLowCorner), static_cast<float>(kHighCorner));
        const double aL = magnitude(sp.low, kLowHz, fs) * magnitude(sp.high, kLowHz, fs) * sp.gain;
        const double aM = magnitude(sp.low, kMidHz, fs) * magnitude(sp.high, kMidHz, fs) * sp.gain;
        const double aH = magnitude(sp.low, kHighHz, fs) * magnitude(sp.high, kHighHz, fs) * sp.gain;
        l *= static_cast<float>(gLow / aL);
        m *= static_cast<float>(gMid / aM);
        h *= static_cast<float>(gHigh / aH);
    }
}

}  // namespace

float Fdn::random01() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) / 16777216.0f;
}

void Fdn::nextModSegment(Line& L) {
    // Wander to a new random offset over 0.15 .. 0.5 s. The slew is at most a
    // few samples per second: a pitch deviation far below what is audible even
    // on sustained tones, while every mode drifts by a few hertz.
    const float target = (2.0f * random01() - 1.0f) * L.modDepth;
    L.modCount = static_cast<int>((0.15f + 0.35f * random01()) * fs_);
    L.modStep = (target - L.modCur) / static_cast<float>(L.modCount);
}

void Fdn::init(const FdnParams& p) {
    nCh_ = ambiChannels(std::min(p.ambiOrder, kMaxAmbiOrder));
    fs_ = p.sampleRate;
    const float fs = fs_;
    rng_ = 0x9e3779b9u;

    // Line lengths: log-spaced between 0.6x and 2.8x the mean free path with
    // a little jitter, then the nearest prime, so they are mutually prime and
    // the echo pattern never repeats. The spread keeps the modal density high
    // (the lines sum to ~50x the mean free path) while the shortest lines and
    // the input allpasses give an early onset.
    const float base = std::max(p.meanFreePathSeconds * fs, 64.0f);
    for (int i = 0; i < kLines; ++i) {
        Line& L = lines_[i];
        const float frac = static_cast<float>(i) / (kLines - 1);
        const float jitter = 1.0f + 0.04f * (random01() - 0.5f);
        const float ratio = 0.7f * std::pow(3.2f / 0.7f, frac) * jitter;
        L.length = nearestPrime(std::max(static_cast<int>(base * ratio), 16));
        // Modulation on every other line, +-0.4 % of the length (1 .. 6 samples).
        L.modDepth = p.modulation ? clamp(0.006f * L.length, 1.0f, 8.0f) : 0.0f;
        int size = 1;
        while (size < L.length + 12) size <<= 1;
        L.buf.assign(static_cast<size_t>(size), 0.0f);
        L.mask = size - 1;
        L.write = 0;
        L.modCur = L.modStep = 0;
        L.modCount = 0;
    }

    // Input signs: a shuffled zero-sum +-1 pattern, so the W channel does not
    // simply sum 32 copies of the input.
    for (int i = 0; i < kLines; ++i) lines_[i].inputGain = i < kLines / 2 ? 1.0f : -1.0f;
    for (int i = kLines - 1; i > 0; --i) std::swap(lines_[i].inputGain, lines_[static_cast<int>(random01() * (i + 1)) % (i + 1)].inputGain);

    // Per-line absorption so each band decays by 60 dB in its RT60:
    // gain per pass g = 10^(-3 * L / (fs * T60)).
    auto lineGain = [&](int len, float t60) {
        t60 = std::max(t60, 0.05f);
        return std::pow(10.0f, -3.0f * static_cast<float>(len) / (fs * t60));
    };
    for (int i = 0; i < kLines; ++i) {
        Line& L = lines_[i];
        designAbsorption(L.absorb, lineGain(L.length, p.rt60Low), lineGain(L.length, p.rt60Mid), lineGain(L.length, p.rt60High), fs);
        L.absorb.reset();
    }

    // Input diffusion: four allpasses, 1.9 to 12.7 ms.
    const float apMs[kAllpasses] = {1.9f, 4.3f, 7.9f, 12.7f};
    const float apG[kAllpasses] = {0.62f, 0.58f, 0.54f, 0.5f};
    for (int k = 0; k < kAllpasses; ++k) ap_[k].init(nearestPrime(static_cast<int>(apMs[k] * 0.001f * fs)), apG[k]);

    const int pre = static_cast<int>(std::max(p.preDelaySeconds, 0.0f) * fs);
    preDelay_.assign(static_cast<size_t>(pre), 0.0f);
    prePos_ = 0;

    // Output directions: a Fibonacci sphere with a little jitter, so the field
    // is uniform without any two lines sharing a symmetric pair of directions.
    for (int i = 0; i < kLines; ++i) {
        const float y = clamp(1.0f - 2.0f * (i + 0.5f) / kLines + 0.03f * (random01() - 0.5f), -1.0f, 1.0f);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = i * 2.3999632f + 0.2f * (random01() - 0.5f);  // golden angle
        const Vec3 dir{r * std::cos(phi), y, r * std::sin(phi)};
        lines_[i].sh.fill(0.0f);
        encodeDirection(dir, std::min(p.ambiOrder, kMaxAmbiOrder), lines_[i].sh.data());
    }

    // Normalise: run an impulse through a copy and measure the W-channel energy
    // so that a unit impulse in gives unit-energy omni reverb out.
    outputScale_ = 1.0f;
    Fdn probe = *this;
    probe.reset();
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
    for (auto& L : lines_) {
        std::fill(L.buf.begin(), L.buf.end(), 0.0f);
        L.write = 0;
        L.absorb.reset();
        L.modCur = L.modStep = 0;
        L.modCount = 0;
        L.apState = 0;
    }
    std::fill(preDelay_.begin(), preDelay_.end(), 0.0f);
    prePos_ = 0;
    for (auto& a : ap_) a.reset();
    rng_ = 0x2545f491u;  // the modulation path restarts identically after a reset
}

}  // namespace sp::dsp
