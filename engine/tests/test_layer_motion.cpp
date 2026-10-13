// Layers travelling along their own paths: timing, end rules, turning,
// level automation, the file format, and what the renderer makes of it.
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

// 10 m along -Z from the origin, at 2 m/s from t = 1 s.
LayerMotion straight(PathEnd end) {
    LayerMotion m;
    m.path = line({0, 0, 0}, {0, 0, -10});
    m.speed.keys = {{0, 2.0f, Easing::Linear}};
    m.startTime = 1.0;
    m.end = end;
    return m;
}

}  // namespace

TEST_CASE("Layer motion: speed timing waits, travels, and stops at the end") {
    LayerMotionEvaluator m(straight(PathEnd::Stop));
    REQUIRE(m.active());
    CHECK(m.evaluate(0.5).offset.z == Approx(0).margin(1e-4));
    CHECK(m.evaluate(3.0).offset.z == Approx(-4).margin(1e-3));
    CHECK(m.evaluate(20.0).offset.z == Approx(-10).margin(1e-3));
    CHECK(m.evaluate(20.0).gain == 1.0f);
}

TEST_CASE("Layer motion: loop jumps back and fades around the jump; a closed path does not") {
    LayerMotionEvaluator m(straight(PathEnd::Loop));
    CHECK(m.evaluate(1.0 + 7.0).offset.z == Approx(-4).margin(1e-3));   // 14 m travelled -> 4 m
    CHECK(m.evaluate(1.0 + 7.0).gain == Approx(1));
    CHECK(m.evaluate(1.0 + 5.0 - 0.001).gain < 0.5f);   // 2 mm before the end
    CHECK(m.evaluate(1.0 + 5.0 + 0.001).gain < 0.5f);   // just after the jump
    CHECK(m.evaluate(1.0 + 0.001).gain == Approx(1));   // the first start is not faded

    LayerMotion ring;
    ring.path = line({0, 0, 0}, {0, 0, -10});
    ring.path.closed = true;  // there and back: 20 m round
    ring.speed.keys = {{0, 2.0f, Easing::Linear}};
    ring.end = PathEnd::Loop;
    LayerMotionEvaluator r(ring);
    CHECK(r.length() == Approx(20).margin(1e-3));
    CHECK(r.evaluate(10.0 - 0.001).gain == Approx(1));
}

TEST_CASE("Layer motion: back and forth turns round at the ends") {
    LayerMotion mm = straight(PathEnd::PingPong);
    mm.turnAlongPath = true;
    LayerMotionEvaluator m(mm);
    const MotionState out = m.evaluate(1.0 + 3.0);   // 6 m out
    CHECK(out.offset.z == Approx(-6).margin(1e-3));
    CHECK(out.forward);
    CHECK(out.yawDeg == Approx(0).margin(0.1));
    const MotionState back = m.evaluate(1.0 + 7.0);  // 14 m: 6 m back from the far end
    CHECK(back.offset.z == Approx(-6).margin(1e-3));
    CHECK_FALSE(back.forward);
    CHECK(std::fabs(back.yawDeg) == Approx(180).margin(0.1));
    CHECK(m.evaluate(1.0 + 11.0).offset.z == Approx(-2).margin(1e-3));  // 22 m: out again
}

TEST_CASE("Layer motion: turning along a corner follows the direction of travel") {
    LayerMotion mm;
    Path p = line({0, 0, 0}, {0, 0, -5});
    PathSegment s2;
    s2.type = SegmentType::Line;
    s2.points = {{0, 0, -5}, {5, 0, -5}};   // turn right (towards +X)
    p.segments.push_back(s2);
    mm.path = p;
    mm.speed.keys = {{0, 1.0f, Easing::Linear}};
    mm.turnAlongPath = true;
    LayerMotionEvaluator m(mm);
    CHECK(m.evaluate(2.0).yawDeg == Approx(0).margin(0.5));
    CHECK(m.evaluate(8.0).yawDeg == Approx(-90).margin(0.5));   // right = negative yaw
    mm.turnAlongPath = false;
    CHECK(LayerMotionEvaluator(mm).evaluate(8.0).yawDeg == 0.0f);
}

TEST_CASE("Layer motion: keys put the layer where it should be when") {
    LayerMotion mm;
    mm.path = line({0, 0, 0}, {0, 0, -10});
    mm.timing = LayerTiming::Keys;
    mm.keys = {{2.0, 0.0f, Easing::Linear}, {4.0, 1.0f, Easing::Linear}, {6.0, 0.5f, Easing::Linear}};
    LayerMotionEvaluator m(mm);
    CHECK(m.evaluate(0.0).offset.z == Approx(0).margin(1e-3));
    CHECK(m.evaluate(3.0).offset.z == Approx(-5).margin(1e-3));
    CHECK(m.evaluate(4.0).offset.z == Approx(-10).margin(1e-3));
    CHECK(m.evaluate(5.0).offset.z == Approx(-7.5).margin(1e-3));
    CHECK_FALSE(m.evaluate(5.0).forward);
    CHECK_FALSE(m.evaluate(9.0).forward);    // waiting after a move back
    CHECK(m.evaluate(9.0).offset.z == Approx(-5).margin(1e-3));

    mm.end = PathEnd::Loop;   // period 4 s
    LayerMotionEvaluator l(mm);
    CHECK(l.evaluate(7.0).offset.z == Approx(-5).margin(1e-3));   // = t 3
    mm.end = PathEnd::PingPong;
    LayerMotionEvaluator pp(mm);
    CHECK(pp.evaluate(7.0).offset.z == Approx(-7.5).margin(1e-3));  // = t 5, played backwards
    CHECK(pp.evaluate(7.0).forward);                                 // the move back, reversed
}

TEST_CASE("Layer motion: position timing and the plugin's overrides") {
    LayerMotion mm = straight(PathEnd::Stop);
    mm.timing = LayerTiming::Position;
    mm.fraction = 0.25f;
    LayerMotionEvaluator m(mm);
    CHECK(m.evaluate(100).offset.z == Approx(-2.5).margin(1e-3));
    MotionOverride o;
    o.fraction = 0.75f;
    CHECK(m.evaluate(100, o).offset.z == Approx(-7.5).margin(1e-3));

    LayerMotionEvaluator s(straight(PathEnd::Loop));
    MotionOverride d;
    d.distance = 13.0;
    d.speed = 2.0f;
    CHECK(s.evaluate(0, d).offset.z == Approx(-3).margin(1e-3));
}

TEST_CASE("Level automation: keys in dB, fades to silence in gain") {
    std::vector<LevelKey> k = {{0, kSilentDb, Easing::Linear}, {2, 0, Easing::Linear}, {4, -6, Easing::Linear}};
    CHECK(levelKeysGain(k, 0) == 0.0f);
    CHECK(levelKeysGain(k, 1) == Approx(0.5).margin(1e-4));
    CHECK(levelKeysDb(k, 3) == Approx(-3).margin(1e-4));
    CHECK(levelKeysDb(k, 10) == Approx(-6).margin(1e-4));
    CHECK(levelKeysGain({}, 5) == 1.0f);
}

TEST_CASE("Layer motion and level keys survive the scene file; plain layers write neither") {
    Scene s;
    Layer a;
    a.name = "moving";
    a.motion = straight(PathEnd::PingPong);
    a.motion.turnAlongPath = true;
    a.motion.keys = {{1, 0.2f, Easing::EaseIn}};
    a.levelKeys = {{0, kSilentDb, Easing::Linear}, {1.5, -3, Easing::SmoothStep}};
    s.layers.push_back(a);
    Layer b;
    b.name = "still";
    s.layers.push_back(b);
    const std::string text = sceneToJson(s);
    CHECK(text.find("\"motion\"") != std::string::npos);
    const Scene r = sceneFromJson(text);
    const Layer& ra = r.layers[0];
    REQUIRE(ra.motion.hasPath());
    CHECK(ra.motion.end == PathEnd::PingPong);
    CHECK(ra.motion.turnAlongPath);
    CHECK(ra.motion.startTime == Approx(1.0));
    CHECK(ra.motion.speed.keys.size() == 1);
    CHECK(ra.motion.keys.size() == 1);
    CHECK(ra.motion.keys[0].easing == Easing::EaseIn);
    CHECK(ra.levelKeys.size() == 2);
    CHECK(ra.levelKeys[0].levelDb == kSilentDb);
    CHECK_FALSE(r.layers[1].motion.hasPath());
    CHECK(sceneToJson(r) == text);
}

TEST_CASE("Moving layer: Doppler of a source approaching a still listener") {
    Scene s;
    s.room.type = RoomType::None;
    s.environment.airAbsorption = false;
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.position = {0, 1.6f, -200};
    l.motion.path = line({0, 1.6f, -200}, {0, 1.6f, -20});
    l.motion.speed.keys = {{0, 30.0f, Easing::Linear}};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 48000 * 2;
    std::vector<float> tone(n);
    for (int i = 0; i < n; ++i) tone[i] = std::sin(2 * kPi * 1000 * i / 48000.0f);
    Renderer r(s, cfg, 3.0);
    std::vector<float> w(n);
    for (int pos = 0; pos < n; pos += 256) {
        const float* in[1] = {tone.data() + pos};
        float* out[4] = {w.data() + pos, nullptr, nullptr, nullptr};
        r.process(in, out, std::min(256, n - pos), pos / 48000.0);
    }
    int c = 0;
    for (int i = 48000; i < n; ++i)
        if ((w[i - 1] < 0) != (w[i] < 0)) ++c;
    // A moving source, f c / (c - v): 1095.8 Hz. The listener moving at the
    // same speed towards a still source hears f (c + v) / c = 1087.5 Hz.
    CHECK(c / 2.0 == Approx(1000.0 * 343.0 / (343.0 - 30.0)).epsilon(0.002));
}

TEST_CASE("Moving layer: a fade-in from silence, and the analysis tracks the layer") {
    Scene s;
    s.room.type = RoomType::None;
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.position = {2, 1.6f, -2};
    l.motion.path = line({2, 1.6f, -2}, {-2, 1.6f, -2});
    l.motion.speed.keys = {{0, 1.0f, Easing::Linear}};
    l.levelKeys = {{0, kSilentDb, Easing::Linear}, {0.5, 0, Easing::Linear}};
    s.layers.push_back(l);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    Renderer r(s, cfg, 2.0);
    std::vector<float> one(4800, 0.5f), w(4800);
    const float* in[1] = {one.data()};
    float* out[4] = {w.data(), nullptr, nullptr, nullptr};
    r.process(in, out, 4800, 0.0);
    CHECK(std::fabs(w[200]) < 1e-3f);   // still silent near t = 0 (the first sub-block is latency)

    const SceneAnalysis a = analyzeScene(s, 4.0);
    REQUIRE(a.layers.size() == 1);
    CHECK(a.layers[0].length == Approx(4).margin(1e-3));
    const auto& last = a.layers[0].samples.back();
    CHECK(last.position.x == Approx(-2).margin(1e-3));
    CHECK(a.layers[0].samples[static_cast<size_t>(std::lround(2.0 / a.layers[0].dt))].position.x == Approx(0).margin(1e-2));
}

TEST_CASE("Head looking at a moving layer follows it") {
    Scene s;
    Layer l;
    l.position = {0, 1.6f, -5};
    l.motion.path = line({0, 1.6f, -5}, {5, 1.6f, -5});
    l.motion.speed.keys = {{0, 1.0f, Easing::Linear}};
    s.layers.push_back(l);
    s.listener.staticPosition = {0, 1.6f, 0};
    s.listener.head.mode = HeadMode::LookAt;
    s.listener.head.lookAtLayer = 0;
    PoseEvaluator e(s, 10);
    float yaw, pitch, roll;
    e.evaluate(5.0).orientation.toYawPitchRoll(yaw, pitch, roll);
    CHECK(radToDeg(yaw) == Approx(-45).margin(0.5));   // looking front-right
}
