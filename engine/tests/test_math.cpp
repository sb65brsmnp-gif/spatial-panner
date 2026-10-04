#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/Ambisonics.h"
#include "saf.h"
#include "sp/Math.h"

using namespace sp;
using Catch::Approx;

TEST_CASE("Quaternion yaw turns the forward vector left") {
    const Quat q = Quat::fromYawPitchRoll(degToRad(90), 0, 0);
    const Vec3 f = q.forward();
    CHECK(f.x == Approx(-1).margin(1e-5));  // left is -X
    CHECK(f.z == Approx(0).margin(1e-5));
}

TEST_CASE("Quaternion pitch looks up") {
    const Quat q = Quat::fromYawPitchRoll(0, degToRad(45), 0);
    const Vec3 f = q.forward();
    CHECK(f.y == Approx(std::sin(degToRad(45))).margin(1e-5));
    CHECK(f.z == Approx(-std::cos(degToRad(45))).margin(1e-5));
}

TEST_CASE("Yaw/pitch/roll round trip") {
    const Quat q = Quat::fromYawPitchRoll(degToRad(35), degToRad(-20), degToRad(10));
    float y, p, r;
    q.toYawPitchRoll(y, p, r);
    CHECK(radToDeg(y) == Approx(35).margin(1e-3));
    CHECK(radToDeg(p) == Approx(-20).margin(1e-3));
    CHECK(radToDeg(r) == Approx(10).margin(1e-3));
}

TEST_CASE("lookRotation points -Z along the target") {
    const Vec3 target{3, 1, -2};
    const Quat q = Quat::lookRotation(target);
    const Vec3 f = q.forward();
    const Vec3 t = target.normalized();
    CHECK(f.x == Approx(t.x).margin(1e-5));
    CHECK(f.y == Approx(t.y).margin(1e-5));
    CHECK(f.z == Approx(t.z).margin(1e-5));
    CHECK(q.up().y > 0.5f);
}

TEST_CASE("Spherical conversion matches SOFA conventions") {
    // Source to the left of the listener: azimuth +90.
    Spherical s = toSpherical({-1, 0, 0});
    CHECK(s.azimuthDeg == Approx(90).margin(1e-4));
    CHECK(s.elevationDeg == Approx(0).margin(1e-4));
    // Straight ahead: 0/0. Above: elevation 90.
    s = toSpherical({0, 0, -1});
    CHECK(s.azimuthDeg == Approx(0).margin(1e-4));
    s = toSpherical({0, 1, 0});
    CHECK(s.elevationDeg == Approx(90).margin(1e-4));
    // Round trip
    const Vec3 d{0.3f, -0.5f, -0.8f};
    const Vec3 back = fromSpherical(toSpherical(d));
    const Vec3 n = d.normalized();
    CHECK(back.x == Approx(n.x).margin(1e-5));
    CHECK(back.y == Approx(n.y).margin(1e-5));
    CHECK(back.z == Approx(n.z).margin(1e-5));
}

TEST_CASE("Closed-form spherical harmonics match SAF (ACN/N3D)") {
    const Vec3 dirs[] = {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {0.3f, 0.5f, -0.8f}, {0.7f, -0.2f, 0.4f}};
    for (const Vec3& d : dirs) {
        const Spherical s = toSpherical(d);
        float dirDeg[2] = {s.azimuthDeg, s.elevationDeg};
        float ref[16];
        getRSH(3, dirDeg, 1, ref);
        float mine[16];
        dsp::encodeDirection(d, 3, mine);
        for (int c = 0; c < 16; ++c) CHECK(mine[c] == Approx(ref[c]).margin(1e-4));
    }
}
