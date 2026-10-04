#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "sp/Renderer.h"

using namespace sp;
using Catch::Approx;

namespace {

struct Rendered {
    std::vector<std::vector<float>> out;
};

Rendered render(const Scene& scene, RenderConfig cfg, const std::vector<std::vector<float>>& inputs, int frames,
                int hostBlock = 512) {
    Renderer r(scene, cfg, frames / cfg.sampleRate + 1);
    Rendered res;
    res.out.assign(r.numOutputs(), std::vector<float>(frames + r.latencySamples(), 0.0f));
    std::vector<const float*> in(inputs.size());
    std::vector<float*> out(r.numOutputs());
    std::vector<float> zeros(hostBlock, 0.0f);
    int pos = 0;
    const int total = frames + r.latencySamples();
    while (pos < total) {
        const int n = std::min(hostBlock, total - pos);
        for (size_t i = 0; i < inputs.size(); ++i) in[i] = pos < frames ? inputs[i].data() + pos : zeros.data();
        for (int c = 0; c < r.numOutputs(); ++c) out[c] = res.out[c].data() + pos;
        r.process(in.data(), out.data(), n, pos / cfg.sampleRate);
        pos += n;
    }
    // Drop the latency.
    for (auto& ch : res.out) ch.erase(ch.begin(), ch.begin() + r.latencySamples());
    return res;
}

double energy(const std::vector<float>& v, size_t from = 0, size_t to = 0) {
    if (to == 0) to = v.size();
    double e = 0;
    for (size_t i = from; i < to; ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
}

Scene freeFieldScene() {
    Scene s;
    s.room.type = RoomType::None;
    s.environment.airAbsorption = false;
    s.listener.staticPosition = {0, 1.6f, 0};
    return s;
}

std::vector<float> noise(int n, unsigned seed = 1) {
    std::vector<float> v(n);
    unsigned x = seed;
    for (int i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * 0.5f;
    }
    return v;
}

}  // namespace

TEST_CASE("Binaural: a source on the left is louder in the left ear") {
    Scene s = freeFieldScene();
    Layer l;
    l.position = {-2, 1.6f, 0};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.hrtfPath = SP_TEST_HRTF;
    const int n = 48000;
    auto r = render(s, cfg, {noise(n)}, n);
    const double eL = energy(r.out[0], 4800), eR = energy(r.out[1], 4800);
    CHECK(10 * std::log10(eL / eR) > 3.0);
}

TEST_CASE("Binaural: the engine runs with odd host block sizes identically") {
    Scene s = freeFieldScene();
    Layer l;
    l.position = {1, 1.6f, -2};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.hrtfPath = SP_TEST_HRTF;
    const int n = 48000 / 2;
    const auto in = noise(n);
    auto a = render(s, cfg, {in}, n, 512);
    auto b = render(s, cfg, {in}, n, 37);
    double diff = 0;
    for (int i = 0; i < n; ++i) diff += std::fabs(a.out[0][i] - b.out[0][i]);
    CHECK(diff / n < 1e-6);
}

TEST_CASE("Distance halves the level with inverse-distance rolloff") {
    Scene s = freeFieldScene();
    Layer l;
    l.position = {0, 1.6f, -2};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 24000;
    // A tone, so the fractional-delay interpolation (which slightly colours
    // the top octave of white noise) does not affect the energy comparison.
    std::vector<float> in(n);
    for (int i = 0; i < n; ++i) in[i] = 0.5f * std::sin(2 * kPi * 1000 * i / 48000.0f);
    auto near = render(s, cfg, {in}, n);
    s.layers[0].position = {0, 1.6f, -4};
    auto far = render(s, cfg, {in}, n);
    const double ratio = 10 * std::log10(energy(near.out[0], 2400) / energy(far.out[0], 2400));
    CHECK(ratio == Approx(6.02).margin(0.3));
}

TEST_CASE("Ambisonics output encodes direction (W, Y, X) correctly") {
    Scene s = freeFieldScene();
    Layer l;
    l.position = {-3, 1.6f, 0};  // hard left, 3 m
    l.referenceDistance = 3;     // so the direct gain is 1
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 24000;
    const auto in = noise(n);
    auto r = render(s, cfg, {in}, n);
    // ambiX: W = s / 1, Y (left) = s (SN3D), X = 0, Z = 0.
    const double eW = energy(r.out[0], 2400), eY = energy(r.out[1], 2400), eZ = energy(r.out[2], 2400),
                 eX = energy(r.out[3], 2400);
    CHECK(eY / eW == Approx(1.0).margin(0.02));
    CHECK(eX / eW < 1e-3);
    CHECK(eZ / eW < 1e-3);
    // Correlation sign: a left source gives positive Y.
    double corr = 0;
    for (int i = 2400; i < n; ++i) corr += r.out[0][i] * r.out[1][i];
    CHECK(corr > 0);
}

TEST_CASE("Speakers: a front-left source lands on L in 7.1.4") {
    Scene s = freeFieldScene();
    Layer l;
    l.position = {-std::sin(degToRad(30.0f)) * 2, 1.6f, -std::cos(degToRad(30.0f)) * 2};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Speakers;
    cfg.layout = SpeakerLayout::preset("7.1.4");
    const int n = 24000;
    auto r = render(s, cfg, {noise(n)}, n);
    REQUIRE(r.out.size() == 12);
    const double eL = energy(r.out[0], 2400);
    double others = 0;
    for (int c = 1; c < 12; ++c) others += energy(r.out[c], 2400);
    CHECK(eL > 20 * others);
    CHECK(energy(r.out[3]) == 0.0);  // LFE silent
}

TEST_CASE("Speakers: stereo and 5.1 (planar) layouts render without error") {
    Scene s = freeFieldScene();
    Layer l;
    l.position = {1, 1.6f, -2};
    s.layers.push_back(l);
    for (const char* name : {"stereo", "5.1", "quad"}) {
        RenderConfig cfg;
        cfg.mode = OutputMode::Speakers;
        cfg.layout = SpeakerLayout::preset(name);
        const int n = 9600;
        auto r = render(s, cfg, {noise(n)}, n);
        double total = 0;
        for (auto& ch : r.out) total += energy(ch);
        CHECK(total > 0);
        for (auto& ch : r.out)
            for (float v : ch) REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("Binaural: reflections and reverb reach both ears and keep the source side") {
    Scene s;
    s.room.type = RoomType::Box;
    s.room.size = {8, 3, 10};
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.position = {-2.5f, 1.6f, -1};  // left
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.hrtfPath = SP_TEST_HRTF;
    const int n = 48000;
    std::vector<float> click(n, 0.0f);
    click[0] = 1.0f;
    auto r = render(s, cfg, {click}, n);
    for (int e = 0; e < 2; ++e)
        for (float v : r.out[e]) REQUIRE(std::isfinite(v));
    // The late tail (after 0.4 s) must exist in both ears, be similar in level...
    const double lateL = energy(r.out[0], 19200, 48000), lateR = energy(r.out[1], 19200, 48000);
    CHECK(lateL > 1e-9);
    CHECK(lateR > 1e-9);
    CHECK(std::fabs(10 * std::log10(lateL / lateR)) < 3.0);
    // ...and the early part must favour the left ear.
    const double earlyL = energy(r.out[0], 0, 2400), earlyR = energy(r.out[1], 0, 2400);
    CHECK(10 * std::log10(earlyL / earlyR) > 2.0);
    // The tail decays.
    CHECK(energy(r.out[0], 9600, 14400) > energy(r.out[0], 38400, 43200) * 2);
}

TEST_CASE("Room reflections and reverb add a tail") {
    Scene s;
    s.room.type = RoomType::Box;
    s.room.size = {8, 3, 10};
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.position = {1, 1.6f, -3};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 48000;
    std::vector<float> click(n, 0.0f);
    click[0] = 1.0f;
    auto r = render(s, cfg, {click}, n);
    // Energy well after the direct arrival (which is ~9 ms) but inside the tail.
    const double late = energy(r.out[0], 48000 / 2, 48000 / 2 + 4800);
    CHECK(late > 1e-9);
    Scene dry = s;
    dry.room.type = RoomType::None;
    auto rd = render(dry, cfg, {click}, n);
    const double lateDry = energy(rd.out[0], 48000 / 2, 48000 / 2 + 4800);
    CHECK(lateDry < 1e-12);
    for (float v : r.out[0]) REQUIRE(std::isfinite(v));
}

TEST_CASE("Doppler: approaching listener raises the pitch, doppler = 0 does not") {
    Scene s = freeFieldScene();
    Layer l;
    l.position = {0, 1.6f, -200};
    s.layers.push_back(l);
    Path p;
    PathSegment seg;
    seg.type = SegmentType::Line;
    seg.points = {{0, 1.6f, 0}, {0, 1.6f, -150}};
    p.segments.push_back(seg);
    s.listener.paths.push_back(p);
    s.listener.speed.keys = {{0, 30, Easing::Linear}};  // 30 m/s towards the source
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 48000 * 2;
    std::vector<float> tone(n);
    for (int i = 0; i < n; ++i) tone[i] = std::sin(2 * kPi * 1000 * i / 48000.0f);
    auto count = [&](const std::vector<float>& w) {
        int c = 0;
        for (size_t i = 48000; i < w.size(); ++i)
            if ((w[i - 1] < 0) != (w[i] < 0)) ++c;
        return c / 2.0;  // Hz over the second half (1 s)
    };
    auto r = render(s, cfg, {tone}, n);
    const double measured = count(r.out[0]);
    const double expected = 1000.0 * (343.0 + 30.0) / 343.0;  // moving receiver
    CHECK(measured == Approx(expected).epsilon(0.01));

    s.layers[0].dopplerAmount = 0;
    auto r0 = render(s, cfg, {tone}, n);
    CHECK(count(r0.out[0]) == Approx(1000).epsilon(0.005));
}

TEST_CASE("Stereo layer: two inputs, left channel heard on the left, mono fold sums at the centre") {
    Scene s = freeFieldScene();
    Layer l;
    l.channels = 2;
    l.position = {0, 1.6f, -3};
    l.stereo.width = 4;  // left end at x = -2, right end at x = +2
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 24000;
    std::vector<float> tone(n), silence(n, 0.0f);
    for (int i = 0; i < n; ++i) tone[i] = 0.5f * std::sin(2 * kPi * 1000 * i / 48000.0f);

    Renderer r(s, cfg, 2);
    CHECK(r.numInputs() == 2);
    CHECK(r.numLayers() == 1);
    CHECK(r.inputIndex(0) == 0);

    // Only the left channel sounds: Y (ACN 1, +left) is positive, in phase with W.
    auto left = render(s, cfg, {tone, silence}, n);
    double wy = 0;
    for (int i = 2400; i < n; ++i) wy += static_cast<double>(left.out[0][i]) * left.out[1][i];
    CHECK(wy > 0);
    // Only the right channel: Y negative.
    auto right = render(s, cfg, {silence, tone}, n);
    wy = 0;
    for (int i = 2400; i < n; ++i) wy += static_cast<double>(right.out[0][i]) * right.out[1][i];
    CHECK(wy < 0);

    // Mono fold: the same tone in both channels plays from the centre at the
    // level of one channel (the ends each carry (L + R) / 4), so W matches a
    // mono layer with the same tone, and Y is about zero.
    Scene folded = s;
    folded.layers[0].stereo.mono = true;
    auto mono = render(folded, cfg, {tone, tone}, n);
    Scene single = s;
    single.layers[0].channels = 1;
    auto ref = render(single, cfg, {tone}, n);
    const double dB = 10 * std::log10(energy(mono.out[0], 2400) / energy(ref.out[0], 2400));
    CHECK(dB == Approx(0).margin(0.3));
    CHECK(energy(mono.out[1], 2400) < 0.01 * energy(mono.out[0], 2400));
}

TEST_CASE("Stereo geometry: offsets and ends round trip") {
    Vec3 d = stereoOffset(4, 0, 0);
    CHECK(d.x == Approx(2));
    CHECK(d.z == Approx(0).margin(1e-6));
    d = stereoOffset(4, 90, 0);  // right end turned towards -Z
    CHECK(d.x == Approx(0).margin(1e-6));
    CHECK(d.z == Approx(-2));
    d = stereoOffset(4, 30, 20);
    Vec3 centre{1, 1.6f, -2};
    Vec3 c;
    Layer::Stereo st;
    stereoFromEnds(centre - d, centre + d, c, st);
    CHECK(c.x == Approx(1));
    CHECK(st.width == Approx(4));
    CHECK(st.rotationDeg == Approx(30).margin(1e-3));
    CHECK(st.elevationDeg == Approx(20).margin(1e-3));
    Scene s;
    Layer a, b;
    b.channels = 2;
    s.layers = {a, b};
    CHECK(inputChannels(s) == 3);
}
