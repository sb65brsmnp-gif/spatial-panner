// sp-render: scene.json + audio files -> WAV.
//
//   sp-render scene.json -o out.wav [--mode binaural|speakers|ambix]
//             [--layout 7.1.4] [--hrtf file.sofa] [--duration s] [--rate 48000]
//             [--order 3] [--float] [--bench]
//
// Layer audio paths in the scene are resolved relative to the scene file.
// Inputs are resampled to the render rate when needed.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "WavIo.h"
#include "saf.h"
#include "sp/Renderer.h"
#include "sp/SceneJson.h"

namespace fs = std::filesystem;
using namespace sp;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: sp-render scene.json -o out.wav [options]\n"
                 "  --mode binaural|speakers|ambix   output mode (default binaural)\n"
                 "  --layout NAME                    speaker layout: stereo quad 5.1 7.1 5.1.4 7.1.4 9.1.6\n"
                 "  --hrtf FILE.sofa                 HRTF for binaural (default: built-in SADIE II KU100)\n"
                 "  --order N                        Ambisonics order 1..3 (default 3)\n"
                 "  --duration S                     render length in seconds (default: longest layer)\n"
                 "  --rate HZ                        sample rate (default 48000)\n"
                 "  --block N                        host block size to simulate (default 512)\n"
                 "  --float                          write 32-bit float instead of 24-bit PCM\n"
                 "  --normalize                      scale the output so its peak is -1 dBFS\n"
                 "  --bench                          print CPU usage (realtime factor)\n"
                 "  --no-reflections / --no-reverb   disable room parts\n");
}

std::vector<float> resampleLinear(const std::vector<float>& in, int fromRate, int toRate) {
    if (fromRate == toRate) return in;
    // speex resampler (bundled with SAF): good quality, no extra dependency.
    int err = 0;
    SpeexResamplerState* st = speex_resampler_init(1, fromRate, toRate, 8, &err);
    if (!st) throw std::runtime_error("Resampler init failed");
    const spx_uint32_t inLen = static_cast<spx_uint32_t>(in.size());
    std::vector<float> out(static_cast<size_t>(std::ceil(in.size() * static_cast<double>(toRate) / fromRate)) + 64);
    spx_uint32_t il = inLen, ol = static_cast<spx_uint32_t>(out.size());
    speex_resampler_process_float(st, 0, in.data(), &il, out.data(), &ol);
    speex_resampler_destroy(st);
    out.resize(ol);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenePath, outPath, modeStr = "binaural", layoutName, hrtfPath;
    int order = 3, rate = 48000, block = 512;
    double duration = 0;
    bool floatOut = false, bench = false, noRefl = false, noReverb = false, normalize = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "-o" || a == "--out") outPath = next("-o");
        else if (a == "--mode") modeStr = next("--mode");
        else if (a == "--layout") layoutName = next("--layout");
        else if (a == "--hrtf") hrtfPath = next("--hrtf");
        else if (a == "--order") order = std::stoi(next("--order"));
        else if (a == "--duration") duration = std::stod(next("--duration"));
        else if (a == "--rate") rate = std::stoi(next("--rate"));
        else if (a == "--block") block = std::stoi(next("--block"));
        else if (a == "--float") floatOut = true;
        else if (a == "--normalize") normalize = true;
        else if (a == "--bench") bench = true;
        else if (a == "--no-reflections") noRefl = true;
        else if (a == "--no-reverb") noReverb = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else scenePath = a;
    }
    if (scenePath.empty() || outPath.empty()) { usage(); return 2; }

    try {
        Scene scene = loadSceneFile(scenePath);
        if (noRefl) scene.room.reflectionsEnabled = false;
        if (noReverb) scene.room.reverbEnabled = false;

        RenderConfig cfg;
        cfg.sampleRate = rate;
        cfg.ambisonicsOrder = order;
        if (modeStr == "binaural") cfg.mode = OutputMode::Binaural;
        else if (modeStr == "speakers") cfg.mode = OutputMode::Speakers;
        else if (modeStr == "ambix" || modeStr == "ambisonics") cfg.mode = OutputMode::Ambisonics;
        else { std::fprintf(stderr, "unknown mode %s\n", modeStr.c_str()); return 2; }
        if (!layoutName.empty()) {
            cfg.layout = SpeakerLayout::preset(layoutName);
            if (cfg.layout.numChannels() == 0) { std::fprintf(stderr, "unknown layout %s\n", layoutName.c_str()); return 2; }
            if (modeStr == "binaural") cfg.mode = OutputMode::Speakers;
        }
        if (!hrtfPath.empty()) cfg.hrtfPath = hrtfPath;
#ifdef SP_DEFAULT_HRTF
        else cfg.hrtfPath = SP_DEFAULT_HRTF;
#endif

        // Load layer audio.
        const fs::path sceneDir = fs::absolute(scenePath).parent_path();
        std::vector<std::vector<float>> audio(scene.layers.size());
        size_t longest = 0;
        for (size_t i = 0; i < scene.layers.size(); ++i) {
            const Layer& l = scene.layers[i];
            if (l.audioFile.empty()) continue;
            fs::path p = l.audioFile;
            if (p.is_relative()) p = sceneDir / p;
            tools::AudioFile f = tools::readWav(p.string());
            // Mono mix.
            std::vector<float> mono(f.frames(), 0.0f);
            for (int c = 0; c < f.channels; ++c)
                for (size_t n = 0; n < mono.size(); ++n) mono[n] += f.data[c][n] / f.channels;
            audio[i] = resampleLinear(mono, f.sampleRate, rate);
            const size_t end = static_cast<size_t>(l.startTime * rate) + audio[i].size();
            if (!l.loop) longest = std::max(longest, end);
            std::fprintf(stderr, "layer %-16s %s (%.1f s)\n", l.name.c_str(), p.filename().string().c_str(),
                         audio[i].size() / static_cast<double>(rate));
        }
        if (duration <= 0) duration = scene.duration;
        if (duration <= 0) duration = longest / static_cast<double>(rate);
        if (duration <= 0) { std::fprintf(stderr, "nothing to render (no audio and no duration)\n"); return 1; }

        Renderer renderer(scene, cfg, duration);
        const auto st = renderer.stats();
        std::fprintf(stderr, "render: %s, %d outputs, %.1f s at %d Hz; %d images/layer, RT60 mid %.2f s, delay line %.0f m\n",
                     modeStr.c_str(), renderer.numOutputs(), duration, rate, st.numImagesPerLayer, st.reverbRt60Mid,
                     st.maxDistance);

        const int latency = renderer.latencySamples();
        const size_t totalFrames = static_cast<size_t>(duration * rate);
        std::vector<std::vector<float>> out(renderer.numOutputs(), std::vector<float>(totalFrames + latency, 0.0f));
        std::vector<float> inBlock(static_cast<size_t>(scene.layers.size()) * block);
        std::vector<const float*> inPtrs(scene.layers.size());
        std::vector<float*> outPtrs(renderer.numOutputs());

        const auto t0 = std::chrono::steady_clock::now();
        size_t pos = 0;
        while (pos < totalFrames + latency) {
            const int n = static_cast<int>(std::min<size_t>(block, totalFrames + latency - pos));
            for (size_t i = 0; i < scene.layers.size(); ++i) {
                float* dst = inBlock.data() + i * block;
                const Layer& l = scene.layers[i];
                const auto& a = audio[i];
                const long start = static_cast<long>(l.startTime * rate);
                for (int k = 0; k < n; ++k) {
                    long idx = static_cast<long>(pos + k) - start;
                    float v = 0;
                    if (!a.empty() && idx >= 0) {
                        if (l.loop) idx %= static_cast<long>(a.size());
                        if (idx < static_cast<long>(a.size())) v = a[idx];
                    }
                    dst[k] = v;
                }
                inPtrs[i] = dst;
            }
            for (int c = 0; c < renderer.numOutputs(); ++c) outPtrs[c] = out[c].data() + pos;
            renderer.process(inPtrs.data(), outPtrs.data(), n, pos / static_cast<double>(rate));
            pos += n;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (bench) {
            std::fprintf(stderr, "bench: %zu layers, %.2f s audio rendered in %.2f s -> %.1f%% of one core\n",
                         scene.layers.size(), duration, secs, 100.0 * secs / duration);
        }

        for (auto& ch : out) ch.erase(ch.begin(), ch.begin() + latency);
        float peak = 0;
        for (auto& ch : out)
            for (float v : ch) peak = std::max(peak, std::fabs(v));
        std::fprintf(stderr, "peak %.2f dBFS\n", 20 * std::log10(std::max(peak, 1e-9f)));
        if (normalize && peak > 0) {
            const float g = dbToGain(-1.0f) / peak;
            for (auto& ch : out)
                for (float& v : ch) v *= g;
            std::fprintf(stderr, "normalized by %+.2f dB\n", gainToDb(g));
        } else if (peak > 1.0f && !floatOut) {
            std::fprintf(stderr, "warning: output clips; lower layer levels, use --normalize or --float\n");
        }
        tools::writeWav(outPath, out, rate, floatOut);
        std::fprintf(stderr, "wrote %s\n", outPath.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
