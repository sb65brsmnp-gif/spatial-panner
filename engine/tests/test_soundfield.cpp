// Ambisonic layers: the sound-field matrix (rotation, walking inside and
// outside the sphere) and the renderer's handling of a recording.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <vector>

#include "dsp/Ambisonics.h"
#include "dsp/SoundField.h"
#include "saf.h"
#include "sp/Renderer.h"
#include "sp/SceneJson.h"

using namespace sp;
using namespace sp::dsp;
using Catch::Approx;

namespace {

// Energy-weighted mean direction of an order-1 field (the velocity vector),
// in engine axes: the direction the field "comes from".
Vec3 fieldDirection(const float* sh) {
    // ACN 1 = Y (left), 2 = Z (up), 3 = X (front), N3D.
    const float left = sh[1], up = sh[2], fwd = sh[3];
    return Vec3{-left, up, -fwd}.normalized();
}

void applyMatrix(const std::vector<float>& m, int nOut, int nIn, const float* in, float* out) {
    for (int k = 0; k < nOut; ++k) {
        float s = 0;
        for (int c = 0; c < nIn; ++c) s += m[static_cast<size_t>(k) * nIn + c] * in[c];
        out[k] = s;
    }
}

std::vector<float> noise(int n, unsigned seed = 7) {
    std::vector<float> v(n);
    unsigned x = seed;
    for (int i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = (static_cast<float>(x >> 8) / 8388608.0f - 1.0f) * 0.5f;
    }
    return v;
}

// Noise with little energy near Nyquist, so fractional delays pass it at
// (almost) unit gain.
std::vector<float> smoothNoise(int n, unsigned seed = 7) {
    std::vector<float> v = noise(n, seed);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = n - 1; i >= 3; --i) v[static_cast<size_t>(i)] = 0.25f * (v[static_cast<size_t>(i)] + v[static_cast<size_t>(i) - 1] + v[static_cast<size_t>(i) - 2] + v[static_cast<size_t>(i) - 3]);
    return v;
}

// W of the warped field for an order-N plane wave from the front, with the
// listener `z` metres along the front axis (positive = towards the sound),
// integrated directly over the sphere (axisymmetric, so a 1-D integral):
// W = sum_n (2n + 1) / 2 * integral_-1^1 g(x) P_n(x) dx, g = a / |a d - o|.
double expectedFrontW(int order, float a, float z) {
    const int steps = 200000;
    double sum = 0;
    for (int i = 0; i < steps; ++i) {
        const double x = -1.0 + (i + 0.5) * 2.0 / steps;
        const double r = std::sqrt(a * a + z * z - 2.0 * a * z * x);
        const double g = a / std::max(r, 0.25);
        double p0 = 1, p1 = x, k = 1;   // Legendre recurrence
        for (int n = 1; n <= order; ++n) {
            k += (2 * n + 1) * p1;
            const double p2 = ((2 * n + 1) * x * p1 - n * p0) / (n + 1);
            p0 = p1;
            p1 = p2;
        }
        sum += g * k;
    }
    return sum / steps;
}

double energy(const std::vector<float>& v, size_t from = 0) {
    double e = 0;
    for (size_t i = from; i < v.size(); ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
}

double dot(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0) {
    double e = 0;
    for (size_t i = from; i < a.size() && i < b.size(); ++i) e += static_cast<double>(a[i]) * b[i];
    return e;
}

struct Rendered { std::vector<std::vector<float>> out; };

Rendered render(const Scene& scene, RenderConfig cfg, const std::vector<std::vector<float>>& inputs, int frames) {
    Renderer r(scene, cfg, frames / cfg.sampleRate + 1);
    REQUIRE(r.numInputs() == static_cast<int>(inputs.size()));
    Rendered res;
    res.out.assign(static_cast<size_t>(r.numOutputs()), std::vector<float>(static_cast<size_t>(frames + r.latencySamples()), 0.0f));
    std::vector<const float*> in(inputs.size());
    std::vector<float*> out(static_cast<size_t>(r.numOutputs()));
    std::vector<float> zeros(512, 0.0f);
    const int total = frames + r.latencySamples();
    for (int pos = 0; pos < total;) {
        const int n = std::min(512, total - pos);
        for (size_t i = 0; i < inputs.size(); ++i) in[i] = pos < frames ? inputs[i].data() + pos : zeros.data();
        for (size_t c = 0; c < out.size(); ++c) out[c] = res.out[c].data() + pos;
        r.process(in.data(), out.data(), n, pos / cfg.sampleRate);
        pos += n;
    }
    for (auto& ch : res.out) ch.erase(ch.begin(), ch.begin() + r.latencySamples());
    return res;
}

Scene fieldScene(int channels = 4) {
    Scene s;
    s.room.type = RoomType::None;
    s.environment.airAbsorption = false;
    s.listener.staticPosition = {0, 1.6f, 0};
    Layer l;
    l.channels = channels;
    l.position = {0, 1.6f, 0};
    l.ambisonic.radius = 3;
    s.layers.push_back(l);
    return s;
}

// A first-order ambiX (ACN/SN3D) recording of one plane wave from `dir`
// (engine axes) carrying `signal`.
std::vector<std::vector<float>> planeWaveAmbix(const Vec3& dir, const std::vector<float>& signal) {
    float sh[4];
    encodeDirection(dir, 1, sh);
    std::vector<std::vector<float>> ch(4, std::vector<float>(signal.size()));
    for (int c = 0; c < 4; ++c) {
        const float g = sh[c] * n3dToSn3d(c);
        for (size_t i = 0; i < signal.size(); ++i) ch[static_cast<size_t>(c)][i] = signal[i] * g;
    }
    return ch;
}

}  // namespace

TEST_CASE("Sound field: at the centre without rotation the matrix is the identity") {
    SoundFieldTransform t;
    t.init(3, 3);
    std::vector<float> m(16 * 16);
    t.compute({0, 0, 0}, 3.0f, Quat::identity(), 1.0f, 0.25f, m.data());
    for (int k = 0; k < 16; ++k)
        for (int c = 0; c < 16; ++c) CHECK(m[static_cast<size_t>(k) * 16 + c] == Approx(k == c ? 1.0f : 0.0f).margin(1e-3));
    // A first-order recording fills only the first-order channels.
    t.init(1, 3);
    m.assign(16 * 4, 0.0f);
    t.compute({0, 0, 0}, 3.0f, Quat::identity(), 1.0f, 0.25f, m.data());
    for (int k = 0; k < 16; ++k)
        for (int c = 0; c < 4; ++c) CHECK(m[static_cast<size_t>(k) * 4 + c] == Approx(k == c ? 1.0f : 0.0f).margin(1e-3));
}

TEST_CASE("Sound field: at the centre a rotation matches SAF's SH rotation and the rotated direction") {
    SoundFieldTransform t;
    t.init(3, 3);
    const Quat q = Quat::fromYawPitchRoll(degToRad(35), degToRad(-20), degToRad(10));
    std::vector<float> m(16 * 16);
    t.compute({0, 0, 0}, 2.0f, q, 1.0f, 0.25f, m.data());

    // A plane wave from d must come out as a plane wave from q(d).
    const Vec3 d = Vec3{0.3f, 0.4f, -0.8f}.normalized();
    float in[16], out[16], ref[16];
    encodeDirection(d, 3, in);
    applyMatrix(m, 16, 16, in, out);
    encodeDirection(q.rotate(d), 3, ref);
    for (int k = 0; k < 16; ++k) CHECK(out[k] == Approx(ref[k]).margin(2e-3));

    // And against SAF's rotation matrix, built from the same rotation in
    // Ambisonics axes (x forward, y left, z up).
    const Vec3 axes[3] = {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}};
    float R[3][3];
    for (int k = 0; k < 3; ++k) {
        const Vec3 r = q.rotate(axes[k]);
        R[0][k] = -r.z;
        R[1][k] = -r.x;
        R[2][k] = r.y;
    }
    std::vector<float> rot(16 * 16);
    getSHrotMtxReal(R, rot.data(), 3);
    for (int k = 0; k < 256; ++k) CHECK(m[static_cast<size_t>(k)] == Approx(rot[static_cast<size_t>(k)]).margin(2e-3));
}

TEST_CASE("Sound field: walking towards a sound makes it louder and wider, away quieter; outside, the field narrows to the centre") {
    SoundFieldTransform t;
    t.init(1, 3);
    const float a = 3.0f;
    const Vec3 front{0, 0, -1};   // the recorded sound is in front of the mic
    float in[4];
    encodeDirection(front, 1, in);
    std::vector<float> m(16 * 4);
    float out[16];

    // A first-order recording spreads one sound over a wide lobe, so its W
    // follows the gain averaged over that lobe (the direct integral), not the
    // 3 / 1 of a point. The nearer, the louder; the farther, the quieter.
    t.compute({0, 0, -2.0f}, a, Quat::identity(), 1.0f, 0.25f, m.data());   // 2 m towards it: 1 m away
    applyMatrix(m, 16, 4, in, out);
    const float wNear = out[0];
    CHECK(wNear == Approx(expectedFrontW(1, a, 2.0f)).margin(0.02));
    CHECK(wNear > 1.5f);
    CHECK(fieldDirection(out).dot(front) > 0.99f);

    t.compute({0, 0, 2.0f}, a, Quat::identity(), 1.0f, 0.25f, m.data());    // 2 m away from it: 5 m
    applyMatrix(m, 16, 4, in, out);
    CHECK(out[0] == Approx(expectedFrontW(1, a, -2.0f)).margin(0.02));
    CHECK(out[0] < 0.75f);
    CHECK(fieldDirection(out).dot(front) > 0.99f);

    // A third-order recording is sharper and gets closer to the 3 / 1 of a point.
    t.init(3, 3);
    m.assign(16 * 16, 0.0f);
    float in3[16], out3[16];
    encodeDirection(front, 3, in3);
    t.compute({0, 0, -2.0f}, a, Quat::identity(), 1.0f, 0.25f, m.data());
    applyMatrix(m, 16, 16, in3, out3);
    CHECK(out3[0] == Approx(expectedFrontW(3, a, 2.0f)).margin(0.03));
    CHECK(out3[0] > wNear);

    // A diffuse field (energy in all directions): the energy of the warped
    // field's W follows the mean of (a / r)^2 over the sphere. At 0.9 a from
    // the centre that is (a / 2 rho) ln((a + rho) / (a - rho)) = 1.64 for a
    // point-sharp field; a third-order field smooths the gain over its lobes
    // and lands a little under that, and well above the 1.0 of the centre.
    m.assign(16 * 16, 0.0f);
    t.compute({0, 0, -0.9f * a}, a, Quat::identity(), 1.0f, 0.25f, m.data());
    double e = 0;
    for (int c = 0; c < 16; ++c) e += static_cast<double>(m[static_cast<size_t>(c)]) * m[static_cast<size_t>(c)];   // W row: sum over an uncorrelated unit field
    CHECK(e > 1.3);
    CHECK(e < 1.64);

    // Outside: everything arrives from the centre's direction, at a / rho.
    t.init(1, 3);
    m.assign(16 * 4, 0.0f);
    t.compute({0, 0, 30.0f}, a, Quat::identity(), 1.0f, 0.25f, m.data());   // 30 m behind the sphere
    encodeDirection({1, 0, 0}, 1, in);   // the recorded sound was on the right
    applyMatrix(m, 16, 4, in, out);
    CHECK(out[0] == Approx(0.1f).margin(0.01f));
    CHECK(fieldDirection(out).dot({0, 0, -1}) > 0.99f);   // but now it is ahead, where the sphere is
}

TEST_CASE("Sound field: cost of the matrix") {
    SoundFieldTransform t;
    t.init(3, 3);
    std::vector<float> m(16 * 16);
    const auto t0 = std::chrono::steady_clock::now();
    const int reps = 200;
    for (int i = 0; i < reps; ++i) t.compute({0.5f, 0.1f * i, -1.0f}, 3.0f, Quat::fromYawPitchRoll(0.01f * i, 0, 0), 1.0f, 0.25f, m.data());
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
    // Re-derived at most every 4 sub-blocks (2.7 ms): must be a small share of that.
    CHECK(us < 1000);
}

TEST_CASE("Renderer: an ambiX layer is heard in full at the centre and turns with the head") {
    Scene s = fieldScene(4);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 24000;
    const auto sig = smoothNoise(n);
    const Vec3 left{-1, 0, 0};
    const auto in = planeWaveAmbix(left, sig);

    Renderer r(s, cfg, 1);
    CHECK(r.numInputs() == 4);
    CHECK(r.inputIndex(0) == 0);

    // Head straight ahead: the sound stays on the left (Y positive, in phase with W).
    auto out = render(s, cfg, in, n);
    const size_t skip = 2400;
    CHECK(dot(out.out[0], out.out[1], skip) > 0.9 * energy(out.out[0], skip));
    CHECK(std::fabs(dot(out.out[0], out.out[3], skip)) < 0.1 * energy(out.out[0], skip));
    // W passes at unit gain (delay aside): same energy as the recording's W (SN3D W = N3D W).
    CHECK(10 * std::log10(energy(out.out[0], skip) / energy(in[0], skip)) == Approx(0).margin(0.5));

    // Head turned 90 degrees to the left: the sound is now straight ahead.
    Scene turned = s;
    turned.listener.head.yawOffsetDeg = 90;
    out = render(turned, cfg, in, n);
    CHECK(std::fabs(dot(out.out[0], out.out[1], skip)) < 0.1 * energy(out.out[0], skip));
    CHECK(dot(out.out[0], out.out[3], skip) > 0.9 * energy(out.out[0], skip));

    // The recording turned 90 degrees the other way instead: same result for
    // the listener as turning the head towards it... the sound was on the
    // recording's left; the recording's front now points right, so its left
    // points forward.
    Scene rec = s;
    rec.layers[0].ambisonic.yawDeg = -90;
    out = render(rec, cfg, in, n);
    CHECK(dot(out.out[0], out.out[3], skip) > 0.9 * energy(out.out[0], skip));
}

TEST_CASE("Renderer: FuMa input lands on the same directions as ambiX") {
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 12000;
    const auto sig = noise(n);
    const Vec3 dir = Vec3{0.6f, 0.2f, -0.5f}.normalized();
    const auto ambix = planeWaveAmbix(dir, sig);
    Scene s = fieldScene(4);
    auto ref = render(s, cfg, ambix, n);
    // FuMa: W X Y Z with W at 1 / sqrt(2).
    std::vector<std::vector<float>> fuma = {ambix[0], ambix[3], ambix[1], ambix[2]};
    for (float& v : fuma[0]) v *= 0.70710678f;
    s.layers[0].ambisonic.format = Layer::Ambisonic::Format::FuMa;
    auto out = render(s, cfg, fuma, n);
    for (int c = 0; c < 4; ++c)
        for (int i = 1000; i < n; i += 997) CHECK(out.out[static_cast<size_t>(c)][static_cast<size_t>(i)] == Approx(ref.out[static_cast<size_t>(c)][static_cast<size_t>(i)]).margin(1e-4));
}

TEST_CASE("Renderer: an Ambisonic layer is heard binaurally, with reverb only when sent, and third order loads") {
    Scene s = fieldScene(4);
    s.room.type = RoomType::Box;
    RenderConfig cfg;
    cfg.hrtfPath = SP_TEST_HRTF;
    const int n = 24000;
    const auto in = planeWaveAmbix({-1, 0, 0}, smoothNoise(n));
    // The recording stops half way, so the tail shows what rings on.
    std::vector<std::vector<float>> shortIn = in;
    for (auto& ch : shortIn) std::fill(ch.begin() + n / 2, ch.end(), 0.0f);
    auto out = render(s, cfg, shortIn, n);
    const double eL = energy(out.out[0], 2400), eR = energy(out.out[1], 2400);
    CHECK(10 * std::log10(eL / eR) > 3.0);
    // No room send: the output stops with the input (within the delay and HRTF).
    const size_t after = static_cast<size_t>(n / 2 + 4800);
    CHECK(energy(out.out[0], after) < 1e-3 * eL);
    // With the send on, the room rings on after the input stops.
    s.layers[0].ambisonic.roomSend = true;
    auto sent = render(s, cfg, shortIn, n);
    CHECK(energy(sent.out[0], after) > 100 * energy(out.out[0], after));

    // A third-order recording (16 channels) renders; its W alone is heard like the first-order one.
    Scene third = fieldScene(16);
    third.room.type = RoomType::Box;
    std::vector<std::vector<float>> in16(16, std::vector<float>(n, 0.0f));
    for (int c = 0; c < 4; ++c) in16[static_cast<size_t>(c)] = shortIn[static_cast<size_t>(c)];
    Renderer r(third, cfg, 1);
    CHECK(r.numInputs() == 16);
    auto o16 = render(third, cfg, in16, n);
    CHECK(10 * std::log10(energy(o16.out[0], 2400) / energy(out.out[0], 2400)) == Approx(0).margin(0.5));
}

TEST_CASE("Renderer: walking out of the sphere lowers the level and the field follows the sphere") {
    Scene s = fieldScene(4);
    s.layers[0].position = {0, 1.6f, -4};   // the sphere's centre is 4 m ahead; radius 3
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    const int n = 24000;
    const auto in = planeWaveAmbix({1, 0, 0}, noise(n));   // on the recording's right
    // Inside (1 m from the centre): the sound stays on the right.
    s.listener.staticPosition = {0, 1.6f, -3};
    auto inside = render(s, cfg, in, n);
    CHECK(dot(inside.out[0], inside.out[1], 2400) < -0.8 * energy(inside.out[0], 2400));
    // Outside, 10 m behind the sphere (14 m from its centre): the sound on the
    // surface was sqrt(9 + 1) m away inside and is sqrt(9 + 196) m away now,
    // so about 13 dB quieter, and it now comes from ahead.
    s.listener.staticPosition = {0, 1.6f, 10};
    auto outside = render(s, cfg, in, n);
    const double expected = 20 * std::log10(3.0 / std::sqrt(205.0)) - 20 * std::log10(3.0 / std::sqrt(10.0));
    CHECK(10 * std::log10(energy(outside.out[0], 4800) / energy(inside.out[0], 4800)) == Approx(expected).margin(1.5));
    CHECK(dot(outside.out[0], outside.out[3], 4800) > 0.9 * energy(outside.out[0], 4800));
}

TEST_CASE("Scene JSON: Ambisonic layers round-trip, validate, and old files load") {
    Scene s;
    Layer l;
    l.name = "field";
    l.channels = 9;
    l.ambisonic.radius = 4.5f;
    l.ambisonic.yawDeg = 30;
    l.ambisonic.roomSend = true;
    l.audioFiles = {"w.wav", "y.wav"};
    s.layers.push_back(l);
    const Scene back = sceneFromJson(sceneToJson(s));
    REQUIRE(back.layers.size() == 1);
    CHECK(back.layers[0].channels == 9);
    CHECK(isAmbisonic(back.layers[0]));
    CHECK(back.layers[0].ambisonic.radius == Approx(4.5f));
    CHECK(back.layers[0].ambisonic.yawDeg == Approx(30));
    CHECK(back.layers[0].ambisonic.roomSend);
    CHECK(back.layers[0].audioFiles.size() == 2);
    CHECK(inputChannels(back) == 9);
    CHECK(sceneFromJson(R"({"layers":[{"channels":4,"ambisonic":{"format":"fuma"}}]})").layers[0].ambisonic.format == Layer::Ambisonic::Format::FuMa);
    CHECK_THROWS(sceneFromJson(R"({"layers":[{"channels":3}]})"));
    CHECK_THROWS(sceneFromJson(R"({"layers":[{"channels":9,"ambisonic":{"format":"fuma"}}]})"));
    CHECK_THROWS(sceneFromJson(R"({"layers":[{"channels":4,"ambisonic":{"radius":0}}]})"));
    CHECK(sceneFromJson(R"({"layers":[{"name":"old"}]})").layers[0].channels == 1);
}

TEST_CASE("Live update: an Ambisonic layer's radius, orientation and format change without a new renderer") {
    Scene s = fieldScene(4);
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    cfg.maxDistance = 50;   // room to move the sphere without a new renderer
    Renderer r(s, cfg, 10);
    Scene edited = s;
    edited.layers[0].ambisonic.radius = 6;
    edited.layers[0].ambisonic.yawDeg = 45;
    edited.layers[0].ambisonic.format = Layer::Ambisonic::Format::FuMa;
    edited.layers[0].position = {1, 1.6f, -1};
    CHECK(r.prepareUpdate(edited) != nullptr);
    Scene more = s;
    more.layers[0].channels = 9;
    CHECK(r.prepareUpdate(more) == nullptr);
}
