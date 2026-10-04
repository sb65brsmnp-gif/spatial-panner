// Steam Audio back-end: geometry, occlusion and the A/B against the built-in
// image-source model. Compiled only when the engine has the back-end.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "sp/Renderer.h"
#include "sp/SceneJson.h"

using namespace sp;
using Catch::Approx;

namespace {

std::vector<std::vector<float>> render(const Scene& scene, RenderConfig cfg, const std::vector<std::vector<float>>& inputs,
                                       int frames, int hostBlock = 480) {
    Renderer r(scene, cfg, frames / cfg.sampleRate + 1);
    std::vector<std::vector<float>> out(r.numOutputs(), std::vector<float>(frames + r.latencySamples(), 0.0f));
    std::vector<const float*> in(inputs.size());
    std::vector<float*> op(r.numOutputs());
    std::vector<float> zeros(hostBlock, 0.0f);
    int pos = 0;
    const int total = frames + r.latencySamples();
    while (pos < total) {
        const int n = std::min(hostBlock, total - pos);
        for (size_t i = 0; i < inputs.size(); ++i) in[i] = pos < frames ? inputs[i].data() + pos : zeros.data();
        for (int c = 0; c < r.numOutputs(); ++c) op[c] = out[c].data() + pos;
        r.process(in.data(), op.data(), n, pos / cfg.sampleRate);
        pos += n;
    }
    for (auto& ch : out) ch.erase(ch.begin(), ch.begin() + r.latencySamples());
    return out;
}

double energy(const std::vector<float>& v, size_t from = 0, size_t to = 0) {
    if (to == 0) to = v.size();
    double e = 0;
    for (size_t i = from; i < to && i < v.size(); ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
}

std::vector<float> noiseBurst(int n, int burst, unsigned seed = 7) {
    std::vector<float> v(n, 0.0f);
    unsigned x = seed;
    for (int i = 0; i < burst; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * 0.5f;
    }
    return v;
}

Scene boxScene() {
    Scene s;
    s.room.type = RoomType::Box;
    s.room.size = {10, 3.2f, 12};
    s.room.materials = {materials::byName("plaster"), materials::byName("plaster"), materials::byName("wood_floor"),
                        materials::byName("acoustic_tile"), materials::byName("plaster"), materials::byName("curtain")};
    s.environment.airAbsorption = false;
    s.listener.staticPosition = {0, 1.6f, 2};
    Layer l;
    l.name = "src";
    l.position = {-2.5f, 1.5f, -3};
    s.layers.push_back(l);
    return s;
}

RenderConfig ambiConfig() {
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    cfg.steam.rays = 4096;
    cfg.steam.threads = 2;
    cfg.steam.updateInterval = 10;  // one simulation per render
    return cfg;
}

}  // namespace

TEST_CASE("Steam Audio is available in this build") {
    REQUIRE(Renderer::steamAudioAvailable());
}

TEST_CASE("Steam Audio: box room reflections carry energy comparable to the image-source model") {
    Scene s = boxScene();
    const int fs = 48000, frames = fs;
    const auto in = std::vector<std::vector<float>>{noiseBurst(frames, 2400)};

    RenderConfig a = ambiConfig();
    a.reflections = ReflectionsBackend::Builtin;
    RenderConfig b = ambiConfig();
    b.reflections = ReflectionsBackend::SteamAudio;
    const auto outA = render(s, a, in, frames);
    const auto outB = render(s, b, in, frames);

    // Direct arrival: 6.1 m -> 854 samples. Everything after the burst has
    // passed (2400 + 900 samples) is reflections and reverb only.
    const size_t tailFrom = 3400;
    const double tailA = energy(outA[0], tailFrom), tailB = energy(outB[0], tailFrom);
    const double dirA = energy(outA[0], 0, tailFrom), dirB = energy(outB[0], 0, tailFrom);
    INFO("tail energy builtin " << 10 * std::log10(tailA) << " dB, steam " << 10 * std::log10(tailB) << " dB");
    INFO("early energy builtin " << 10 * std::log10(dirA) << " dB, steam " << 10 * std::log10(dirB) << " dB");
    REQUIRE(tailA > 0);
    REQUIRE(tailB > 0);
    // Different models of the same room: within 6 dB of each other, both for
    // the early part (direct + first reflections) and the tail.
    REQUIRE(std::fabs(10 * std::log10(tailA / tailB)) < 6.0);
    REQUIRE(std::fabs(10 * std::log10(dirA / dirB)) < 6.0);
    // And both decay: the last 200 ms hold less than the 200 ms after the burst.
    REQUIRE(energy(outB[0], frames - 9600) < energy(outB[0], tailFrom, tailFrom + 9600));
}

TEST_CASE("Steam Audio: a wall between the layer and the listener lowers the direct sound") {
    Scene s = boxScene();
    s.room.reflectionsEnabled = false;
    s.room.reverbEnabled = false;
    s.layers[0].position = {0, 1.6f, -4};
    s.listener.staticPosition = {0, 1.6f, 2};
    const int fs = 48000, frames = fs / 2;
    const auto in = std::vector<std::vector<float>>{noiseBurst(frames, frames)};

    RenderConfig cfg = ambiConfig();
    cfg.reflections = ReflectionsBackend::SteamAudio;
    const auto open = render(s, cfg, in, frames);

    SceneObject wall;
    wall.name = "wall";
    wall.minCorner = {-3, 0, -1.2f};
    wall.maxCorner = {3, 3.2f, -0.8f};
    wall.material = materials::byName("brick");
    s.room.objects.push_back(wall);
    const auto blocked = render(s, cfg, in, frames);

    const double eOpen = energy(open[0], 4800), eBlocked = energy(blocked[0], 4800);
    INFO("open " << 10 * std::log10(eOpen) << " dB, blocked " << 10 * std::log10(eBlocked) << " dB");
    REQUIRE(eBlocked < eOpen * 0.25);  // at least 6 dB down
    REQUIRE(eBlocked > 0);             // transmission through brick is not silence
}

TEST_CASE("Steam Audio: Auto picks the ray tracer for objects and meshes, Builtin ignores them") {
    Scene s = boxScene();
    SceneObject o;
    s.room.objects.push_back(o);
    RenderConfig cfg = ambiConfig();
    {
        Renderer r(s, cfg, 1);
        REQUIRE(r.stats().backend == ReflectionsBackend::SteamAudio);
        REQUIRE(r.stats().numTriangles == 24);  // 12 for the room, 12 for the box
        REQUIRE(r.stats().irSeconds > 0.3f);
    }
    cfg.reflections = ReflectionsBackend::Builtin;
    {
        Renderer r(s, cfg, 1);
        REQUIRE(r.stats().backend == ReflectionsBackend::Builtin);
        REQUIRE_FALSE(r.stats().note.empty());
    }
    Scene plain = boxScene();
    cfg.reflections = ReflectionsBackend::Auto;
    {
        Renderer r(plain, cfg, 1);
        REQUIRE(r.stats().backend == ReflectionsBackend::Builtin);
    }
}

TEST_CASE("Steam Audio: a mesh room from OBJ renders and decays") {
    const std::string path = (std::filesystem::temp_directory_path() / "sp_test_room.obj").string();
    {
        std::ofstream f(path);
        f << "# box 6 x 3 x 8\n";
        const float x = 3, y = 3, z = 4;
        f << "v " << -x << " 0 " << -z << "\nv " << x << " 0 " << -z << "\nv " << x << " 0 " << z << "\nv " << -x << " 0 " << z << "\n";
        f << "v " << -x << " " << y << " " << -z << "\nv " << x << " " << y << " " << -z << "\nv " << x << " " << y << " " << z << "\nv " << -x << " " << y << " " << z << "\n";
        f << "usemtl wood_floor\nf 1 2 3 4\nusemtl acoustic_tile\nf 8 7 6 5\n";
        f << "usemtl plaster\nf 1 5 6 2\nf 2 6 7 3\nf 3 7 8 4\nf 4 8 5 1\n";
    }
    const std::string json = R"({"layers": [{"name": "s", "position": [1.5, 1.5, -2]}],
        "room": {"type": "mesh", "mesh": {"file": ")" + path + R"("}},
        "listener": {"static_position": [0, 1.6, 1]}, "environment": {"air_absorption": false}})";
    Scene s = sceneFromJson(json, std::filesystem::temp_directory_path().string());
    REQUIRE(s.room.type == RoomType::Mesh);
    REQUIRE(s.room.mesh.triangles.size() == 12);
    REQUIRE(s.room.mesh.materials.size() >= 3);
    REQUIRE(s.room.size.x == Approx(6));

    RenderConfig cfg = ambiConfig();
    const int fs = 48000, frames = fs;
    const auto out = render(s, cfg, {noiseBurst(frames, 2400)}, frames);
    REQUIRE(energy(out[0], 3400) > 0);
    REQUIRE(energy(out[0], frames - 9600) < energy(out[0], 3400, 13000));
    std::remove(path.c_str());

    // Round trip keeps the file reference and the objects.
    s.room.objects.push_back(SceneObject{});
    s.room.meshFile.clear();  // serialise the geometry inline, the file is gone
    const Scene again = sceneFromJson(sceneToJson(s));
    REQUIRE(again.room.type == RoomType::Mesh);
    REQUIRE(again.room.objects.size() == 1);
}
