#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sp/Pose.h"

using namespace sp;
using Catch::Approx;

namespace {

Scene lineScene(float length, float speed) {
    Scene s;
    Path p;
    PathSegment seg;
    seg.type = SegmentType::Line;
    seg.points = {{0, 1.6f, 0}, {0, 1.6f, -length}};
    p.segments.push_back(seg);
    s.listener.paths.push_back(p);
    s.listener.speed.keys = {{0, speed, Easing::Linear}};
    return s;
}

}  // namespace

TEST_CASE("Line path arc length and position") {
    Path p;
    PathSegment seg;
    seg.type = SegmentType::Line;
    seg.points = {{0, 0, 0}, {3, 4, 0}};
    p.segments.push_back(seg);
    SampledPath sp(p);
    CHECK(sp.length() == Approx(5).margin(1e-4));
    const Vec3 mid = sp.positionAt(2.5f);
    CHECK(mid.x == Approx(1.5).margin(1e-4));
    CHECK(mid.y == Approx(2.0).margin(1e-4));
}

TEST_CASE("Full circle arc has circumference length") {
    Path p;
    PathSegment seg;
    seg.type = SegmentType::Arc;
    seg.points = {{0, 0, 0}, {2, 0, 0}, {2, 0, 0}};  // centre, start == end -> full circle
    p.segments.push_back(seg);
    SampledPath sp(p);
    CHECK(sp.length() == Approx(2 * kPi * 2).epsilon(1e-3));
    // Every point is 2 m from the centre.
    for (float s = 0; s < sp.length(); s += 0.5f) CHECK(sp.positionAt(s).length() == Approx(2).margin(1e-3));
}

TEST_CASE("Catmull-Rom passes through its control points") {
    Path p;
    PathSegment seg;
    seg.type = SegmentType::CatmullRom;
    seg.points = {{0, 0, 0}, {1, 0, -1}, {3, 0, -1}, {4, 0, 0}};
    p.segments.push_back(seg);
    const Vec3 a = SampledPath::evaluateSegment(seg, 0.0f);
    const Vec3 b = SampledPath::evaluateSegment(seg, 1.0f / 3.0f);
    const Vec3 c = SampledPath::evaluateSegment(seg, 1.0f);
    CHECK((a - seg.points[0]).length() < 1e-5f);
    CHECK((b - seg.points[1]).length() < 1e-5f);
    CHECK((c - seg.points[3]).length() < 1e-5f);
}

TEST_CASE("Constant speed covers distance = v * t") {
    Scene s = lineScene(100, 2.0f);
    PoseEvaluator ev(s, 60);
    const Pose p = ev.evaluate(10.0);
    CHECK(p.position.z == Approx(-20).margin(1e-2));
    CHECK(p.velocity.length() == Approx(2).margin(1e-4));
    // Looking along the path: forward is -Z.
    CHECK(p.orientation.forward().z == Approx(-1).margin(1e-4));
}

TEST_CASE("Speed ramp integrates correctly") {
    Scene s = lineScene(1000, 0);
    s.listener.speed.keys = {{0, 0, Easing::Linear}, {10, 10, Easing::Linear}};
    PoseEvaluator ev(s, 60);
    // Area under the ramp: 0.5 * 10 * 10 = 50 m at t = 10.
    CHECK(ev.distanceAlongPath(10.0) == Approx(50).margin(0.05));
    // Then constant 10 m/s.
    CHECK(ev.distanceAlongPath(15.0) == Approx(100).margin(0.1));
}

TEST_CASE("Pose is a pure function of time") {
    Scene s = lineScene(100, 1.4f);
    s.listener.head.mode = HeadMode::Keyframed;
    s.listener.head.keys = {{0, 0, 0, 0, Easing::SmoothStep}, {5, 90, 20, 0, Easing::SmoothStep}};
    PoseEvaluator ev(s, 60);
    const Pose a = ev.evaluate(3.3);
    ev.evaluate(50.0);
    ev.evaluate(0.1);
    const Pose b = ev.evaluate(3.3);
    CHECK((a.position - b.position).length() == 0.0f);
    CHECK(a.orientation.w == b.orientation.w);
}

TEST_CASE("Head offset yaw and look-at") {
    Scene s = lineScene(100, 1.0f);
    s.listener.head.yawOffsetDeg = 90;
    PoseEvaluator ev(s, 60);
    Pose p = ev.evaluate(1.0);
    CHECK(p.orientation.forward().x == Approx(-1).margin(1e-4));  // path forward -Z, turned left -> -X

    s.listener.head.yawOffsetDeg = 0;
    s.listener.head.mode = HeadMode::LookAt;
    s.listener.head.lookAtPoint = {10, 1.6f, -1};
    PoseEvaluator ev2(s, 60);
    p = ev2.evaluate(1.0);  // listener at z = -1
    CHECK(p.orientation.forward().x == Approx(1).margin(1e-4));

    ListenerControls c;
    c.yawOffsetDeg = -90;  // turn right from facing +X: now facing +Z
    p = ev2.evaluate(1.0, c);
    CHECK(p.orientation.forward().z == Approx(1).margin(1e-4));
}

TEST_CASE("Along-path position mode uses the fraction") {
    Scene s = lineScene(100, 1.0f);
    s.listener.positionMode = PositionMode::AlongPath;
    s.listener.pathFraction = 0.25f;
    PoseEvaluator ev(s, 60);
    CHECK(ev.evaluate(123.0).position.z == Approx(-25).margin(1e-3));
    ListenerControls c;
    c.pathFraction = 0.5f;
    CHECK(ev.evaluate(1.0, c).position.z == Approx(-50).margin(1e-3));
}
