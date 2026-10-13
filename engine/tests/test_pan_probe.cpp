#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "sp/PanProbe.h"
#include "sp/SceneJson.h"

using namespace sp;
using Catch::Approx;

TEST_CASE("The pan probe reports VBAP, distance and alignment per speaker") {
    PanProbeRequest req;
    req.layout = SpeakerLayout::preset("5.1");
    // 1 m straight ahead: all on C; 2 m: half the gain.
    req.points = {{0, 0, -1}, {0, 0, -2}, {-std::sin(30 * 3.14159265f / 180) * 1, 0, -std::cos(30 * 3.14159265f / 180) * 1}};
    auto g = panProbe(req);
    REQUIRE(g.size() == 3);
    REQUIRE(g[0].size() == 6);
    CHECK(g[0][2] == Approx(1.0f).margin(1e-3f));
    CHECK(g[0][0] == Approx(0.0f).margin(1e-3f));
    CHECK(g[1][2] == Approx(0.5f).margin(1e-3f));
    CHECK(g[2][0] == Approx(1.0f).margin(1e-2f));   // on L
    CHECK(g[0][3] == 0.0f);                          // LFE never
    SECTION("distances and trims align the feeds") {
        req.layout.speakers[2].distance = 1;   // C nearer than the others
        for (auto& spk : req.layout.speakers) if (spk.distance == 0) spk.distance = 2;
        req.layout.speakers[2].trimDb = -6;
        auto a = panProbe(req);
        CHECK(a[0][2] == Approx(0.5f * dbToGain(-6)).margin(1e-3f));
    }
    SECTION("a pin at the point replaces the panner, a pin out of reach does nothing") {
        Pin pin;
        pin.position = {0, 0, -1};
        pin.radius = 0.5f;
        pin.gainsDb = {-120, 0, -120, -120, -120, -120};
        req.pins = {pin};
        auto a = panProbe(req);
        CHECK(a[0][1] == Approx(1.0f).margin(1e-3f));
        CHECK(a[0][2] == Approx(0.0f).margin(1e-3f));
        CHECK(a[1][2] == Approx(0.5f).margin(1e-3f));
    }
}

TEST_CASE("Pins round-trip through the scene JSON") {
    Scene s;
    Pin p;
    p.name = "stage left";
    p.position = {-3, 1.6f, -2};
    p.radius = 2;
    p.gainsDb = {0, -6, -120};
    s.pins.push_back(p);
    const Scene back = sceneFromJson(sceneToJson(s, 0));
    REQUIRE(back.pins.size() == 1);
    CHECK(back.pins[0].name == "stage left");
    CHECK(back.pins[0].position.x == Approx(-3));
    CHECK(back.pins[0].radius == Approx(2));
    REQUIRE(back.pins[0].gainsDb.size() == 3);
    CHECK(back.pins[0].gainsDb[1] == Approx(-6));
    CHECK(sceneFromJson("{}").pins.empty());
}
