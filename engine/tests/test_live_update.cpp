// Live scene updates (Renderer::prepareUpdate / applyUpdate) and the scene
// analysis the editor draws from.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <vector>

#include "sp/Renderer.h"
#include "sp/SceneAnalysis.h"
#include "sp/SceneJson.h"

using namespace sp;
using Catch::Approx;

namespace {

Scene freeField() {
    Scene s;
    s.room.type = RoomType::None;
    s.environment.airAbsorption = false;
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.position = {-2, 1.6f, 0};
    s.layers.push_back(l);
    return s;
}

std::vector<float> noise(int n) {
    std::vector<float> v(n);
    unsigned x = 7;
    for (int i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * 0.5f;
    }
    return v;
}

// Renders `frames` of one layer, calling `atBlock(r, blockIndex)` before each 256-frame block.
std::vector<std::vector<float>> run(Renderer& r, const std::vector<float>& in, int frames,
                                    const std::function<void(Renderer&, int)>& atBlock) {
    const int block = 256;
    std::vector<std::vector<float>> out(r.numOutputs(), std::vector<float>(frames, 0.0f));
    std::vector<float*> o(r.numOutputs());
    for (int pos = 0, b = 0; pos < frames; pos += block, ++b) {
        atBlock(r, b);
        const int n = std::min(block, frames - pos);
        const float* i[1] = {in.data() + pos};
        for (int c = 0; c < r.numOutputs(); ++c) o[c] = out[c].data() + pos;
        r.process(i, o.data(), n, pos / r.config().sampleRate);
    }
    return out;
}

double energy(const std::vector<float>& v, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from; i < to; ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
}

RenderConfig stereoSpeakers() {
    RenderConfig cfg;
    cfg.mode = OutputMode::Speakers;
    cfg.layout = SpeakerLayout::preset("stereo");
    return cfg;
}

}  // namespace

TEST_CASE("prepareUpdate: what updates live and what needs a new renderer") {
    Scene s = freeField();
    RenderConfig cfg = stereoSpeakers();
    cfg.maxDistance = 50;  // headroom for edits, as the editor sets it
    Renderer r(s, cfg, 10);

    Scene moved = s;
    moved.layers[0].position = {2, 1.6f, -1};
    moved.layers[0].levelDb = -12;
    moved.listener.head.yawOffsetDeg = 30;
    CHECK(r.prepareUpdate(moved) != nullptr);

    Scene added = s;
    added.layers.push_back(Layer{});
    CHECK(r.prepareUpdate(added) == nullptr);

    Scene stereo = s;
    stereo.layers[0].channels = 2;  // one more input: a new renderer
    CHECK(r.prepareUpdate(stereo) == nullptr);

    Scene room = s;
    room.room.type = RoomType::Box;
    CHECK(r.prepareUpdate(room) == nullptr);

    Scene env = s;
    env.environment.temperatureC = 30;
    CHECK(r.prepareUpdate(env) == nullptr);

    Scene far = s;
    far.layers[0].position = {0, 1.6f, -400};  // beyond the delay line sized for the original scene
    CHECK(r.prepareUpdate(far) == nullptr);

    // Geometry the ray tracer is built from (objects, a mesh, transmission).
    Scene object = s;
    object.room.objects.push_back(SceneObject{});
    CHECK(r.prepareUpdate(object) == nullptr);

    Scene mesh = s;
    mesh.room.mesh.addBox({-1, 0, -1}, {1, 2, 1}, mesh.room.mesh.addMaterial(materials::byName("brick")), false);
    CHECK(r.prepareUpdate(mesh) == nullptr);

    Scene transmissive = s;
    transmissive.room.materials[WallNegX].transmission = {0.1f, 0.05f, 0.01f};
    CHECK(r.prepareUpdate(transmissive) == nullptr);
}

TEST_CASE("Applying an unchanged scene leaves the output bit-identical") {
    Scene s = freeField();
    RenderConfig cfg;
    cfg.hrtfPath = SP_TEST_HRTF;
    const int n = 24000;
    const auto in = noise(n);
    Renderer a(s, cfg, 1), b(s, cfg, 1);
    auto outA = run(a, in, n, [](Renderer&, int) {});
    auto outB = run(b, in, n, [&](Renderer& r, int blk) {
        if (blk % 10 == 5) {
            auto u = r.prepareUpdate(s);
            REQUIRE(u);
            r.applyUpdate(*u);
        }
    });
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) REQUIRE(outA[c][i] == outB[c][i]);
}

TEST_CASE("A layer moved live glides across to its new position") {
    Scene s = freeField();
    s.layers[0].position = {-2, 1.6f, -2};  // front left
    const int fs = 48000, n = fs;
    const auto in = noise(n);
    RenderConfig cfg = stereoSpeakers();
    cfg.maxDistance = 50;
    Renderer r(s, cfg, 2);
    Scene moved = s;
    moved.layers[0].position = {2, 1.6f, -2};  // front right
    auto upd = r.prepareUpdate(moved);
    REQUIRE(upd);
    auto out = run(r, in, n, [&](Renderer& rr, int blk) {
        if (blk == 94) rr.applyUpdate(*upd);  // ~0.5 s
    });
    // Before: left louder. Well after (0.25 s later): right louder.
    CHECK(energy(out[0], 12000, 22000) > 10 * energy(out[1], 12000, 22000));
    CHECK(energy(out[1], 36000, 46000) > 10 * energy(out[0], 36000, 46000));
    // No jump: the largest step between consecutive samples stays in the range of the noise itself.
    float maxIn = 0, maxOut = 0;
    for (int i = 1; i < n; ++i) maxIn = std::max(maxIn, std::fabs(in[i] - in[i - 1]));
    for (int c = 0; c < 2; ++c)
        for (int i = 24000; i < 30000; ++i) maxOut = std::max(maxOut, std::fabs(out[c][i] - out[c][i - 1]));
    CHECK(maxOut < maxIn);
}

TEST_CASE("Muting a layer live silences it") {
    Scene s = freeField();
    const int n = 24000;
    const auto in = noise(n);
    Renderer r(s, stereoSpeakers(), 1);
    Scene muted = s;
    muted.layers[0].mute = true;
    auto upd = r.prepareUpdate(muted);
    REQUIRE(upd);
    auto out = run(r, in, n, [&](Renderer& rr, int blk) {
        if (blk == 40) rr.applyUpdate(*upd);
    });
    CHECK(energy(out[0], 2000, 10000) > 1.0);
    CHECK(energy(out[0], 12000, n) < 1e-9);
}

TEST_CASE("Scene analysis: path samples, travel time and heading") {
    Scene s;
    s.duration = 10;
    Path p;
    p.segments.push_back({SegmentType::Line, {{0, 1.7f, 0}, {-10, 1.7f, 0}}});
    s.listener.paths.push_back(p);
    s.listener.speed.keys = {{0.0, 2.0f, Easing::Linear}};

    const SceneAnalysis a = analyzeScene(s, 0, 0.5);
    REQUIRE(a.paths.size() == 1);
    CHECK(a.paths[0].length == Approx(10).margin(1e-3));
    CHECK(a.paths[0].points.front().x == Approx(0));
    CHECK(a.paths[0].points.back().x == Approx(-10));
    REQUIRE(a.poses.size() == 21);
    CHECK(a.poses[5].distance == Approx(5).margin(1e-3));  // t = 2.5 s
    CHECK(a.poses[5].position.x == Approx(-5).margin(1e-3));
    CHECK(a.poses[5].yawDeg == Approx(90).margin(0.01));   // walking towards -X = facing left
    CHECK(a.arrivalTime == Approx(5.0));

    // Head keyframes in along-path mode turn the head relative to the walking direction.
    s.listener.head.keys = {{0.0, 30, -10, 0, Easing::Linear}};
    const SceneAnalysis b = analyzeScene(s, 0, 0.5);
    CHECK(b.poses[5].yawDeg == Approx(120).margin(0.01));
    CHECK(b.poses[5].pitchDeg == Approx(-10).margin(0.01));

    const std::string js = analysisToJson(b);
    CHECK(js.find("\"poses\"") != std::string::npos);
}
