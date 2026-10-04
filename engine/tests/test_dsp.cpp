#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

#include "dsp/AirAbsorption.h"
#include "dsp/Convolver.h"
#include "dsp/DelayLine.h"
#include "dsp/Fdn.h"
#include "dsp/Fft.h"
#include "dsp/Filters.h"
#include "dsp/Hrtf.h"
#include "dsp/RoomAcoustics.h"

using namespace sp;
using namespace sp::dsp;
using Catch::Approx;

namespace {

// Magnitude response of a first-order section at f.
double magnitude(const FirstOrder& s, double f, double fs) {
    const std::complex<double> z = std::exp(std::complex<double>(0, -2 * M_PI * f / fs));
    const std::complex<double> h = (static_cast<double>(s.b0) + static_cast<double>(s.b1) * z) / (1.0 + static_cast<double>(s.a1) * z);
    return std::abs(h);
}

}  // namespace

TEST_CASE("Real FFT round trip") {
    RealFft fft(64);
    std::vector<float> x(64), y(64);
    for (int i = 0; i < 64; ++i) x[i] = std::sin(0.3f * i) + 0.1f * i;
    std::vector<cfloat> X(fft.bins());
    fft.forward(x.data(), X.data());
    fft.inverse(X.data(), y.data());
    for (int i = 0; i < 64; ++i) CHECK(y[i] == Approx(x[i]).margin(1e-4));
}

TEST_CASE("Partitioned convolution equals direct convolution") {
    ConvolverSpec spec;
    spec.blockSize = 32;
    spec.partitions = 4;
    RealFft fft(spec.fftSize());
    std::vector<float> h(spec.filterLength());
    for (size_t i = 0; i < h.size(); ++i) h[i] = std::cos(0.7f * i) * std::exp(-0.02f * i);
    PartitionedFilter filt(spec, fft);
    filt.set(h.data(), static_cast<int>(h.size()), fft);
    SpectralInput in(spec, fft);

    const int N = 32 * 10;
    std::vector<float> x(N), y(N), ref(N, 0.0f);
    for (int i = 0; i < N; ++i) x[i] = std::sin(0.11f * i) + (i % 37 == 0 ? 1.0f : 0.0f);
    for (int n = 0; n < N; ++n)
        for (size_t k = 0; k < h.size(); ++k)
            if (n >= static_cast<int>(k)) ref[n] += h[k] * x[n - k];

    std::vector<cfloat> acc(spec.bins());
    for (int b = 0; b < N / 32; ++b) {
        in.push(x.data() + b * 32, fft);
        std::fill(acc.begin(), acc.end(), cfloat{});
        convolveAccumulate(in, filt, spec.partitions, acc.data());
        spectrumToBlock(acc.data(), fft, 32, y.data() + b * 32);
    }
    for (int n = 0; n < N; ++n) CHECK(y[n] == Approx(ref[n]).margin(1e-3));
}

TEST_CASE("Delay line fractional read reproduces a delayed sine") {
    DelayLine d;
    d.init(1000);
    const float delay = 100.37f;
    const float w = 0.05f;
    for (int i = 0; i < 2000; ++i) {
        d.write(std::sin(w * i));
        if (i > 300) {
            const float expect = std::sin(w * (i - delay));
            CHECK(d.read(delay) == Approx(expect).margin(2e-4));
        }
    }
}

TEST_CASE("Moving read head produces the expected Doppler shift") {
    // A source approaching at v: the delay shrinks at rate v/c per second,
    // so the observed frequency is f * c / (c - v).
    const float fs = 48000, c = 343, v = 30, f = 1000;
    DelayLine d;
    d.init(static_cast<int>(fs * 2));
    float delay = 1.0f * fs;  // 343 m away
    const float dDelay = -(v / c);  // samples per sample
    std::vector<float> out;
    for (int i = 0; i < static_cast<int>(3 * fs); ++i) {
        d.write(std::sin(2 * kPi * f * i / fs));
        out.push_back(d.read(delay));
        delay += dDelay;
    }
    // Count zero crossings in the last second (the first second is the
    // initial propagation delay, i.e. silence).
    int crossings = 0;
    for (size_t i = out.size() - static_cast<size_t>(fs) + 1; i < out.size(); ++i)
        if ((out[i - 1] < 0) != (out[i] < 0)) ++crossings;
    const float measured = crossings / 2.0f;  // Hz over 1 s
    const float expected = f * c / (c - v);
    CHECK(measured == Approx(expected).epsilon(0.01));
}

TEST_CASE("ISO 9613-1 air absorption has the right magnitude") {
    // 20 C, 50 % RH, 1 atm: about 0.02 dB/m at 1 kHz, ~0.1 dB/m at 4 kHz,
    // roughly 0.5 dB/m at 10 kHz (published tables, within tolerance).
    const float a1k = isoAirAttenuationDbPerMetre(1000, 20, 50, 101.325f);
    const float a4k = isoAirAttenuationDbPerMetre(4000, 20, 50, 101.325f);
    const float a10k = isoAirAttenuationDbPerMetre(10000, 20, 50, 101.325f);
    CHECK(a1k == Approx(0.005).margin(0.004));
    CHECK(a4k == Approx(0.03).margin(0.03));
    CHECK(a10k > 0.1f);
    CHECK(a10k < 0.4f);
    CHECK(a10k > a4k);
    CHECK(a4k > a1k);
}

TEST_CASE("Air absorption filter fit matches the ISO curve at distance") {
    Environment env;
    const float fs = 48000;
    AirAbsorptionTable table(env, fs, 300);
    auto filterDb = [&](const AirFilter& af, double f) {
        double m = 1;
        for (const auto& s : af.s) m *= magnitude(s, f, fs);
        return -20 * std::log10(m);
    };
    for (float d : {20.0f, 100.0f, 250.0f}) {
        AirFilter af;
        table.apply(d, af);
        for (float f : {2000.0f, 5000.0f, 10000.0f}) {
            // Beyond ~24 dB of attenuation the band is gone either way; the
            // fit deliberately spends its accuracy below that.
            const double got = std::min(filterDb(af, f), 24.0);
            const double want = std::min(
                isoAirAttenuationDbPerMetre(f, env.temperatureC, env.relativeHumidity, env.pressureKPa) * d, 24.0f);
            CHECK(got == Approx(want).margin(std::max(1.5, 0.25 * want)));
        }
    }
    // Negligible at 1 m.
    AirFilter af;
    table.apply(1.0f, af);
    CHECK(filterDb(af, 10000) < 1.0);
}

TEST_CASE("Shelf pair hits its three band targets") {
    ShelfPair sp;
    const float fs = 48000;
    sp.set(0.9f, 0.6f, 0.3f, fs);
    auto mag = [&](double f) {
        return magnitude(sp.low, f, fs) * magnitude(sp.high, f, fs) * sp.gain;
    };
    CHECK(mag(60) == Approx(0.9).margin(0.05));
    CHECK(mag(800) == Approx(0.6).margin(0.08));
    CHECK(mag(12000) == Approx(0.3).margin(0.05));
}

TEST_CASE("Image sources: order 2 box gives 24 images with correct geometry") {
    Room room;
    room.type = RoomType::Box;
    room.size = {6, 3, 8};
    room.origin = {0, 0, 0};
    const Vec3 src{1, 1.5f, -2};
    const auto imgs = computeImages(room, src, 2);
    REQUIRE(imgs.size() == 25);
    CHECK(imgs[0].order == 0);
    // Image across the +X wall (x = 3): x' = 6 - 1 = 5.
    bool found = false;
    for (const auto& im : imgs) {
        if (im.index == std::array<int, 3>{1, 0, 0}) {
            found = true;
            CHECK(im.position.x == Approx(5).margin(1e-5));
            CHECK(im.position.y == Approx(1.5).margin(1e-5));
            CHECK(im.wallHits[WallPosX] == 1);
        }
        if (im.index == std::array<int, 3>{0, -1, 0}) {
            CHECK(im.position.y == Approx(-1.5).margin(1e-5));
            CHECK(im.wallHits[WallNegY] == 1);
        }
        if (im.index == std::array<int, 3>{-2, 0, 0}) {
            CHECK(im.position.x == Approx(1 - 12).margin(1e-5));
            CHECK(im.wallHits[WallNegX] == 1);
            CHECK(im.wallHits[WallPosX] == 1);
        }
    }
    CHECK(found);
    int order1 = 0, order2 = 0;
    for (const auto& im : imgs) { order1 += im.order == 1; order2 += im.order == 2; }
    CHECK(order1 == 6);
    CHECK(order2 == 18);
}

TEST_CASE("Eyring RT60 for a plausible room") {
    Room room;
    room.size = {8, 3, 10};
    for (auto& m : room.materials) m = materials::byName("plaster");
    Environment env;
    const RoomStats st = computeRoomStats(room, env);
    CHECK(st.volume == Approx(240));
    CHECK(st.meanFreePath == Approx(4 * 240 / 268.0).margin(1e-3));
    // Plaster is fairly reflective: expect a mid RT60 around 1-2 s.
    const float mid = bandAverage(st.rt60, 2, 3);
    CHECK(mid > 0.8f);
    CHECK(mid < 3.0f);
}

TEST_CASE("FDN decays by 60 dB in about its RT60") {
    FdnParams p;
    p.sampleRate = 48000;
    p.rt60Low = p.rt60Mid = p.rt60High = 1.0f;
    p.meanFreePathSeconds = 0.012f;
    p.preDelaySeconds = 0;
    p.ambiOrder = 1;
    Fdn fdn;
    fdn.init(p);
    const int n = 48000 * 2;
    std::vector<float> w(n);
    float amb[16];
    for (int i = 0; i < n; ++i) {
        std::fill(amb, amb + 16, 0.0f);
        fdn.process(i == 0 ? 1.0f : 0.0f, 1.0f, amb);
        w[i] = amb[0];
    }
    // Unit energy normalisation.
    double e = 0;
    for (float v : w) e += static_cast<double>(v) * v;
    CHECK(e == Approx(1.0).epsilon(0.05));
    // Energy in 100 ms windows: slope should be about -60 dB/s.
    auto winDb = [&](int start) {
        double s = 0;
        for (int i = start; i < start + 4800; ++i) s += static_cast<double>(w[i]) * w[i];
        return 10 * std::log10(s + 1e-30);
    };
    const double slope = (winDb(48000) - winDb(14400)) / 0.7;  // dB per second between 0.3 s and 1.0 s
    CHECK(slope == Approx(-60).margin(10));
}

TEST_CASE("HRTF set loads and has interaural differences") {
    HrtfSet h;
    h.load(SP_TEST_HRTF, 48000);
    REQUIRE(h.loaded());
    CHECK(h.filterLength() == 256);
    CHECK(h.numMeasurements() > 1000);
    std::vector<float> l(h.filterLength()), r(h.filterLength());
    // Source hard left: the left ear must be louder.
    h.getFilter({-1, 0, 0}, l.data(), r.data());
    double el = 0, er = 0;
    for (int i = 0; i < h.filterLength(); ++i) { el += l[i] * l[i]; er += r[i] * r[i]; }
    CHECK(el > er * 2);
    // ...and earlier.
    auto onset = [&](const std::vector<float>& ir) {
        float peak = 0;
        for (float v : ir) peak = std::max(peak, std::fabs(v));
        for (int i = 0; i < static_cast<int>(ir.size()); ++i)
            if (std::fabs(ir[i]) > 0.1f * peak) return i;
        return 0;
    };
    CHECK(onset(l) < onset(r));
    // Front: nearly symmetric.
    h.getFilter({0, 0, -1}, l.data(), r.data());
    el = er = 0;
    for (int i = 0; i < h.filterLength(); ++i) { el += l[i] * l[i]; er += r[i] * r[i]; }
    CHECK(std::fabs(10 * std::log10(el / er)) < 1.5);
}
