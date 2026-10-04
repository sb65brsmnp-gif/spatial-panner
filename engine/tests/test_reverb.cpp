// Late reverb quality: the FDN's impulse response must build echo density
// quickly, decay per band at the requested RT60, and have a noise-like late
// spectrum (no isolated modes, which are what sounds metallic).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "dsp/Fdn.h"
#include "dsp/Fft.h"

using namespace sp;
using namespace sp::dsp;
using Catch::Approx;

namespace {

constexpr float kFs = 48000;

// W-channel impulse response of an FDN, `seconds` long.
std::vector<float> fdnImpulse(Fdn& fdn, float seconds) {
    const int n = static_cast<int>(kFs * seconds);
    std::vector<float> w(n);
    float amb[kMaxAmbiChannels];
    for (int i = 0; i < n; ++i) {
        std::fill(amb, amb + kMaxAmbiChannels, 0.0f);
        fdn.process(i == 0 ? 1.0f : 0.0f, 1.0f, amb);
        w[i] = amb[0];
    }
    return w;
}

int onset(const std::vector<float>& h) {
    float peak = 0;
    for (float v : h) peak = std::max(peak, std::fabs(v));
    for (size_t i = 0; i < h.size(); ++i)
        if (std::fabs(h[i]) > 1e-3f * peak) return static_cast<int>(i);
    return 0;
}

// Normalised echo density (Abel & Huang 2007): the share of samples in a
// window that exceed the window's standard deviation, divided by what
// Gaussian noise gives (erfc(1/sqrt 2) = 0.3173). 1 = fully dense.
double echoDensity(const std::vector<float>& h, int start, int len) {
    double m2 = 0;
    for (int i = start; i < start + len; ++i) m2 += static_cast<double>(h[i]) * h[i];
    const double sd = std::sqrt(m2 / len);
    int above = 0;
    for (int i = start; i < start + len; ++i) above += std::fabs(h[i]) > sd;
    return (static_cast<double>(above) / len) / 0.3173;
}

// RBJ band-pass biquad (constant 0 dB peak), run forward over `x`.
std::vector<float> bandpass(const std::vector<float>& x, double f0, double Q) {
    const double w0 = 2 * M_PI * f0 / kFs, alpha = std::sin(w0) / (2 * Q), c = std::cos(w0);
    const double b0 = alpha, b1 = 0, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
    std::vector<float> y(x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double v = (b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1; x1 = x[i]; y2 = y1; y1 = v;
        y[i] = static_cast<float>(v);
    }
    return y;
}

// Reverberation time from the Schroeder backward-integrated decay, fitted
// between -5 and -35 dB (T30, extrapolated to 60 dB).
double rt60(const std::vector<float>& h) {
    std::vector<double> edc(h.size());
    double acc = 0;
    for (size_t i = h.size(); i-- > 0;) { acc += static_cast<double>(h[i]) * h[i]; edc[i] = acc; }
    const double top = 10 * std::log10(edc[0]);
    int i5 = -1, i35 = -1;
    for (size_t i = 0; i < edc.size(); ++i) {
        const double db = 10 * std::log10(edc[i] + 1e-30) - top;
        if (i5 < 0 && db <= -5) i5 = static_cast<int>(i);
        if (i35 < 0 && db <= -35) { i35 = static_cast<int>(i); break; }
    }
    REQUIRE(i5 >= 0);
    REQUIRE(i35 > i5);
    // Least squares over the segment.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const int n = i35 - i5;
    for (int i = i5; i < i35; ++i) {
        const double x = i / kFs, y = 10 * std::log10(edc[i] + 1e-30) - top;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);  // dB per second
    return -60.0 / slope;
}

// Standard deviation (dB) of the magnitude spectrum of h[from, to) around its
// smoothed trend, over 200 Hz .. 6 kHz. Gaussian noise gives about 5.6 dB
// (Rayleigh-distributed magnitudes); isolated resonances give more.
double spectralRoughnessDb(const std::vector<float>& h, int from, int to) {
    int n = 1;
    while (n < to - from) n <<= 1;
    RealFft fft(n);
    std::vector<float> x(n, 0.0f);
    for (int i = from; i < to; ++i) x[i - from] = h[i] * static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i - from) / (to - from)));
    std::vector<cfloat> X(fft.bins());
    fft.forward(x.data(), X.data());
    std::vector<double> db(fft.bins());
    for (int k = 0; k < fft.bins(); ++k) db[k] = 20 * std::log10(std::abs(X[k]) + 1e-12);
    const int k0 = static_cast<int>(200.0 * n / kFs), k1 = static_cast<int>(6000.0 * n / kFs);
    const int half = std::max(1, static_cast<int>(100.0 * n / kFs));  // +-100 Hz trend window
    double s = 0, s2 = 0;
    int cnt = 0;
    for (int k = k0; k < k1; ++k) {
        double t = 0;
        for (int j = k - half; j <= k + half; ++j) t += db[j];
        const double d = db[k] - t / (2 * half + 1);
        s += d; s2 += d * d; ++cnt;
    }
    return std::sqrt(s2 / cnt - (s / cnt) * (s / cnt));
}

std::vector<float> decayingNoise(int n, float t60, unsigned seed) {
    std::vector<float> v(n);
    unsigned x = seed;
    for (int i = 0; i < n; ++i) {
        // Box-Muller from two LCG draws.
        x = x * 1664525u + 1013904223u;
        const double u1 = (x >> 8) / 16777216.0 + 1e-9;
        x = x * 1664525u + 1013904223u;
        const double u2 = (x >> 8) / 16777216.0;
        const double g = std::sqrt(-2 * std::log(u1)) * std::cos(2 * M_PI * u2);
        v[i] = static_cast<float>(g * std::pow(10.0, -3.0 * i / (kFs * t60)));
    }
    return v;
}

FdnParams params(float low, float mid, float high) {
    FdnParams p;
    p.sampleRate = kFs;
    p.rt60Low = low;
    p.rt60Mid = mid;
    p.rt60High = high;
    p.meanFreePathSeconds = 0.0135f;  // the 12 x 3.5 x 16 m demo room
    p.preDelaySeconds = 0;
    p.ambiOrder = 3;
    return p;
}

}  // namespace

TEST_CASE("FDN: unit W energy and the echo density builds up quickly") {
    Fdn fdn;
    fdn.init(params(1.0f, 1.0f, 1.0f));
    const auto w = fdnImpulse(fdn, 2.0f);
    double e = 0;
    for (float v : w) e += static_cast<double>(v) * v;
    CHECK(e == Approx(1.0).epsilon(0.05));

    const int t0 = onset(w), win = static_cast<int>(0.02f * kFs);
    const double d20 = echoDensity(w, t0 + static_cast<int>(0.020f * kFs), win);
    const double d50 = echoDensity(w, t0 + static_cast<int>(0.050f * kFs), win);
    const double d100 = echoDensity(w, t0 + static_cast<int>(0.100f * kFs), win);
    const double d300 = echoDensity(w, t0 + static_cast<int>(0.300f * kFs), win);
    WARN("FDN normalised echo density at 20 / 50 / 100 / 300 ms after onset: " << d20 << " / " << d50 << " / " << d100 << " / " << d300);
    CHECK(d100 > d20);
    CHECK(d100 > 0.8);
    CHECK(d300 > 0.9);
}

TEST_CASE("FDN: late tail spectrum is noise-like (no isolated modes)") {
    Fdn fdn;
    fdn.init(params(1.0f, 1.0f, 1.0f));
    const auto w = fdnImpulse(fdn, 1.5f);
    const int from = static_cast<int>(0.3f * kFs), to = static_cast<int>(0.8f * kFs);
    const double fdnDb = spectralRoughnessDb(w, from, to);
    double refDb = 0;
    for (unsigned seed = 1; seed <= 4; ++seed) refDb += spectralRoughnessDb(decayingNoise(static_cast<int>(1.5f * kFs), 1.0f, seed), from, to) / 4;
    WARN("Late-tail spectral roughness: FDN " << fdnDb << " dB, decaying-noise reference " << refDb << " dB");
    CHECK(fdnDb < refDb * 1.15);
}

TEST_CASE("FDN: per-band decay matches the requested RT60 within 15 %") {
    Fdn fdn;
    fdn.init(params(1.6f, 1.0f, 0.6f));
    const auto w = fdnImpulse(fdn, 3.0f);
    // Band centres of the engine's low (125-250), mid (500-1k) and high (2k-4k) material bands.
    const double tLow = rt60(bandpass(w, 177, 1.4)), tMid = rt60(bandpass(w, 707, 1.4)), tHigh = rt60(bandpass(w, 2828, 1.4));
    WARN("FDN RT60 low/mid/high: " << tLow << " / " << tMid << " / " << tHigh << " s (asked 1.6 / 1.0 / 0.6)");
    CHECK(tLow == Approx(1.6).epsilon(0.15));
    CHECK(tMid == Approx(1.0).epsilon(0.15));
    CHECK(tHigh == Approx(0.6).epsilon(0.15));
}

TEST_CASE("FDN: cost per sample") {
    Fdn fdn;
    fdn.init(params(1.2f, 1.0f, 0.7f));
    const int n = static_cast<int>(kFs) * 2;
    float amb[kMaxAmbiChannels];
    double sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        std::fill(amb, amb + kMaxAmbiChannels, 0.0f);
        fdn.process(i % 97 == 0 ? 0.5f : 0.0f, 1.0f, amb);
        sink += amb[0];
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    WARN("FDN cost: " << secs / n * 1e9 << " ns per sample, " << 100.0 * secs / (n / kFs) << " % of one core at 48 kHz (sink " << sink << ")");
    CHECK(std::isfinite(sink));
}
