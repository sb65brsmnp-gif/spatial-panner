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
                 "  --no-reflections / --no-reverb   disable room parts\n"
                 "  --ir FILE.wav                    use this impulse response as the late reverb (overrides the scene's)\n"
                 "  --ir-gain DB / --ir-channels N   trim and interpretation (0 = from the file, 1 mono, 2 stereo, 4 ambiX)\n"
                 "  --reflections auto|builtin|steam reflections back-end (default auto: steam for mesh rooms/objects)\n"
                 "  --steam-rays N / --steam-bounces N   ray tracing effort (default 4096 / enough for the IR, max 96)\n"
                 "  --steam-block N                  reflection convolution block (default 256; smaller = costlier, less lag)\n"
                 "  --steam-ir S                     impulse response length (default: from the room)\n"
                 "  --steam-interval S               seconds between ray-tracing passes (default 0.1)\n"
                 "  --steam-reverb convolution|hybrid|parametric   tail rendering (default convolution)\n"
                 "  --steam-threads N                ray tracing threads (default: cores - 1)\n");
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
    std::string reflections = "auto", steamReverb = "convolution", irFile;
    double irGain = 0;
    int irChannels = -1;
    int steamRays = -1, steamBounces = -1, steamThreads = -1, steamBlock = -1;
    double steamIr = -1, steamInterval = -1;
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
        else if (a == "--ir") irFile = next("--ir");
        else if (a == "--ir-gain") irGain = std::stod(next("--ir-gain"));
        else if (a == "--ir-channels") irChannels = std::stoi(next("--ir-channels"));
        else if (a == "--reflections") reflections = next("--reflections");
        else if (a == "--steam-rays") steamRays = std::stoi(next("--steam-rays"));
        else if (a == "--steam-bounces") steamBounces = std::stoi(next("--steam-bounces"));
        else if (a == "--steam-threads") steamThreads = std::stoi(next("--steam-threads"));
        else if (a == "--steam-block") steamBlock = std::stoi(next("--steam-block"));
        else if (a == "--steam-ir") steamIr = std::stod(next("--steam-ir"));
        else if (a == "--steam-interval") steamInterval = std::stod(next("--steam-interval"));
        else if (a == "--steam-reverb") steamReverb = next("--steam-reverb");
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else scenePath = a;
    }
    if (scenePath.empty() || outPath.empty()) { usage(); return 2; }

    try {
        Scene scene = loadSceneFile(scenePath);
        if (noRefl) scene.room.reflectionsEnabled = false;
        if (noReverb) scene.room.reverbEnabled = false;
        if (!irFile.empty()) {
            scene.room.impulseResponse.file = fs::absolute(irFile).string();
            scene.room.impulseResponse.gainDb = static_cast<float>(irGain);
            scene.room.impulseResponse.enabled = true;
        }
        if (irChannels >= 0) scene.room.impulseResponse.channels = irChannels;

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
        if (reflections == "auto") cfg.reflections = ReflectionsBackend::Auto;
        else if (reflections == "builtin") cfg.reflections = ReflectionsBackend::Builtin;
        else if (reflections == "steam") cfg.reflections = ReflectionsBackend::SteamAudio;
        else { std::fprintf(stderr, "unknown reflections back-end %s\n", reflections.c_str()); return 2; }
        if (steamRays > 0) cfg.steam.rays = steamRays;
        if (steamBounces > 0) cfg.steam.bounces = steamBounces;
        if (steamThreads > 0) cfg.steam.threads = steamThreads;
        if (steamBlock > 0) cfg.steam.frameSize = steamBlock;
        if (steamIr > 0) cfg.steam.irSeconds = static_cast<float>(steamIr);
        if (steamInterval > 0) cfg.steam.updateInterval = static_cast<float>(steamInterval);
        if (steamReverb == "convolution") cfg.steam.reverb = SteamAudioSettings::Reverb::Convolution;
        else if (steamReverb == "hybrid") cfg.steam.reverb = SteamAudioSettings::Reverb::Hybrid;
        else if (steamReverb == "parametric") cfg.steam.reverb = SteamAudioSettings::Reverb::Parametric;
        else { std::fprintf(stderr, "unknown --steam-reverb %s\n", steamReverb.c_str()); return 2; }
        cfg.steam.asyncSimulation = false;  // offline: simulate in line, every interval
        if (!hrtfPath.empty()) cfg.hrtfPath = hrtfPath;
#ifdef SP_DEFAULT_HRTF
        else cfg.hrtfPath = SP_DEFAULT_HRTF;
#endif

        // Load layer audio.
        const fs::path sceneDir = fs::absolute(scenePath).parent_path();
        // One buffer per renderer input: a layer's channels in order (a mono
        // layer sums the file's channels; a stereo layer takes left and right;
        // an Ambisonic layer takes all its channels from one file, or one mono
        // file per channel from "audio_files").
        std::vector<std::vector<float>> audio(static_cast<size_t>(inputChannels(scene)));
        std::vector<size_t> firstInput(scene.layers.size());
        size_t longest = 0;
        for (size_t i = 0, input = 0; i < scene.layers.size(); ++i) {
            const Layer& l = scene.layers[i];
            const int nch = layerInputs(l);
            firstInput[i] = input;
            input += static_cast<size_t>(nch);
            const auto resolve = [&](const std::string& file) {
                fs::path p = file;
                if (p.is_relative()) p = sceneDir / p;
                return p;
            };
            std::string shown;
            if (isAmbisonic(l) && !l.audioFiles.empty()) {
                if (static_cast<int>(l.audioFiles.size()) != nch) {
                    std::fprintf(stderr, "layer %s: %d-channel Ambisonic layer needs %d files in audio_files, has %zu\n",
                                 l.name.c_str(), nch, nch, l.audioFiles.size());
                    return 1;
                }
                for (int c = 0; c < nch; ++c) {
                    const fs::path p = resolve(l.audioFiles[static_cast<size_t>(c)]);
                    tools::AudioFile f = tools::readWav(p.string());
                    std::vector<float> ch(f.frames(), 0.0f);
                    for (int fc = 0; fc < f.channels; ++fc)
                        for (size_t n = 0; n < ch.size(); ++n) ch[n] += f.data[static_cast<size_t>(fc)][n] / f.channels;
                    audio[firstInput[i] + static_cast<size_t>(c)] = resampleLinear(ch, f.sampleRate, rate);
                    if (c == 0) shown = p.filename().string() + " + " + std::to_string(nch - 1) + " more";
                }
            } else {
                if (l.audioFile.empty()) continue;
                const fs::path p = resolve(l.audioFile);
                tools::AudioFile f = tools::readWav(p.string());
                if (isAmbisonic(l) && f.channels < nch) {
                    std::fprintf(stderr, "layer %s: %d-channel Ambisonic layer, but %s has %d channels\n", l.name.c_str(), nch,
                                 p.filename().string().c_str(), f.channels);
                    return 1;
                }
                for (int c = 0; c < nch; ++c) {
                    std::vector<float> ch(f.frames(), 0.0f);
                    if (nch == 1) {
                        for (int fc = 0; fc < f.channels; ++fc)
                            for (size_t n = 0; n < ch.size(); ++n) ch[n] += f.data[static_cast<size_t>(fc)][n] / f.channels;
                    } else {
                        ch = f.data[static_cast<size_t>(std::min(c, f.channels - 1))];
                    }
                    audio[firstInput[i] + static_cast<size_t>(c)] = resampleLinear(ch, f.sampleRate, rate);
                }
                shown = p.filename().string();
            }
            const size_t end = static_cast<size_t>(l.startTime * rate) + audio[firstInput[i]].size();
            if (!l.loop) longest = std::max(longest, end);
            std::fprintf(stderr, "layer %-16s %s (%.1f s%s)\n", l.name.c_str(), shown.c_str(),
                         audio[firstInput[i]].size() / static_cast<double>(rate),
                         isAmbisonic(l) ? (", Ambisonic order " + std::to_string(ambisonicOrder(l.channels))).c_str() : nch == 2 ? ", stereo" : "");
        }
        if (duration <= 0) duration = scene.duration;
        if (duration <= 0) duration = longest / static_cast<double>(rate);
        if (duration <= 0) { std::fprintf(stderr, "nothing to render (no audio and no duration)\n"); return 1; }

        Renderer renderer(scene, cfg, duration);
        const auto st = renderer.stats();
        if (st.backend == ReflectionsBackend::SteamAudio)
            std::fprintf(stderr, "render: %s, %d outputs, %.1f s at %d Hz; Steam Audio reflections (%d triangles, IR %.2f s, %d rays x %d bounces every %.0f ms, block %d), delay line %.0f m\n",
                         modeStr.c_str(), renderer.numOutputs(), duration, rate, st.numTriangles, st.irSeconds,
                         cfg.steam.rays, st.bounces, cfg.steam.updateInterval * 1000, cfg.steam.frameSize, st.maxDistance);
        else if (st.irChannels > 0)
            std::fprintf(stderr, "render: %s, %d outputs, %.1f s at %d Hz; built-in reflections, %d images/layer, impulse-response reverb (%d ch, %.2f s, %s, lags %d samples), delay line %.0f m\n",
                         modeStr.c_str(), renderer.numOutputs(), duration, rate, st.numImagesPerLayer, st.irChannels, st.irSeconds,
                         st.irChannels == 1 ? "mono, diffuse" : st.irChannels == 2 ? "stereo L/R" : "ambiX, world-fixed", st.reflectionLatency,
                         st.maxDistance);
        else
            std::fprintf(stderr, "render: %s, %d outputs, %.1f s at %d Hz; built-in reflections, %d images/layer, RT60 mid %.2f s, delay line %.0f m\n",
                         modeStr.c_str(), renderer.numOutputs(), duration, rate, st.numImagesPerLayer, st.reverbRt60Mid,
                         st.maxDistance);
        if (!st.note.empty()) std::fprintf(stderr, "note: %s\n", st.note.c_str());

        const int latency = renderer.latencySamples();
        const size_t totalFrames = static_cast<size_t>(duration * rate);
        std::vector<std::vector<float>> out(renderer.numOutputs(), std::vector<float>(totalFrames + latency, 0.0f));
        std::vector<float> inBlock(audio.size() * block);
        std::vector<const float*> inPtrs(audio.size());
        std::vector<float*> outPtrs(renderer.numOutputs());

        const auto t0 = std::chrono::steady_clock::now();
        size_t pos = 0;
        while (pos < totalFrames + latency) {
            const int n = static_cast<int>(std::min<size_t>(block, totalFrames + latency - pos));
            for (size_t i = 0; i < scene.layers.size(); ++i) {
                const Layer& l = scene.layers[i];
                const int nch = layerInputs(l);
                const long start = static_cast<long>(l.startTime * rate);
                for (int c = 0; c < nch; ++c) {
                    const size_t in = firstInput[i] + static_cast<size_t>(c);
                    float* dst = inBlock.data() + in * block;
                    const auto& a = audio[in];
                    for (int k = 0; k < n; ++k) {
                        long idx = static_cast<long>(pos + k) - start;
                        float v = 0;
                        if (!a.empty() && idx >= 0) {
                            if (l.loop) idx %= static_cast<long>(a.size());
                            if (idx < static_cast<long>(a.size())) v = a[idx];
                        }
                        dst[k] = v;
                    }
                    inPtrs[in] = dst;
                }
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
