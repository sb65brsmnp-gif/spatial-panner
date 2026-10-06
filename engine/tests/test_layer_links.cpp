// Layers linked to the listener or to another layer (Layer::link), and
// layers that play straight through (Layer::spatialize off).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "sp/Pose.h"
#include "sp/Renderer.h"
#include "sp/SceneAnalysis.h"
#include "sp/SceneJson.h"

using namespace sp;
using Catch::Approx;

namespace {

Path line(const Vec3& a, const Vec3& b) {
    Path p;
    PathSegment seg;
    seg.type = SegmentType::Line;
    seg.points = {a, b};
    p.segments.push_back(seg);
    return p;
}

// The listener walks 5 m along -Z then turns right and walks 5 m along +X,
// at 1 m/s from t = 0.
Scene cornerWalk() {
    Scene s;
    s.room.type = RoomType::None;
    Path p = line({0, 1.6f, 0}, {0, 1.6f, -5});
    PathSegment s2;
    s2.type = SegmentType::Line;
    s2.points = {{0, 1.6f, -5}, {5, 1.6f, -5}};
    p.segments.push_back(s2);
    s.listener.paths.push_back(p);
    s.listener.speed.keys = {{0, 1.0f, Easing::Linear}};
    return s;
}

}  // namespace

TEST_CASE("Linked to the listener: a layer keeps its place and bearing as the listener walks and turns") {
    Scene s = cornerWalk();
    Layer l;
    l.position = {1, 1.6f, 0};   // 1 m to the listener's right at the start
    l.link.to = kLinkListener;
    s.layers.push_back(l);
    PoseEvaluator e(s, 20);
    CHECK(e.layerMoves(0));
    // Walking straight: still 1 m to the right.
    Placement p = e.layerPlacement(0, 3.0);
    CHECK(p.linked);
    CHECK(p.position.x == Approx(1).margin(1e-3));
    CHECK(p.position.z == Approx(-3).margin(1e-3));
    CHECK(p.yawDeg == Approx(0).margin(0.1));
    // After the corner the listener heads +X; its right is now +Z.
    p = e.layerPlacement(0, 8.0);
    CHECK(p.position.x == Approx(3).margin(1e-3));
    CHECK(p.position.z == Approx(-4).margin(1e-3));
    CHECK(p.yawDeg == Approx(-90).margin(0.1));
    CHECK(p.leaderPosition.x == Approx(3).margin(1e-3));
    // The analysis carries it (no path of its own: one point, where it stands).
    const SceneAnalysis a = analyzeScene(s, 10.0);
    REQUIRE(a.layers.size() == 1);
    CHECK(a.layers[0].points.size() == 1);
    const auto& smp = a.layers[0].samples[static_cast<size_t>(std::lround(8.0 / a.layers[0].dt))];
    CHECK(smp.position.z == Approx(-4).margin(1e-2));
}

TEST_CASE("Unlinking leaves the layer where it is; linking again takes hold from there") {
    Scene s = cornerWalk();
    Layer l;
    l.position = {1, 1.6f, 0};
    l.link.to = kLinkListener;
    l.link.keys = {{3.0, false}, {8.0, true}};
    s.layers.push_back(l);
    PoseEvaluator e(s, 20);
    // Unlinked at 3 s: left at (1, -3) while the listener walks on.
    Placement p = e.layerPlacement(0, 6.0);
    CHECK_FALSE(p.linked);
    CHECK(p.position.x == Approx(1).margin(1e-3));
    CHECK(p.position.z == Approx(-3).margin(1e-3));
    // Linked again at 8 s, when the listener is at (3, -5) heading +X: the
    // layer is 2 m behind and 2 m to its left; it stays so.
    p = e.layerPlacement(0, 10.0);
    CHECK(p.linked);
    CHECK(p.position.x == Approx(3).margin(1e-3));
    CHECK(p.position.z == Approx(-3).margin(1e-3));
    CHECK(p.yawDeg == Approx(0).margin(0.1));   // no turn since the link took hold
}

TEST_CASE("A layer's own path is travelled in its leader's frame, and carries on after unlinking") {
    Scene s = cornerWalk();
    Layer l;
    l.position = {1, 1.6f, 0};
    l.motion.path = line({1, 1.6f, 0}, {1, 1.6f, -2});   // 2 m "forward" over 2 s, from t = 6
    l.motion.speed.keys = {{0, 1.0f, Easing::Linear}};
    l.motion.startTime = 6.0;
    l.link.to = kLinkListener;
    l.link.keys = {{7.0, false}};
    s.layers.push_back(l);
    PoseEvaluator e(s, 20);
    // At 7 s the listener is at (2, -5) heading +X; the layer's own travel so
    // far (1 m forward, now +X) puts it 1 m ahead and 1 m to the right.
    Placement p = e.layerPlacement(0, 7.0);
    CHECK(p.position.x == Approx(3).margin(1e-3));
    CHECK(p.position.z == Approx(-4).margin(1e-3));
    // Unlinked there, the second metre of its path continues in the world
    // frame (-Z) from where it was left.
    p = e.layerPlacement(0, 8.0);
    CHECK_FALSE(p.linked);
    CHECK(p.position.x == Approx(3).margin(1e-3));
    CHECK(p.position.z == Approx(-5).margin(1e-3));
}

TEST_CASE("Linked to a layer: the follower goes where the leader goes, through a chain") {
    Scene s;
    s.room.type = RoomType::None;
    Layer lead;
    lead.position = {0, 1.6f, 0};
    lead.motion.path = line({0, 1.6f, 0}, {4, 1.6f, 0});
    lead.motion.speed.keys = {{0, 1.0f, Easing::Linear}};
    Layer mid;
    mid.position = {0, 1.6f, -2};
    mid.link.to = 0;
    Layer tail;
    tail.position = {0, 1.6f, -4};
    tail.link.to = 1;
    s.layers = {lead, mid, tail};
    PoseEvaluator e(s, 20);
    CHECK(e.layerMoves(1));
    CHECK(e.layerMoves(2));
    CHECK(e.layerPlacement(1, 2.0).position.x == Approx(2).margin(1e-3));
    CHECK(e.layerPlacement(2, 2.0).position.x == Approx(2).margin(1e-3));
    CHECK(e.layerPlacement(2, 2.0).position.z == Approx(-4).margin(1e-3));
    // A layer that leads itself, or a leader that does not move, moves nothing.
    s.layers[2].link.to = 2;
    s.layers[1].link.to = 2;
    PoseEvaluator e2(s, 20);
    CHECK_FALSE(e2.layerMoves(2));
    CHECK_FALSE(e2.layerMoves(1));
    CHECK(e2.layerPlacement(2, 5.0).position.z == Approx(-4).margin(1e-3));
    // A cycle (hand-written JSON) ends at the depth limit instead of looping.
    s.layers[1].link.to = 2;
    s.layers[2].link.to = 1;
    PoseEvaluator e3(s, 20);
    CHECK_FALSE(e3.layerMoves(1));
    CHECK(e3.layerPlacement(1, 5.0).position.z == Approx(-2).margin(1e-3));
}

TEST_CASE("Spatialize, room send and links survive the scene file") {
    Scene s;
    Layer a;
    a.spatialize = false;
    a.roomSend = true;
    Layer b;
    b.link.to = kLinkListener;
    b.link.linkedAtStart = false;
    b.link.keys = {{2.5, true}, {4.0, false}};
    Layer c;
    c.link.to = 0;
    c.referenceOnly = true;
    s.layers = {a, b, c};
    const Scene r = sceneFromJson(sceneToJson(s));
    REQUIRE(r.layers.size() == 3);
    CHECK_FALSE(r.layers[0].spatialize);
    CHECK(r.layers[0].roomSend);
    CHECK(r.layers[1].spatialize);
    CHECK(r.layers[1].link.to == kLinkListener);
    CHECK_FALSE(r.layers[1].link.linkedAtStart);
    REQUIRE(r.layers[1].link.keys.size() == 2);
    CHECK(r.layers[1].link.keys[1].time == Approx(4.0));
    CHECK_FALSE(r.layers[1].link.keys[1].linked);
    CHECK(r.layers[1].link.linkedAt(3.0));
    CHECK_FALSE(r.layers[1].link.linkedAt(5.0));
    CHECK(r.layers[2].link.to == 0);
    CHECK(r.layers[2].referenceOnly);
    CHECK(layerInputs(r.layers[2]) == 0);
    CHECK(inputChannels(r) == 2);
    // The file says nothing about defaults.
    const std::string text = sceneToJson(Scene{});
    CHECK(text.find("spatialize") == std::string::npos);
}

TEST_CASE("Straight through: a mono layer reaches both ears at -3 dB with no delay, a stereo one its own side") {
    Scene s;
    s.room.type = RoomType::None;
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer mono;
    mono.position = {-3, 1.6f, -1};   // off to the left, 3 m away: would be quiet and late
    mono.spatialize = false;
    Layer st;
    st.position = {2, 1.6f, -2};
    st.channels = 2;
    st.spatialize = false;
    s.layers = {mono, st};
    RenderConfig cfg;
    cfg.mode = OutputMode::Binaural;
    cfg.hrtfPath = SP_TEST_HRTF;
    Renderer r(s, cfg, 2.0);
    REQUIRE(r.numInputs() == 3);
    const int n = 4096;
    std::vector<float> m(n, 0.0f), l(n, 0.0f), rr(n, 0.0f), outL(n, 0.0f), outR(n, 0.0f);
    m[1000] = 1.0f;
    l[2000] = 1.0f;
    rr[3000] = 1.0f;
    const float* in[3] = {m.data(), l.data(), rr.data()};
    float* out[2] = {outL.data(), outR.data()};
    r.process(in, out, n, 0.0);
    const int lat = r.latencySamples();
    CHECK(outL[1000 + lat] == Approx(0.7071f).margin(1e-3));
    CHECK(outR[1000 + lat] == Approx(0.7071f).margin(1e-3));
    CHECK(outL[2000 + lat] == Approx(1.0f).margin(1e-3));
    CHECK(std::fabs(outR[2000 + lat]) < 1e-4f);
    CHECK(outR[3000 + lat] == Approx(1.0f).margin(1e-3));
    CHECK(std::fabs(outL[3000 + lat]) < 1e-4f);
    // Nothing else: no HRTF tail, no propagation delay.
    double other = 0;
    for (int i = 0; i < n; ++i) {
        if (i == 1000 + lat || i == 2000 + lat || i == 3000 + lat) continue;
        other += std::fabs(outL[i]) + std::fabs(outR[i]);
    }
    CHECK(other < 1e-3);
}

TEST_CASE("Straight through in a room: the reverb is fed only with Room send on") {
    Scene s;
    s.room.type = RoomType::Box;
    s.room.size = {8, 3, 10};
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.position = {1, 1.6f, -2};
    l.spatialize = false;
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Binaural;
    cfg.hrtfPath = SP_TEST_HRTF;
    auto tail = [&](bool send) {
        s.layers[0].roomSend = send;
        Renderer r(s, cfg, 2.0);
        const int n = 48000;
        std::vector<float> in(n, 0.0f), outL(n, 0.0f), outR(n, 0.0f);
        for (int i = 0; i < 2400; ++i) in[i] = 0.5f;   // 50 ms burst
        const float* ins[1] = {in.data()};
        float* outs[2] = {outL.data(), outR.data()};
        r.process(ins, outs, n, 0.0);
        double e = 0;
        for (int i = 12000; i < n; ++i) e += static_cast<double>(outL[i]) * outL[i];   // after 250 ms: reverb only
        return e;
    };
    CHECK(tail(false) < 1e-6);
    CHECK(tail(true) > 1e-3);
}

TEST_CASE("The switch glides: no step in the output when a layer stops being spatialised") {
    Scene s;
    s.room.type = RoomType::None;
    s.environment.airAbsorption = false;
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.position = {0, 1.6f, -1};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    Renderer r(s, cfg, 2.0);
    const int n = 24000;
    std::vector<float> in(n, 0.3f), w(n, 0.0f);
    const float* ins[1] = {in.data()};
    float* outs[4] = {w.data(), nullptr, nullptr, nullptr};
    r.process(ins, outs, 4800, 0.0);
    LayerControls c;
    c.spatialize = false;
    r.setLayerControls(0, c);
    ins[0] = in.data() + 4800;
    outs[0] = w.data() + 4800;
    r.process(ins, outs, n - 4800, 0.1);
    float maxStep = 0;
    for (int i = 4801; i < n; ++i) maxStep = std::max(maxStep, std::fabs(w[i] - w[i - 1]));
    CHECK(maxStep < 0.02f);
    CHECK(w[n - 1] == Approx(0.3f).margin(0.01f));   // W of a plane wave: the signal as it is
}
