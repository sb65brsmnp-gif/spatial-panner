// Room model calibration: the direct-to-reverberant ratio against
// Hopkins-Stryker, energy conservation when walls scatter, and the flutter
// echo between parallel walls.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "dsp/RoomAcoustics.h"
#include "sp/Renderer.h"

using namespace sp;
using Catch::Approx;

namespace {

constexpr double kFs = 48000;

// W channel (ambiX, SN3D W = N3D W) of a click rendered in `scene`.
std::vector<float> renderW(const Scene& scene, float seconds) {
    RenderConfig cfg;
    cfg.mode = OutputMode::Ambisonics;
    cfg.ambisonicsOrder = 1;
    Renderer r(scene, cfg, seconds + 1);
    const int n = static_cast<int>(kFs * seconds), lat = r.latencySamples();
    std::vector<float> click(n + lat, 0.0f);
    click[0] = 1.0f;
    std::vector<std::vector<float>> out(r.numOutputs(), std::vector<float>(n + lat, 0.0f));
    std::vector<float*> o(r.numOutputs());
    const int block = 256;
    for (int pos = 0; pos < n + lat; pos += block) {
        const int k = std::min(block, n + lat - pos);
        const float* in[1] = {click.data() + pos};
        for (int c = 0; c < r.numOutputs(); ++c) o[c] = out[c].data() + pos;
        r.process(in, o.data(), k, pos / kFs);
    }
    std::vector<float> w(out[0].begin() + lat, out[0].end());
    return w;
}

double energy(const std::vector<float>& v, int from, int to) {
    double e = 0;
    for (int i = from; i < std::min<int>(to, static_cast<int>(v.size())); ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
}

// RBJ band-pass, 500 Hz - 2 kHz-ish (f0 1 kHz, Q 0.7): the mid band the
// reverb level is calibrated in.
std::vector<float> midBand(const std::vector<float>& x) {
    const double f0 = 1000, Q = 0.7;
    const double w0 = 2 * M_PI * f0 / kFs, alpha = std::sin(w0) / (2 * Q), c = std::cos(w0);
    const double b0 = alpha, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
    std::vector<float> y(x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double v = (b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1; x1 = x[i]; y2 = y1; y1 = v;
        y[i] = static_cast<float>(v);
    }
    return y;
}

// A tall plaster box with the listener at mid height, so the earliest
// reflection (off the floor or ceiling) trails the direct sound by over 6 ms
// even for a source 4 m away: the direct energy can be windowed out.
Scene plasterRoom() {
    Scene s;
    s.room.type = RoomType::Box;
    s.room.size = {8, 5, 10};
    for (auto& m : s.room.materials) m = materials::byName("plaster");
    s.environment.airAbsorption = false;
    // Off the centre lines: on them, mirror-image pairs (floor/ceiling,
    // left/right, and floor/ceiling whenever the heights add up to the room
    // height) arrive at the same instant and add coherently, 3 dB above
    // the incoherent sum the statistical model assumes.
    s.listener.staticPosition = {-0.6f, 2.1f, 1.0f};
    return s;
}

void setScattering(Scene& s, float sc) {
    for (auto& m : s.room.materials) m.scattering = sc;
}

}  // namespace

TEST_CASE("Direct-to-reverberant ratio follows Hopkins-Stryker at two distances") {
    Scene s = plasterRoom();
    const dsp::RoomStats st = dsp::computeRoomStats(s.room, s.environment);
    const float Rmid = dsp::bandAverage(st.roomConstant, 2, 3);
    for (float dz : {2.0f, 4.0f}) {
        Layer l;
        l.position = {0.4f, 3.1f, 1.0f - dz};
        const float dist = (l.position - s.listener.staticPosition).length();
        s.layers = {l};
        const auto w = midBand(renderW(s, 3.0f));
        // Direct arrival: the first 4 ms after the click reaches the listener
        // (the earliest reflection, off the floor or ceiling, is over 6 ms later).
        const int t0 = static_cast<int>(dist / 343.0 * kFs) - 8;
        const int dEnd = t0 + static_cast<int>(0.004 * kFs);
        const double direct = energy(w, 0, dEnd), reverb = energy(w, dEnd, static_cast<int>(w.size()));
        const double measured = 10 * std::log10(direct / reverb);
        // Hopkins-Stryker: direct 1/(4 pi r^2) vs reverberant 4/R.
        const double expected = 10 * std::log10(Rmid / (16 * kPi * dist * dist));
        WARN("D/R at " << dist << " m: measured " << measured << " dB, Hopkins-Stryker " << expected << " dB (R = " << Rmid << " m^2)");
        {
            // Split: images alone and the FDN alone, each against its share
            // of 16 pi / R (relative to the direct energy 1 / r^2).
            Scene imgOnly = s;
            imgOnly.room.reverbEnabled = false;
            const auto wi = midBand(renderW(imgOnly, 3.0f));
            Scene fdnOnly = s;
            fdnOnly.room.reflectionsEnabled = false;
            const auto wf = midBand(renderW(fdnOnly, 3.0f));
            const double di = energy(wi, 0, dEnd);
            double imgTheory = 0;
            for (const auto& im : dsp::computeImages(s.room, l.position, s.room.reflectionOrder)) {
                if (im.order == 0) continue;
                const float d = (im.position - s.listener.staticPosition).length();
                const float g = im.specular * dsp::bandAverage(im.reflectance, 2, 3) / d;
                imgTheory += g * g;
            }
            const double total = 16 * kPi / Rmid;
            WARN("  images: " << 10 * std::log10(energy(wi, dEnd, static_cast<int>(wi.size())) / di) << " dB re direct (theory "
                 << 10 * std::log10(imgTheory * dist * dist) << "), FDN alone: "
                 << 10 * std::log10(energy(wf, dEnd, static_cast<int>(wf.size())) / energy(wf, 0, dEnd)) << " dB re direct (theory "
                 << 10 * std::log10(total * dist * dist) << ")");
        }
        CHECK(measured == Approx(expected).margin(1.5));
    }
}

TEST_CASE("Scattering moves energy from the images to the diffuse tail and conserves the total") {
    Scene s = plasterRoom();
    Layer l;
    l.position = {1.5f, 2.6f, -2.0f};
    s.layers = {l};
    const int start = static_cast<int>(0.012 * kFs);  // after the direct arrival (3.6 m: 10.6 ms)
    setScattering(s, 0.0f);
    const auto specular = midBand(renderW(s, 2.5f));
    setScattering(s, 0.5f);
    const auto scattered = midBand(renderW(s, 2.5f));
    const double eSpec = energy(specular, start, static_cast<int>(specular.size()));
    const double eScat = energy(scattered, start, static_cast<int>(scattered.size()));
    WARN("Reverberant energy, scattering 0 vs 0.5: " << 10 * std::log10(eScat / eSpec) << " dB difference");
    CHECK(10 * std::log10(eScat / eSpec) == Approx(0.0).margin(1.0));

    // Images only (reverb off): the specular part must drop with scattering.
    s.room.reverbEnabled = false;
    setScattering(s, 0.0f);
    const auto imgSpec = midBand(renderW(s, 0.5f));
    setScattering(s, 0.5f);
    const auto imgScat = midBand(renderW(s, 0.5f));
    const double drop = 10 * std::log10(energy(imgScat, start, static_cast<int>(imgScat.size())) / energy(imgSpec, start, static_cast<int>(imgSpec.size())));
    WARN("Image-source energy change with scattering 0.5: " << drop << " dB");
    CHECK(drop < -2.0);
}

TEST_CASE("Flutter between parallel walls is weaker with scattering walls") {
    // Narrow room: the side walls are 3 m apart and the source sits 0.5 m
    // from the listener on the centre line, so the lateral images arrive as
    // an echo train spaced by the 6 m round trip (17.5 ms).
    Scene s = plasterRoom();
    s.room.size = {3, 5, 12};
    s.listener.staticPosition = {0, 2.5f, 2.0f};
    Layer l;
    l.position = {0, 2.5f, 1.5f};  // on the centre line on purpose: the strongest flutter
    s.layers = {l};
    auto flutter = [&](const std::vector<float>& w) {
        // Share of the early reverberant energy (3 .. 60 ms after the direct
        // sound) that lies within +-1 ms of the lateral images, which sit at
        // x = +-3 m (one bounce) and +-6 m (two bounces).
        const int t0 = static_cast<int>(0.5 / 343.0 * kFs);
        const int from = t0 + static_cast<int>(0.003 * kFs), to = t0 + static_cast<int>(0.060 * kFs);
        double total = energy(w, from, to), inEchoes = 0;
        for (int k = 1; k <= 2; ++k) {
            const int c = static_cast<int>(std::sqrt(0.25 + 9.0 * k * k) / 343.0 * kFs);
            inEchoes += energy(w, c - static_cast<int>(0.001 * kFs), c + static_cast<int>(0.001 * kFs));
        }
        return inEchoes / total;
    };
    setScattering(s, 0.0f);
    const double mirror = flutter(renderW(s, 1.0f));
    setScattering(s, 0.5f);
    const double rough = flutter(renderW(s, 1.0f));
    const auto table = materials::byName("plaster");
    setScattering(s, table.scattering);
    const double builtin = flutter(renderW(s, 1.0f));
    WARN("Share of early energy in the flutter echoes: mirror walls " << mirror << ", scattering 0.5: " << rough
         << ", built-in plaster (" << table.scattering << "): " << builtin);
    CHECK(rough < 0.7 * mirror);
    CHECK(builtin < mirror);
}
