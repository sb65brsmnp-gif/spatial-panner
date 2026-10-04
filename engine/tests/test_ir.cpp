// Impulse-response reverb: the WAV reader, the resampler, the IR convolution
// reverb's channel interpretations and latency, and its place in the renderer.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "dsp/IrReverb.h"
#include "dsp/Resampler.h"
#include "dsp/WavReader.h"
#include "sp/Renderer.h"

using namespace sp;
using namespace sp::dsp;
using Catch::Approx;

namespace {

constexpr float kFs = 48000;

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("sp_test_") + name)).string();
}

// Tiny WAV writer: 16/24-bit PCM or 32-bit float, interleaved.
void writeWav(const std::string& path, const std::vector<std::vector<float>>& ch, int rate, int bits, bool asFloat) {
    const int nch = static_cast<int>(ch.size()), bytes = bits / 8;
    const uint32_t frames = static_cast<uint32_t>(ch[0].size()), dataBytes = frames * nch * bytes;
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(asFloat ? 3 : 1); u16(static_cast<uint16_t>(nch)); u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * nch * bytes)); u16(static_cast<uint16_t>(nch * bytes)); u16(static_cast<uint16_t>(bits));
    out.write("data", 4); u32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < nch; ++c) {
            const float v = ch[c][i];
            if (asFloat) { out.write(reinterpret_cast<const char*>(&v), 4); }
            else if (bits == 16) { const int16_t s = static_cast<int16_t>(std::lrint(v * 32767)); out.write(reinterpret_cast<const char*>(&s), 2); }
            else { const int32_t s = static_cast<int32_t>(std::lrint(v * 8388607)); out.write(reinterpret_cast<const char*>(&s), 3); }
        }
}

std::vector<float> decayingNoise(int n, float t60, unsigned seed) {
    std::vector<float> v(n);
    unsigned x = seed;
    for (int i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * std::pow(10.0f, -3.0f * i / (kFs * t60));
    }
    return v;
}

struct BusOut {
    std::vector<std::vector<float>> ch;  // [ambi channel][sample]
};

// Runs `input` through an IrReverb in sub-blocks of 32 with a fixed head orientation.
BusOut run(IrReverb& r, const std::vector<float>& input, const Quat& head, int order) {
    const int B = 32, nCh = ambiChannels(order);
    BusOut o;
    o.ch.assign(nCh, std::vector<float>(input.size(), 0.0f));
    std::vector<float> bus(static_cast<size_t>(nCh) * B);
    for (size_t pos = 0; pos + B <= input.size(); pos += B) {
        std::fill(bus.begin(), bus.end(), 0.0f);
        r.process(input.data() + pos, head, bus.data());
        for (int c = 0; c < nCh; ++c)
            for (int i = 0; i < B; ++i) o.ch[c][pos + i] = bus[static_cast<size_t>(c) * B + i];
    }
    return o;
}

double energy(const std::vector<float>& v) {
    double e = 0;
    for (float x : v) e += static_cast<double>(x) * x;
    return e;
}

int firstNonZero(const std::vector<float>& v) {
    for (size_t i = 0; i < v.size(); ++i)
        if (std::fabs(v[i]) > 1e-7f) return static_cast<int>(i);
    return -1;
}

IrReverbSpec spec(int order = 1) {
    IrReverbSpec s;
    s.sampleRate = kFs;
    s.subBlock = 32;
    s.blockSize = 256;
    s.ambiOrder = order;
    return s;
}

}  // namespace

TEST_CASE("WAV reader: PCM and float files round trip, header probe matches") {
    std::vector<std::vector<float>> st(2, std::vector<float>(1000));
    for (int i = 0; i < 1000; ++i) { st[0][i] = 0.5f * std::sin(0.05f * i); st[1][i] = -0.25f * std::cos(0.03f * i); }
    const std::string p16 = tempPath("pcm16.wav"), p24 = tempPath("pcm24.wav"), pf = tempPath("float.wav");
    writeWav(p16, st, 44100, 16, false);
    writeWav(p24, st, 96000, 24, false);
    writeWav(pf, {st[0]}, 48000, 32, true);

    const WavData a = readWav(p16);
    REQUIRE(a.info.channels == 2);
    CHECK(a.info.sampleRate == 44100);
    CHECK(a.info.frames == 1000);
    CHECK(a.info.bitsPerSample == 16);
    for (int i = 0; i < 1000; i += 97) {
        CHECK(a.channels[0][i] == Approx(st[0][i]).margin(1.0 / 32767));
        CHECK(a.channels[1][i] == Approx(st[1][i]).margin(1.0 / 32767));
    }
    const WavData b = readWav(p24);
    CHECK(b.info.sampleRate == 96000);
    for (int i = 0; i < 1000; i += 97) CHECK(b.channels[1][i] == Approx(st[1][i]).margin(1.0 / 8388607));
    const WavData c = readWav(pf);
    REQUIRE(c.info.channels == 1);
    CHECK(c.info.isFloat);
    for (int i = 0; i < 1000; i += 97) CHECK(c.channels[0][i] == Approx(st[0][i]).margin(1e-7));

    const WavInfo info = readWavInfo(p24);
    CHECK(info.channels == 2);
    CHECK(info.frames == 1000);
    CHECK(info.seconds() == Approx(1000.0 / 96000));
    CHECK_THROWS(readWav(tempPath("does_not_exist.wav")));
    std::ofstream(tempPath("junk.wav")) << "this is not a wav file at all";
    CHECK_THROWS(readWav(tempPath("junk.wav")));
}

TEST_CASE("Resampler keeps a tone's frequency and amplitude") {
    const int n = 44100;
    std::vector<float> in(n);
    for (int i = 0; i < n; ++i) in[i] = std::sin(2 * kPi * 1000 * i / 44100.0f);
    const auto out = resample(in, 44100, 48000);
    CHECK(out.size() == Approx(48000).margin(2));
    int crossings = 0;
    float peak = 0;
    for (size_t i = 2400; i < out.size() - 2400; ++i) {
        if ((out[i - 1] < 0) != (out[i] < 0)) ++crossings;
        peak = std::max(peak, std::fabs(out[i]));
    }
    const double seconds = (out.size() - 4800) / 48000.0;
    CHECK(crossings / 2.0 / seconds == Approx(1000).epsilon(0.005));
    CHECK(peak == Approx(1.0).margin(0.01));
    // Downsampling keeps the band-limited content too.
    const auto down = resample(out, 48000, 32000);
    CHECK(down.size() == Approx(32000).margin(2));
    float pk = 0;
    for (size_t i = 1600; i < down.size() - 1600; ++i) pk = std::max(pk, std::fabs(down[i]));
    CHECK(pk == Approx(1.0).margin(0.02));
}

TEST_CASE("IR reverb: a mono IR becomes a unit-energy diffuse field after the block latency") {
    std::vector<std::vector<float>> ir(1, std::vector<float>(2000, 0.0f));
    ir[0][0] = 0.7f;  // any scale: normalised away
    IrReverb r;
    r.init(ir, kFs, 0, 0.0f, spec(1));
    CHECK(r.kind() == IrKind::Mono);
    CHECK(r.channels() == 1);
    CHECK(r.latency() == 256 - 32);
    std::vector<float> click(48000, 0.0f);
    click[0] = 1.0f;
    const auto o = run(r, click, Quat::identity(), 1);
    CHECK(firstNonZero(o.ch[0]) >= 224);                 // the block latency...
    CHECK(firstNonZero(o.ch[0]) < 224 + 0.020 * kFs);     // ...plus at most the decorrelators' length
    const double eW = energy(o.ch[0]);
    WARN("Mono IR: W energy " << eW << " (unit in), first-order X/Y/Z re W: " << 10 * std::log10(energy(o.ch[3]) / eW) << " / "
         << 10 * std::log10(energy(o.ch[1]) / eW) << " / " << 10 * std::log10(energy(o.ch[2]) / eW) << " dB");
    CHECK(10 * std::log10(eW) == Approx(0.0).margin(0.1));
    // Diffuse: the directional channels carry energy in every direction (a
    // single plane wave would put all 3 (N3D) into one axis and none elsewhere).
    for (int c = 1; c < 4; ++c) {
        CHECK(energy(o.ch[c]) > 0.1 * eW);
        CHECK(energy(o.ch[c]) < 2.5 * eW);
    }
}

TEST_CASE("IR reverb: a stereo IR gives broad plane waves at head-left and head-right") {
    std::vector<std::vector<float>> ir(2, std::vector<float>(1000, 0.0f));
    ir[0][0] = 1.0f;  // left only
    IrReverb r;
    r.init(ir, kFs, 0, 0.0f, spec(3));
    CHECK(r.kind() == IrKind::Stereo);
    std::vector<float> click(9600, 0.0f);
    click[0] = 1.0f;
    const auto o = run(r, click, Quat::identity(), 3);
    CHECK(o.ch[0][224] == Approx(1.0).margin(1e-4));           // W: unit energy, L alone
    CHECK(o.ch[1][224] == Approx(std::sqrt(3.0)).margin(1e-3));  // Y (left) positive, N3D
    CHECK(std::fabs(o.ch[2][224]) < 1e-5);                       // Z
    CHECK(std::fabs(o.ch[3][224]) < 1e-5);                       // X
    for (int c = 4; c < 16; ++c) CHECK(energy(o.ch[c]) < 1e-9);  // first order only: broad
    // A right-only IR mirrors it; equal L and R share the unit energy.
    std::vector<std::vector<float>> both(2, std::vector<float>(1000, 0.0f));
    both[0][0] = both[1][0] = 1.0f;
    IrReverb r2;
    r2.init(both, kFs, 0, 0.0f, spec(1));
    const auto o2 = run(r2, click, Quat::identity(), 1);
    CHECK(energy(o2.ch[0]) == Approx(2.0).margin(1e-3));  // W = L + R, each with energy 1/2: coherent sum
    CHECK(std::fabs(o2.ch[1][224]) < 1e-5);              // and no Y (centre)
}

TEST_CASE("IR reverb: an ambiX IR is world-fixed and rotates with the head") {
    // W + Y (SN3D): a source on the world's left.
    std::vector<std::vector<float>> ir(4, std::vector<float>(1000, 0.0f));
    ir[0][0] = 1.0f;
    ir[1][0] = 1.0f;
    IrReverb r;
    r.init(ir, kFs, 0, 0.0f, spec(1));
    CHECK(r.kind() == IrKind::AmbiX);
    CHECK(r.channels() == 4);
    std::vector<float> click(4800, 0.0f);
    click[0] = 1.0f;
    const auto front = run(r, click, Quat::identity(), 1);
    CHECK(front.ch[0][224] == Approx(1.0).margin(1e-4));
    CHECK(front.ch[1][224] == Approx(std::sqrt(3.0)).margin(1e-3));  // Y, N3D
    CHECK(std::fabs(front.ch[3][224]) < 1e-4);
    // Head turned 90 degrees to the left: the world-left source is now in front (X).
    IrReverb r2;
    r2.init(ir, kFs, 0, 0.0f, spec(1));
    const auto turned = run(r2, click, Quat::fromYawPitchRoll(degToRad(90.0f), 0, 0), 1);
    CHECK(turned.ch[3][224] == Approx(std::sqrt(3.0)).margin(1e-3));
    CHECK(std::fabs(turned.ch[1][224]) < 1e-3);
    // Forced interpretations and a gain trim.
    IrReverb mono;
    mono.init(ir, kFs, 1, -6.0f, spec(1));
    CHECK(mono.kind() == IrKind::Mono);
    const auto om = run(mono, click, Quat::identity(), 1);
    CHECK(10 * std::log10(energy(om.ch[0])) == Approx(-6.0).margin(1.5));
    CHECK_THROWS(mono.init(ir, kFs, 3, 0.0f, spec(1)));
    std::vector<std::vector<float>> two(2, std::vector<float>(100, 0.0f));
    two[0][0] = 1;
    CHECK_THROWS(mono.init(two, kFs, 4, 0.0f, spec(1)));
}

TEST_CASE("IR reverb: resampled IR keeps its length in seconds") {
    std::vector<std::vector<float>> ir(1, decayingNoise(24000, 0.5f, 3));  // 1 s at 24 kHz
    IrReverb r;
    r.init(ir, 24000, 0, 0.0f, spec(1));
    CHECK(r.seconds() == Approx(1.0).margin(0.02));
}

TEST_CASE("IR reverb: cost of a 3 s stereo IR at a 256-sample block") {
    std::vector<std::vector<float>> ir(2);
    ir[0] = decayingNoise(static_cast<int>(3 * kFs), 1.0f, 11);
    ir[1] = decayingNoise(static_cast<int>(3 * kFs), 1.0f, 12);
    IrReverb r;
    r.init(ir, kFs, 0, 0.0f, spec(3));
    const int n = static_cast<int>(2 * kFs);
    const auto in = decayingNoise(n, 100.0f, 5);
    std::vector<float> bus(16 * 32);
    const auto t0 = std::chrono::steady_clock::now();
    for (int pos = 0; pos + 32 <= n; pos += 32) r.process(in.data() + pos, Quat::identity(), bus.data());
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double load = 100.0 * secs / (n / kFs);
    WARN("IR reverb, 3 s stereo IR, block 256: " << load << " % of one core at 48 kHz");
    CHECK(load < 10.0);

    std::vector<std::vector<float>> ir4(4);
    for (int c = 0; c < 4; ++c) ir4[c] = decayingNoise(static_cast<int>(3 * kFs), 1.0f, 20 + c);
    IrReverb r4;
    r4.init(ir4, kFs, 0, 0.0f, spec(3));
    const auto t1 = std::chrono::steady_clock::now();
    for (int pos = 0; pos + 32 <= n; pos += 32) r4.process(in.data() + pos, Quat::identity(), bus.data());
    const double secs4 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    WARN("IR reverb, 3 s ambiX (4 ch) IR, block 256: " << 100.0 * secs4 / (n / kFs) << " % of one core");
    // Silent input after the tail has rung out costs next to nothing.
    std::vector<float> zeros(n, 0.0f);
    for (int k = 0; k < 2; ++k)
        for (int pos = 0; pos + 32 <= n; pos += 32) r.process(zeros.data() + pos, Quat::identity(), bus.data());
    const auto t2 = std::chrono::steady_clock::now();
    for (int pos = 0; pos + 32 <= n; pos += 32) r.process(zeros.data() + pos, Quat::identity(), bus.data());
    const double idle = 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count() / (n / kFs);
    WARN("IR reverb idle (silent input): " << idle << " % of one core");
    CHECK(idle < 0.2 * load + 0.5);
}

TEST_CASE("Renderer: an impulse response replaces the FDN at the calibrated level") {
    const std::string path = tempPath("room_ir.wav");
    std::vector<std::vector<float>> ir(2);
    ir[0] = decayingNoise(static_cast<int>(1.0f * kFs), 0.8f, 31);
    ir[1] = decayingNoise(static_cast<int>(1.0f * kFs), 0.8f, 32);
    writeWav(path, ir, 48000, 24, false);

    Scene s;
    s.room.type = RoomType::Box;
    s.room.size = {8, 5, 10};
    for (auto& m : s.room.materials) m = materials::byName("plaster");
    s.environment.airAbsorption = false;
    s.listener.staticPosition = {-0.6f, 2.1f, 1.0f};
    Layer l;
    l.position = {0.4f, 3.1f, -1.5f};
    s.layers = {l};
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;

    auto renderW = [&](const Scene& sc, Renderer::Stats* stats) {
        Renderer r(sc, cfg, 4);
        if (stats) *stats = r.stats();
        const int n = static_cast<int>(3 * kFs), lat = r.latencySamples();
        std::vector<float> click(n + lat, 0.0f);
        click[0] = 1.0f;
        std::vector<std::vector<float>> out(r.numOutputs(), std::vector<float>(n + lat, 0.0f));
        std::vector<float*> o(r.numOutputs());
        for (int pos = 0; pos < n + lat; pos += 256) {
            const int k = std::min(256, n + lat - pos);
            const float* in[1] = {click.data() + pos};
            for (int c = 0; c < r.numOutputs(); ++c) o[c] = out[c].data() + pos;
            r.process(in, o.data(), k, pos / kFs);
        }
        return std::vector<float>(out[0].begin() + lat, out[0].end());
    };
    Renderer::Stats fdnStats;
    const auto wFdn = renderW(s, &fdnStats);
    CHECK(fdnStats.irChannels == 0);

    s.room.impulseResponse.file = path;
    Renderer::Stats irStats;
    const auto wIr = renderW(s, &irStats);
    CHECK(irStats.irChannels == 2);
    CHECK(irStats.irSeconds == Approx(1.0).margin(0.02));
    CHECK(irStats.reflectionLatency == 224);
    CHECK(irStats.note.empty());
    // Same reverberant energy (everything after the direct sound, which
    // arrives at 8 ms: images plus tail) either way.
    auto late = [&](const std::vector<float>& w) {
        double e = 0;
        for (size_t i = static_cast<size_t>(0.012 * kFs); i < w.size(); ++i) e += static_cast<double>(w[i]) * w[i];
        return e;
    };
    WARN("Reverberant energy, IR vs FDN: " << 10 * std::log10(late(wIr) / late(wFdn)) << " dB");
    CHECK(10 * std::log10(late(wIr) / late(wFdn)) == Approx(0.0).margin(1.0));
    // The trim (tail only, so the unchanged images do not dilute it).
    Scene tailOnly = s;
    tailOnly.room.reflectionsEnabled = false;
    const auto wTail = renderW(tailOnly, nullptr);
    tailOnly.room.impulseResponse.gainDb = -6;
    const auto wTrim = renderW(tailOnly, nullptr);
    CHECK(10 * std::log10(late(wTrim) / late(wTail)) == Approx(-6.0).margin(0.3));

    // Live update: the IR block is part of the room, so a change rebuilds.
    Renderer r(s, cfg, 4);
    Scene same = s;
    CHECK(r.prepareUpdate(same) != nullptr);
    Scene changed = s;
    changed.room.impulseResponse.gainDb = -6;
    CHECK(r.prepareUpdate(changed) == nullptr);
    Scene off = s;
    off.room.impulseResponse.enabled = false;
    CHECK(r.prepareUpdate(off) == nullptr);
    Renderer rOff(off, cfg, 4);
    CHECK(rOff.stats().irChannels == 0);

    // A missing file falls back to the FDN with a note instead of failing.
    Scene bad = s;
    bad.room.impulseResponse.file = tempPath("missing_ir.wav");
    Renderer rb(bad, cfg, 4);
    CHECK(rb.stats().irChannels == 0);
    CHECK(!rb.stats().note.empty());
    const auto wBad = renderW(bad, nullptr);
    CHECK(10 * std::log10(late(wBad) / late(wFdn)) == Approx(0.0).margin(0.1));
}
