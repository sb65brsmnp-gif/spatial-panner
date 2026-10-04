// sp-gensignals: synthesises the test sources used by the demo scenes, so the
// repository carries no third-party audio.
//
//   sp-gensignals OUT_DIR [--rate 48000] [--seconds 30]
//
// Writes mono 24-bit WAVs: engine, clicks, pluck, voice, bell, drone, noise_bursts, stream,
// two synthetic room impulse responses: hall_ir (stereo, 2.5 s), plate_ir (mono, 1.4 s),
// and field (4-channel first-order ambiX): the sources above as one Ambisonic
// recording, each from its own direction.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "WavIo.h"
#include "sp/Math.h"

using namespace sp;

namespace {

struct Rng {
    std::mt19937 g{12345};
    std::uniform_real_distribution<float> u{-1.0f, 1.0f};
    float next() { return u(g); }
};

struct OnePoleLp {
    float a = 0, z = 0;
    void set(float fc, float fs) { a = std::exp(-kTwoPi * fc / fs); }
    float process(float x) { z = a * z + (1 - a) * x; return z; }
};

// Two-pole resonator (for formants).
struct Resonator {
    float b0 = 1, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(float f, float bw, float fs) {
        const float r = std::exp(-kPi * bw / fs);
        a1 = -2 * r * std::cos(kTwoPi * f / fs);
        a2 = r * r;
        b0 = (1 - r) * std::sqrt(1 - 2 * r * std::cos(2 * kTwoPi * f / fs) + r * r);
    }
    float process(float x) {
        const float y = b0 * x - a1 * z1 - a2 * z2;
        z2 = z1; z1 = y;
        return y;
    }
};

float normalise(std::vector<float>& v, float peakDb = -3.0f) {
    float peak = 0;
    for (float x : v) peak = std::max(peak, std::fabs(x));
    const float g = peak > 0 ? dbToGain(peakDb) / peak : 1.0f;
    for (float& x : v) x *= g;
    return g;
}

// Motorbike-ish: pulsed sawtooth with a slowly wandering rpm, plus exhaust noise.
std::vector<float> engine(int n, float fs) {
    std::vector<float> v(n);
    Rng rng;
    OnePoleLp body, noiseLp;
    body.set(1800, fs);
    noiseLp.set(900, fs);
    double phase = 0;
    for (int i = 0; i < n; ++i) {
        const float t = i / fs;
        const float rpm = 95.0f + 25.0f * std::sin(0.23f * t) + 8.0f * std::sin(1.7f * t);
        phase += rpm / fs;
        if (phase >= 1) phase -= 1;
        const float saw = 2.0f * static_cast<float>(phase) - 1.0f;
        const float pulse = std::pow(std::max(0.0f, std::cos(kTwoPi * static_cast<float>(phase))), 3.0f);
        float x = body.process(saw * 0.6f + pulse * 0.8f);
        x += noiseLp.process(rng.next()) * 0.35f * (0.7f + 0.3f * pulse);
        v[i] = x;
    }
    normalise(v);
    return v;
}

// Sharp clicks at 100 BPM: ideal for hearing reflections and head turns.
std::vector<float> clicks(int n, float fs) {
    std::vector<float> v(n, 0.0f);
    const int period = static_cast<int>(fs * 0.6f);
    for (int i = 0; i < n; i += period) {
        for (int k = 0; k < 400 && i + k < n; ++k) {
            const float env = std::exp(-k / 60.0f);
            v[i + k] += env * std::sin(0.9f * k) * 0.9f + (k < 3 ? 0.8f : 0.0f);
        }
    }
    normalise(v);
    return v;
}

// Karplus-Strong plucked notes on a pentatonic scale.
std::vector<float> pluck(int n, float fs) {
    std::vector<float> v(n, 0.0f);
    Rng rng;
    const float scale[] = {220.0f, 246.94f, 293.66f, 329.63f, 392.0f, 440.0f, 493.88f, 587.33f};
    const int noteLen = static_cast<int>(fs * 0.45f);
    int idx = 0;
    for (int start = 0; start < n; start += noteLen) {
        const float f = scale[(idx * 3 + idx / 3) % 8];
        ++idx;
        const int N = static_cast<int>(fs / f);
        std::vector<float> ks(N);
        for (float& s : ks) s = rng.next();
        int p = 0;
        for (int k = 0; k < static_cast<int>(fs * 1.4f) && start + k < n; ++k) {
            const float cur = ks[p];
            const float nxt = ks[(p + 1) % N];
            ks[p] = 0.5f * (cur + nxt) * 0.998f;
            p = (p + 1) % N;
            v[start + k] += cur;
        }
    }
    normalise(v);
    return v;
}

// Vowel-like phrases: glottal pulses through three formants, with pauses.
std::vector<float> voice(int n, float fs) {
    std::vector<float> v(n, 0.0f);
    Rng rng;
    const float vowels[5][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}, {640, 1190, 2390}};
    Resonator f1, f2, f3;
    OnePoleLp breathLp;
    breathLp.set(3000, fs);
    double phase = 0;
    int vowel = 0;
    for (int i = 0; i < n; ++i) {
        const float t = i / fs;
        // Syllables of ~0.28 s, phrases of 5 syllables then a 1.2 s pause.
        const float phraseT = std::fmod(t, 2.6f);
        const bool speaking = phraseT < 1.4f;
        const int syl = static_cast<int>(phraseT / 0.28f);
        if (i % 2400 == 0) {
            vowel = (syl + static_cast<int>(t / 2.6f)) % 5;
            f1.set(vowels[vowel][0], 80, fs);
            f2.set(vowels[vowel][1], 100, fs);
            f3.set(vowels[vowel][2], 140, fs);
        }
        const float f0 = 118.0f + 14.0f * std::sin(5.1f * t) + 20.0f * std::sin(0.9f * t) - 25.0f * (phraseT / 1.4f);
        phase += f0 / fs;
        if (phase >= 1) phase -= 1;
        // Rosenberg-like glottal pulse.
        const float ph = static_cast<float>(phase);
        const float glottal = ph < 0.4f ? 0.5f * (1 - std::cos(kPi * ph / 0.4f)) : (ph < 0.6f ? std::cos(kPi * (ph - 0.4f) / 0.4f) : 0.0f);
        const float sylEnv = speaking ? std::sin(kPi * std::fmod(phraseT, 0.28f) / 0.28f) : 0.0f;
        const float src = (glottal - 0.3f) * 0.8f + breathLp.process(rng.next()) * 0.04f;
        const float y = (f1.process(src) * 1.0f + f2.process(src) * 0.5f + f3.process(src) * 0.25f);
        v[i] = y * std::pow(sylEnv, 0.6f);
    }
    normalise(v);
    return v;
}

// FM bell struck every 2.5 s.
std::vector<float> bell(int n, float fs) {
    std::vector<float> v(n, 0.0f);
    const int period = static_cast<int>(fs * 2.5f);
    const float carriers[] = {523.25f, 659.25f, 783.99f};
    int hit = 0;
    for (int start = 0; start < n; start += period, ++hit) {
        const float fc = carriers[hit % 3];
        for (int k = 0; k < static_cast<int>(fs * 2.4f) && start + k < n; ++k) {
            const float t = k / fs;
            const float env = std::exp(-t * 1.6f);
            const float mod = std::sin(kTwoPi * fc * 1.4f * t) * 2.5f * std::exp(-t * 3.0f);
            v[start + k] += env * (std::sin(kTwoPi * fc * t + mod) + 0.4f * std::sin(kTwoPi * fc * 2.76f * t) * std::exp(-t * 4));
        }
    }
    normalise(v);
    return v;
}

// Slow detuned pad.
std::vector<float> drone(int n, float fs) {
    std::vector<float> v(n, 0.0f);
    const float freqs[] = {110.0f, 110.3f, 164.8f, 165.1f, 220.4f, 277.2f};
    OnePoleLp lp;
    lp.set(1200, fs);
    for (int i = 0; i < n; ++i) {
        const float t = i / fs;
        float x = 0;
        for (int k = 0; k < 6; ++k) {
            const float f = freqs[k] * (1 + 0.002f * std::sin(0.3f * t + k));
            const float saw = 2.0f * std::fmod(f * t, 1.0f) - 1.0f;
            x += saw * (0.5f + 0.5f * std::sin(0.17f * t + k * 1.3f)) / 6.0f;
        }
        v[i] = lp.process(x) * (0.8f + 0.2f * std::sin(0.11f * t));
    }
    normalise(v, -6.0f);
    return v;
}

// Pink-ish noise bursts every 0.9 s.
std::vector<float> noiseBursts(int n, float fs) {
    std::vector<float> v(n, 0.0f);
    Rng rng;
    OnePoleLp lp;
    lp.set(4000, fs);
    const int period = static_cast<int>(fs * 0.9f);
    for (int i = 0; i < n; ++i) {
        const int k = i % period;
        const float env = k < static_cast<int>(fs * 0.12f) ? std::sin(kPi * k / (fs * 0.12f)) : 0.0f;
        v[i] = lp.process(rng.next()) * env;
    }
    normalise(v);
    return v;
}

// Running water / stream: modulated filtered noise.
std::vector<float> stream(int n, float fs) {
    std::vector<float> v(n, 0.0f);
    Rng rng;
    Resonator r1, r2;
    OnePoleLp lp;
    lp.set(6000, fs);
    for (int i = 0; i < n; ++i) {
        const float t = i / fs;
        if (i % 480 == 0) {
            r1.set(1200 + 500 * std::sin(0.7f * t) + 200 * rng.next(), 400, fs);
            r2.set(2800 + 900 * std::sin(0.43f * t + 1) + 300 * rng.next(), 700, fs);
        }
        const float w = rng.next();
        v[i] = lp.process(r1.process(w) + 0.7f * r2.process(w) + 0.1f * w);
    }
    normalise(v, -6.0f);
    return v;
}

// ------------------------------------------------------------------ test impulse responses
// Synthetic room impulse responses for the Room tab's "Impulse response" late
// reverb (docs/editor.md): exponentially decaying noise, shorter at high
// frequencies, with a few early echoes in front of the diffuse tail. They are
// not recordings of a real room; they exist so the IR path can be exercised
// and heard without third-party files.

// One decaying-noise tail: RT60 `t60` at low frequencies falling to `t60High`
// above ~2 kHz, with a sparse early part before `mix` seconds.
std::vector<float> irTail(int n, float fs, float t60, float t60High, float mix, unsigned seed) {
    std::vector<float> v(n, 0.0f);
    Rng rng;
    rng.g.seed(seed);
    // Two bands: low-passed noise decays slowly, the remainder decays faster.
    OnePoleLp lp;
    lp.set(2000, fs);
    const float kLow = -3.0f * std::log(10.0f) / (t60 * fs);        // per-sample log-amplitude slope
    const float kHigh = -3.0f * std::log(10.0f) / (t60High * fs);
    for (int i = 0; i < n; ++i) {
        const float t = i / fs;
        const float w = rng.next();
        const float low = lp.process(w);
        const float high = w - low;
        // Sparse until `mix` (echo density grows as t^2), dense afterwards.
        float density = t < mix ? 0.02f + 0.98f * (t / mix) * (t / mix) : 1.0f;
        const bool on = std::fabs(rng.next()) < density;
        if (!on) continue;
        v[i] = (low * std::exp(kLow * i) + high * std::exp(kHigh * i)) / std::sqrt(density);
    }
    return v;
}

// A medium hall, stereo (2.5 s, RT60 ~2.2 s low / 1.1 s high): the two
// channels are independent noise so they decorrelate like spaced microphones,
// with a common first echo set so the early part stays centred.
std::vector<std::vector<float>> hallIr(float fs) {
    const int n = static_cast<int>(2.5f * fs);
    std::vector<std::vector<float>> ch = {irTail(n, fs, 2.2f, 1.1f, 0.08f, 11u), irTail(n, fs, 2.2f, 1.1f, 0.08f, 23u)};
    const int preDelay = static_cast<int>(0.018f * fs);
    for (auto& c : ch) {
        std::rotate(c.rbegin(), c.rbegin() + preDelay, c.rend());
        std::fill(c.begin(), c.begin() + preDelay, 0.0f);
    }
    // Shared early echoes (ms, gain) with a little left/right difference.
    const float echoes[][2] = {{21, 0.5f}, {27, 0.35f}, {34, 0.3f}, {45, 0.22f}};
    for (const auto& e : echoes) {
        const int i = static_cast<int>(e[0] * 1e-3f * fs);
        ch[0][i] += e[1] * 1.1f;
        ch[1][i + 3] += e[1] * 0.9f;
    }
    float peak = 0;
    for (const auto& c : ch) for (float x : c) peak = std::max(peak, std::fabs(x));
    for (auto& c : ch) for (float& x : c) x *= dbToGain(-1.0f) / peak;
    return ch;
}

// A bright plate, mono (1.4 s, nearly flat decay): dense from the start.
std::vector<std::vector<float>> plateIr(float fs) {
    const int n = static_cast<int>(1.4f * fs);
    std::vector<float> v = irTail(n, fs, 1.3f, 1.0f, 0.002f, 37u);
    normalise(v, -1.0f);
    return {v};
}

// A first-order ambiX (ACN/SN3D) "recording": each source encoded from a fixed
// direction around the microphone. Directions are engine axes (x right, y up,
// -z forward); ambiX takes x forward, y left, z up.
std::vector<std::vector<float>> field(int n, float fs) {
    struct Src { std::vector<float> (*fn)(int, float); float dir[3]; float db; };
    const Src srcs[] = {
        {voice, {-0.5f, -0.1f, -0.85f}, -6},    // ahead left, a little below (seated)
        {clicks, {0.9f, 0.1f, 0.4f}, -12},      // right, slightly behind
        {bell, {0.3f, 0.7f, 0.65f}, -10},       // behind, high up
        {stream, {-0.95f, -0.3f, 0.1f}, -14},   // left, low
        {drone, {0.0f, 0.95f, -0.3f}, -16},     // overhead
    };
    std::vector<std::vector<float>> ch(4, std::vector<float>(static_cast<size_t>(n), 0.0f));
    for (const Src& s : srcs) {
        const Vec3 d = Vec3{s.dir[0], s.dir[1], s.dir[2]}.normalized();
        const float ax = -d.z, ay = -d.x, az = d.y;   // ambiX frame
        const float g[4] = {1.0f, ay, az, ax};         // W, Y, Z, X (SN3D: W = 1, first order = direction cosines)
        const std::vector<float> v = s.fn(n, fs);
        const float lvl = dbToGain(s.db);
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < n; ++i) ch[static_cast<size_t>(c)][static_cast<size_t>(i)] += v[static_cast<size_t>(i)] * lvl * g[c];
    }
    float peak = 0;
    for (const auto& c : ch) for (float x : c) peak = std::max(peak, std::fabs(x));
    for (auto& c : ch) for (float& x : c) x *= dbToGain(-1.0f) / peak;
    return ch;
}

}  // namespace

int main(int argc, char** argv) {
    std::string outDir;
    int rate = 48000;
    float seconds = 30;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = std::stoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::stof(argv[++i]);
        else outDir = a;
    }
    if (outDir.empty()) {
        std::fprintf(stderr, "usage: sp-gensignals OUT_DIR [--rate 48000] [--seconds 30]\n");
        return 2;
    }
    std::filesystem::create_directories(outDir);
    const int n = static_cast<int>(seconds * rate);
    const float fs = static_cast<float>(rate);
    struct Gen { const char* name; std::vector<float> (*fn)(int, float); };
    const Gen gens[] = {{"engine", engine}, {"clicks", clicks}, {"pluck", pluck}, {"voice", voice},
                        {"bell", bell}, {"drone", drone}, {"noise_bursts", noiseBursts}, {"stream", stream}};
    for (const auto& g : gens) {
        const std::string path = (std::filesystem::path(outDir) / (std::string(g.name) + ".wav")).string();
        tools::writeWav(path, {g.fn(n, fs)}, rate);
        std::fprintf(stderr, "wrote %s\n", path.c_str());
    }
    struct IrGen { const char* name; std::vector<std::vector<float>> (*fn)(float); };
    const IrGen irs[] = {{"hall_ir", hallIr}, {"plate_ir", plateIr}};
    for (const auto& g : irs) {
        const std::string path = (std::filesystem::path(outDir) / (std::string(g.name) + ".wav")).string();
        tools::writeWav(path, g.fn(fs), rate);
        std::fprintf(stderr, "wrote %s\n", path.c_str());
    }
    {
        const std::string path = (std::filesystem::path(outDir) / "field.wav").string();
        tools::writeWav(path, field(n, fs), rate);
        std::fprintf(stderr, "wrote %s (ambiX, 4 channels)\n", path.c_str());
    }
    return 0;
}
