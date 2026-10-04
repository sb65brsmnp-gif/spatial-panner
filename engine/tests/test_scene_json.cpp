#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sp/SceneJson.h"

using namespace sp;
using Catch::Approx;

TEST_CASE("Scene JSON round trip") {
    Scene s;
    s.name = "test";
    Layer l;
    l.name = "voice";
    l.position = {1, 2, -3};
    l.levelDb = -6;
    l.directivity = 0.5f;
    s.layers.push_back(l);
    s.room.size = {5, 3, 7};
    s.room.materials[WallNegY] = materials::byName("carpet");
    Path p;
    PathSegment seg;
    seg.type = SegmentType::CatmullRom;
    seg.points = {{0, 1.6f, 0}, {1, 1.6f, -2}, {3, 1.6f, -3}};
    p.segments.push_back(seg);
    s.listener.paths.push_back(p);
    s.listener.speed.keys = {{0, 1.0f, Easing::Linear}, {4, 2.5f, Easing::SmoothStep}};
    s.listener.head.mode = HeadMode::Keyframed;
    s.listener.head.keys = {{0, 0, 0, 0, Easing::SmoothStep}, {3, 90, -10, 0, Easing::Linear}};

    const std::string text = sceneToJson(s);
    const Scene back = sceneFromJson(text);
    REQUIRE(back.layers.size() == 1);
    CHECK(back.layers[0].name == "voice");
    CHECK(back.layers[0].position.z == Approx(-3));
    CHECK(back.layers[0].levelDb == Approx(-6));
    CHECK(back.layers[0].directivity == Approx(0.5));
    CHECK(back.room.size.z == Approx(7));
    CHECK(back.room.materials[WallNegY].name == "carpet");
    CHECK(back.room.materials[WallNegY].absorption[3] == Approx(0.69f));
    REQUIRE(back.listener.paths.size() == 1);
    REQUIRE(back.listener.paths[0].segments.size() == 1);
    CHECK(back.listener.paths[0].segments[0].type == SegmentType::CatmullRom);
    CHECK(back.listener.paths[0].segments[0].points.size() == 3);
    REQUIRE(back.listener.speed.keys.size() == 2);
    CHECK(back.listener.speed.keys[1].easing == Easing::SmoothStep);
    CHECK(back.listener.head.mode == HeadMode::Keyframed);
    REQUIRE(back.listener.head.keys.size() == 2);
    CHECK(back.listener.head.keys[1].yawDeg == Approx(90));
}

TEST_CASE("Scene JSON accepts shorthand and reports bad enums") {
    const char* text = R"({
        "layers": [{"name": "a", "audio": "a.wav", "position": {"x": 1, "y": 1, "z": -1}}],
        "room": {"type": "outdoor", "materials": "grass"},
        "listener": {"path": {"points": [[0,1.6,0],[0,1.6,-10]]}, "speed": 3.0}
    })";
    const Scene s = sceneFromJson(text);
    CHECK(s.room.type == RoomType::Outdoor);
    CHECK(s.room.materials[WallNegY].name == "grass");
    REQUIRE(s.listener.paths.size() == 1);
    CHECK(s.listener.paths[0].segments[0].type == SegmentType::CatmullRom);
    REQUIRE(s.listener.speed.keys.size() == 1);
    CHECK(s.listener.speed.keys[0].speed == Approx(3));

    CHECK_THROWS(sceneFromJson(R"({"room": {"type": "cathedral"}})"));
    CHECK_THROWS(sceneFromJson("not json"));
}
