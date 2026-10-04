#include "sp/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "dsp/AirAbsorption.h"
#include "dsp/Ambisonics.h"
#include "dsp/Convolver.h"
#include "dsp/DelayLine.h"
#include "dsp/Fdn.h"
#include "dsp/Fft.h"
#include "dsp/Filters.h"
#include "dsp/Hrtf.h"
#include "dsp/Panning.h"
#include "dsp/RoomAcoustics.h"
#ifdef SP_HAVE_STEAM_AUDIO
#include "SteamAudioBackend.h"
#endif

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define SP_HAVE_SSE_CSR 1
#endif

namespace sp {

using namespace dsp;

namespace {

constexpr int kMaxSubBlock = 256;

// Recursive filters decaying into silence produce denormal floats, which are
// catastrophically slow on x86 (and still slow elsewhere). Flush them to zero
// for the duration of a process() call and restore the caller's mode after.
class DenormalGuard {
public:
    DenormalGuard() {
#if defined(SP_HAVE_SSE_CSR)
        csr_ = _mm_getcsr();
        _mm_setcsr(csr_ | 0x8040);  // FTZ | DAZ
#elif defined(__aarch64__)
        asm volatile("mrs %0, fpcr" : "=r"(fpcr_));
        asm volatile("msr fpcr, %0" : : "r"(fpcr_ | (1ull << 24)));  // FZ
#endif
    }
    ~DenormalGuard() {
#if defined(SP_HAVE_SSE_CSR)
        _mm_setcsr(csr_);
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(fpcr_));
#endif
    }
private:
#if defined(SP_HAVE_SSE_CSR)
    unsigned int csr_ = 0;
#elif defined(__aarch64__)
    unsigned long long fpcr_ = 0;
#endif
};

// One propagation path (direct or image) from a layer to the listener.
struct Tap {
    ImageSource image;
    bool isDirect = false;
    float delayCur = 0, delayTarget = 0;   // samples
    float gainCur = 0, gainTarget = 0;     // linear, excluding wall colour
    float distRef = 0;                     // distance when the path started (Doppler amount < 1 blends towards it)
    float distance = 0;
    AirFilter air;      // direct path: accurate ISO 9613-1 fit
    ShelfPair colour;   // images: wall reflectance x air absorption, 3 bands
    bool hasWall = false;
    std::array<float, kMaxAmbiChannels> shCur{}, shTarget{};
};

struct Voice {
    const Layer* layer = nullptr;   // points into Impl::scene.layers
    Vec3 position;                  // layer position, slewed towards layer->position after live edits
    LayerControls controls;
    DelayLine delay;
    std::vector<Tap> taps;  // taps[0] is the direct path
    Vec3 imagesForPosition{1e9f, 1e9f, 1e9f};

    float levelCur = 0, levelTarget = 0;
    float sendCur = 0, sendTarget = 0;
    float steamFeedCur = 0, steamFeedTarget = 0;  // dry feed into the ray-traced reflections
    ShelfPair steamColour;  // level calibration of the traced reflections, 3 bands

    // Binaural direct path
    SpectralInput directIn;
    PartitionedFilter hrtfL[2], hrtfR[2];  // [0] = current, [1] = previous
    bool crossfade = false;
    Vec3 lastDirL{0, 0, 0}, lastDirR{0, 0, 0};
    float lastEarGainL = -1, lastEarGainR = -1, lastSpread = -1;
    int hrtfAge = 0;  // sub-blocks since the HRTF filters were last updated

    // Loudspeaker direct path
    std::vector<float> spkCur, spkTarget;
};

// Longest path (direct or image) between any layer and the listener over the
// timeline, plus headroom: the delay lines are sized from it.
float estimateMaxDistance(const Scene& scene, const PoseEvaluator& poses, double duration,
                          const ListenerControls& controls = {}) {
    const double dur = duration > 0 ? duration : 600.0;
    const int steps = static_cast<int>(std::min(dur / 0.25, 4000.0)) + 1;
    float maxD = 1.0f;
    const int ord = scene.room.type == RoomType::Box ? std::max(scene.room.reflectionOrder, 0) : 1;
    std::vector<std::vector<ImageSource>> images;
    for (const auto& l : scene.layers) {
        const int lo = l.reflectionOrder >= 0 ? l.reflectionOrder : ord;
        images.push_back(computeImages(scene.room, l.position, lo));
    }
    for (int i = 0; i < steps; ++i) {
        const Pose p = poses.evaluate(dur * i / std::max(steps - 1, 1), controls);
        for (const auto& imgs : images)
            for (const auto& img : imgs) maxD = std::max(maxD, (img.position - p.position).length());
    }
    return std::min(maxD * 1.15f + 2.0f, 3000.0f);
}

bool sameMaterial(const Material& a, const Material& b) {
    return a.name == b.name && a.absorption == b.absorption && a.scattering == b.scattering && a.transmission == b.transmission;
}

bool sameMesh(const MeshGeometry& a, const MeshGeometry& b) {
    if (a.vertices != b.vertices || a.triangles != b.triangles || a.materialIndices != b.materialIndices ||
        a.materials.size() != b.materials.size())
        return false;
    for (size_t i = 0; i < a.materials.size(); ++i)
        if (!sameMaterial(a.materials[i], b.materials[i])) return false;
    return true;
}

bool sameObject(const SceneObject& a, const SceneObject& b) {
    return a.name == b.name && a.minCorner == b.minCorner && a.maxCorner == b.maxCorner && sameMaterial(a.material, b.material);
}

bool sameRoom(const Room& a, const Room& b) {
    // Geometry feeds the ray tracer's scene, which is built once per Renderer.
    if (a.meshFile != b.meshFile || !sameMesh(a.mesh, b.mesh) || a.objects.size() != b.objects.size()) return false;
    for (size_t i = 0; i < a.objects.size(); ++i)
        if (!sameObject(a.objects[i], b.objects[i])) return false;
    if (a.type != b.type || !(a.size == b.size) || !(a.origin == b.origin) || a.reflectionOrder != b.reflectionOrder ||
        a.reflectionsLevelDb != b.reflectionsLevelDb || a.reverbLevelDb != b.reverbLevelDb ||
        a.reverbTimeScale != b.reverbTimeScale || a.reflectionsEnabled != b.reflectionsEnabled ||
        a.reverbEnabled != b.reverbEnabled)
        return false;
    for (int w = 0; w < kNumWalls; ++w)
        if (!sameMaterial(a.materials[w], b.materials[w])) return false;
    return true;
}

bool sameEnvironment(const Environment& a, const Environment& b) {
    return a.speedOfSound == b.speedOfSound && a.temperatureC == b.temperatureC &&
           a.relativeHumidity == b.relativeHumidity && a.pressureKPa == b.pressureKPa && a.airAbsorption == b.airAbsorption;
}

}  // namespace

struct Renderer::Impl {
    Scene scene;
    RenderConfig cfg;
    double duration = 0;
    PoseEvaluator poses;
    ListenerControls listenerControls;
    Pose lastPose;

    int B = 32;               // sub-block
    float fs = 48000;
    int nOut = 2;
    int nSh = 16;
    int order = 3;
    float speedOfSound = 343;
    float maxDistance = 100;
    bool first = true;

    // Shared DSP resources
    std::unique_ptr<RealFft> fft;
    ConvolverSpec convSpec;
    HrtfSet hrtf;
    AirAbsorptionTable air;
    float airDbPerMetre[3] = {0, 0, 0};  // at 250 Hz, 1 kHz, 4 kHz (band centres of the 3-band shaper)
    struct AirBandGain {
        const float* dbPerMetre;
        float operator()(float dist) const { return std::pow(10.0f, -(*dbPerMetre) * dist / 20.0f); }
    };
    AirBandGain airBandGain[3] = {{&airDbPerMetre[0]}, {&airDbPerMetre[1]}, {&airDbPerMetre[2]}};
    VbapPanner vbap;
    AmbiSpeakerDecoder ambiDecoder;
    AmbiBinauralFilters ambiBinFilters;
    std::vector<PartitionedFilter> ambiBinPart;  // [ear * nSh + c]
    std::vector<SpectralInput> busIn;            // per SH channel
    Fdn fdn;
    bool fdnEnabled = false;
    RoomStats roomStats;
    float reverbTotalEnergy = 0;  // 16 pi / R (mid band), relative to a 1 m direct path
    float reflGain = 1, reverbTrim = 1;
    std::vector<SimpleDelay> speakerDelays;
    std::vector<float> speakerDelaySamples, speakerGains;
    bool speakerCompensation = false;

    std::vector<Voice> voices;

    // Ray-traced back-end (Steam Audio). When active, the image-source taps
    // and the FDN are off; it supplies the reflected field and occlusion.
    ReflectionsBackend backend = ReflectionsBackend::Builtin;
    bool useSteam = false;
    std::string note;
    float steamIrSeconds = 0;
#ifdef SP_HAVE_STEAM_AUDIO
    std::unique_ptr<SteamAudioBackend> steam;
#endif
    std::vector<float> steamIn;          // B
    std::vector<float> steamAmbi;        // nSh * B
    std::vector<float*> steamAmbiPtrs;
    double lastReflectionSim = -1e9;
    int directSimCountdown = 0;

    // Work buffers (sized at init)
    std::vector<float> bus;        // nSh * B
    std::vector<float> directBuf;  // B
    std::vector<float> revIn;      // B
    std::vector<float> earBuf[2];  // B each
    std::vector<float> tmpBlock;   // B
    std::vector<float> tapBuf;     // B
    std::vector<cfloat> acc, acc2;
    std::vector<float> irL, irR, irTmpL, irTmpR;
    std::vector<float> outBlock;   // nOut * B

    // FIFO for arbitrary host block sizes
    std::vector<float> inFifo;     // nLayers * B
    std::vector<float> outFifo;    // nOut * B
    int fifoFill = 0;
    double subBlockStartTime = 0;

    Impl(const Scene& s, const RenderConfig& c, double dur);
    void initVoices();
    void initRoom();
    void initBackend();
    float estimateIrSeconds() const;
    float meanFreePath() const;
    void computeTargets(Voice& v, const Pose& pose, double time);
    void rebuildTaps(Voice& v, const Vec3& srcPos);
    void updateHrtf(Voice& v, const Vec3& rel, float dist);
    void renderSubBlock(double time);
    void resetState();
};

Renderer::Impl::Impl(const Scene& s, const RenderConfig& c, double dur) : scene(s), cfg(c), duration(dur) {
    fs = static_cast<float>(cfg.sampleRate);
    B = clamp(cfg.subBlockSize, 8, kMaxSubBlock);
    order = clamp(cfg.ambisonicsOrder, 1, kMaxAmbiOrder);
    nSh = ambiChannels(order);
    speedOfSound = std::max(scene.environment.speedOfSound, 50.0f);
    poses = PoseEvaluator(scene, duration > 0 ? duration : 600.0);

    switch (cfg.mode) {
        case OutputMode::Binaural: nOut = 2; break;
        case OutputMode::Speakers: nOut = cfg.layout.numChannels(); break;
        case OutputMode::Ambisonics: nOut = nSh; break;
    }
    if (nOut <= 0) throw std::runtime_error("Output layout has no channels");

    fft = std::make_unique<RealFft>(2 * B);

    if (cfg.mode == OutputMode::Binaural) {
        if (cfg.hrtfPath.empty()) throw std::runtime_error("Binaural output needs an HRTF (SOFA) file");
        hrtf.load(cfg.hrtfPath, fs);
        convSpec.blockSize = B;
        convSpec.partitions = std::max(1, (hrtf.filterLength() + B - 1) / B);
        // Ambisonics bus -> binaural filters, same length as the HRIRs.
        ambiBinFilters = designAmbiBinauralFilters(hrtf, order, convSpec.filterLength());
        ambiBinPart.resize(static_cast<size_t>(2) * nSh);
        for (int ear = 0; ear < 2; ++ear)
            for (int ch = 0; ch < nSh; ++ch) {
                auto& p = ambiBinPart[static_cast<size_t>(ear) * nSh + ch];
                p.reset(convSpec, *fft);
                p.set(ambiBinFilters.filter(ear, ch), ambiBinFilters.length, *fft);
            }
        busIn.resize(nSh);
        for (auto& b : busIn) b.reset(convSpec, *fft);
        irL.assign(hrtf.filterLength(), 0.0f);
        irR.assign(hrtf.filterLength(), 0.0f);
        irTmpL.assign(hrtf.filterLength(), 0.0f);
        irTmpR.assign(hrtf.filterLength(), 0.0f);
        acc.assign(convSpec.bins(), cfloat{});
        acc2.assign(convSpec.bins(), cfloat{});
    } else if (cfg.mode == OutputMode::Speakers) {
        vbap.init(cfg.layout, cfg.vbapResolutionDeg, 0.0f);
        ambiDecoder.init(cfg.layout, order);
        // Distance compensation when the layout declares distances.
        float maxD = 0;
        for (const auto& spk : cfg.layout.speakers) maxD = std::max(maxD, spk.distance);
        speakerCompensation = maxD > 0;
        if (speakerCompensation) {
            speakerDelays.resize(nOut);
            speakerDelaySamples.resize(nOut);
            speakerGains.resize(nOut);
            for (int i = 0; i < nOut; ++i) {
                const float d = cfg.layout.speakers[i].distance > 0 ? cfg.layout.speakers[i].distance : maxD;
                speakerDelaySamples[i] = (maxD - d) / speedOfSound * fs;
                speakerGains[i] = d / maxD;
                speakerDelays[i].init(static_cast<int>(speakerDelaySamples[i]) + 2);
            }
        }
    }

    maxDistance = cfg.maxDistance > 0 ? cfg.maxDistance : estimateMaxDistance(scene, poses, duration, listenerControls);
    air = AirAbsorptionTable(scene.environment, fs, maxDistance + 5.0f, 1.0f);
    if (scene.environment.airAbsorption) {
        const float f[3] = {250.0f, 1000.0f, 4000.0f};
        for (int b = 0; b < 3; ++b)
            airDbPerMetre[b] = isoAirAttenuationDbPerMetre(f[b], scene.environment.temperatureC,
                                                            scene.environment.relativeHumidity, scene.environment.pressureKPa);
    }
    initRoom();
    initBackend();
    initVoices();

    bus.assign(static_cast<size_t>(nSh) * B, 0.0f);
    steamIn.assign(B, 0.0f);
    steamAmbi.assign(static_cast<size_t>(nSh) * B, 0.0f);
    steamAmbiPtrs.resize(nSh);
    for (int c = 0; c < nSh; ++c) steamAmbiPtrs[c] = steamAmbi.data() + static_cast<size_t>(c) * B;
    directBuf.assign(B, 0.0f);
    revIn.assign(B, 0.0f);
    earBuf[0].assign(B, 0.0f);
    earBuf[1].assign(B, 0.0f);
    tmpBlock.assign(B, 0.0f);
    tapBuf.assign(B, 0.0f);
    outBlock.assign(static_cast<size_t>(nOut) * B, 0.0f);
    inFifo.assign(static_cast<size_t>(std::max<size_t>(1, scene.layers.size())) * B, 0.0f);
    outFifo.assign(static_cast<size_t>(nOut) * B, 0.0f);
    fifoFill = 0;
    first = true;
}

void Renderer::Impl::initRoom() {
    const Room& room = scene.room;
    reflGain = dbToGain(room.reflectionsLevelDb);
    reverbTrim = dbToGain(room.reverbLevelDb);
    fdnEnabled = room.type == RoomType::Box && room.reverbEnabled;
    if (room.type == RoomType::Box) {
        roomStats = computeRoomStats(room, scene.environment);
        const float Rmid = std::max(bandAverage(roomStats.roomConstant, 2, 3), 1.0f);
        // Hopkins-Stryker: reverberant energy density 4/R vs direct 1/(4 pi r^2).
        reverbTotalEnergy = 16.0f * kPi / Rmid;
        if (fdnEnabled) {
            FdnParams fp;
            fp.sampleRate = fs;
            fp.rt60Low = bandAverage(roomStats.rt60, 0, 1);
            fp.rt60Mid = bandAverage(roomStats.rt60, 2, 3);
            fp.rt60High = bandAverage(roomStats.rt60, 4, 5);
            fp.meanFreePathSeconds = roomStats.meanFreePath / speedOfSound;
            fp.preDelaySeconds = roomStats.meanFreePath * (room.reflectionOrder + 0.5f) / speedOfSound;
            fp.ambiOrder = order;
            fdn.init(fp);
        }
    }
}

float Renderer::Impl::estimateIrSeconds() const {
    const Room& room = scene.room;
    float rt60 = 1.0f;
    if (room.type == RoomType::Box) {
        rt60 = bandAverage(roomStats.rt60, 2, 3);
    } else if (room.type == RoomType::Mesh && !room.mesh.empty()) {
        // Eyring on the mesh: bounding-box volume, real surface area,
        // area-weighted mid-band absorption.
        const MeshGeometry& g = room.mesh;
        const Vec3 ext = g.maxCorner() - g.minCorner();
        const float V = std::max(ext.x * ext.y * ext.z, 1.0f);
        float S = 0, Sa = 0;
        for (size_t t = 0; t < g.triangles.size(); ++t) {
            const Vec3& p0 = g.vertices[g.triangles[t][0]];
            const float a = 0.5f * (g.vertices[g.triangles[t][1]] - p0).cross(g.vertices[g.triangles[t][2]] - p0).length();
            const int m = t < g.materialIndices.size() ? g.materialIndices[t] : 0;
            const float alpha = m >= 0 && m < static_cast<int>(g.materials.size()) ? bandAverage(g.materials[m].absorption, 2, 3) : 0.1f;
            S += a;
            Sa += a * alpha;
        }
        const float alpha = clamp(S > 0 ? Sa / S : 0.1f, 0.01f, 0.99f);
        rt60 = 0.161f * V / (-S * std::log(1.0f - alpha));
    } else {
        rt60 = 0.6f;  // outdoors / free field with objects: short, sparse echoes
    }
    rt60 *= std::max(room.reverbTimeScale, 0.05f);
    // The IR needs to hold the tail down to roughly -60 dB plus the first arrivals.
    return clamp(rt60 * 1.1f + 0.15f, 0.3f, 6.0f);
}

float Renderer::Impl::meanFreePath() const {
    const Room& room = scene.room;
    if (room.type == RoomType::Box) return std::max(roomStats.meanFreePath, 0.5f);
    if (room.type == RoomType::Mesh && !room.mesh.empty()) {
        const Vec3 ext = room.mesh.maxCorner() - room.mesh.minCorner();
        const float V = std::max(ext.x * ext.y * ext.z, 1.0f);
        return std::max(4.0f * V / std::max(room.mesh.surfaceArea(), 1.0f), 0.5f);
    }
    return 20.0f;  // outdoors / objects only: sparse, long paths
}

void Renderer::Impl::initBackend() {
    const Room& room = scene.room;
    const bool needsRays = room.type == RoomType::Mesh || !room.objects.empty();
    const bool available = Renderer::steamAudioAvailable();
    useSteam = false;
    switch (cfg.reflections) {
        case ReflectionsBackend::Auto:
            useSteam = available && needsRays;
            if (needsRays && !available) note = "scene has mesh geometry or objects but the engine was built without Steam Audio; rendering without them";
            break;
        case ReflectionsBackend::Builtin:
            if (needsRays) note = "built-in reflections: mesh geometry and objects are ignored";
            break;
        case ReflectionsBackend::SteamAudio:
            useSteam = available;
            if (!available) note = "Steam Audio back-end requested but not built in (SP_WITH_STEAM_AUDIO); using built-in reflections";
            break;
    }
    if (useSteam && room.type == RoomType::Mesh && room.mesh.empty()) {
        note = "mesh room without geometry; rendering free field";
        useSteam = false;
    }
#ifdef SP_HAVE_STEAM_AUDIO
    if (useSteam) {
        try {
            SteamBackendSettings st;
            st.sampleRate = fs;
            st.subBlock = B;
            st.ambiOrder = order;
            st.numSources = static_cast<int>(scene.layers.size());
            st.steam = cfg.steam;
            st.irSeconds = cfg.steam.irSeconds > 0 ? cfg.steam.irSeconds : estimateIrSeconds();
            st.speedOfSound = speedOfSound;
            st.meanFreePath = meanFreePath();
            st.airAbsorption = scene.environment.airAbsorption;
            steam = std::make_unique<SteamAudioBackend>(roomGeometry(room), st);
            steamIrSeconds = steam->irSeconds();
        } catch (const std::exception& e) {
            note = std::string("Steam Audio back-end failed to initialise: ") + e.what() + "; using built-in reflections";
            steam.reset();
            useSteam = false;
        }
    }
#endif
    backend = useSteam ? ReflectionsBackend::SteamAudio : ReflectionsBackend::Builtin;
    if (useSteam) fdnEnabled = false;  // the traced IR carries the whole tail
}

void Renderer::Impl::initVoices() {
    voices.clear();
    voices.resize(scene.layers.size());
    const int maxDelay = static_cast<int>(maxDistance / speedOfSound * fs) + 16;
    for (size_t i = 0; i < voices.size(); ++i) {
        Voice& v = voices[i];
        v.layer = &scene.layers[i];
        v.position = v.layer->position;
        v.delay.init(maxDelay);
        rebuildTaps(v, v.position);
        // Steam Audio's reconstructed IRs come out below the diffuse-field
        // level a box room should have, by a band-dependent amount (measured
        // against statistical theory in two rooms; see docs/engine.md).
        v.steamColour.set(dbToGain(2.5f), dbToGain(7.7f), dbToGain(4.3f), fs, 800.0f, 8000.0f);
        if (cfg.mode == OutputMode::Binaural) {
            v.directIn.reset(convSpec, *fft);
            for (int k = 0; k < 2; ++k) {
                v.hrtfL[k].reset(convSpec, *fft);
                v.hrtfR[k].reset(convSpec, *fft);
            }
        } else if (cfg.mode == OutputMode::Speakers) {
            v.spkCur.assign(nOut, 0.0f);
            v.spkTarget.assign(nOut, 0.0f);
        }
    }
}

void Renderer::Impl::updateHrtf(Voice& v, const Vec3& rel, float dist) {
    // Per-ear directions (parallax) and near-field level: the ears are at
    // +/- headRadius on the head's X axis.
    const float r = scene.listener.headRadius;
    Vec3 dirL, dirR;
    float gL = 1, gR = 1;
    if (cfg.nearField && dist < cfg.nearFieldRadius) {
        const Vec3 earL{-r, 0, 0}, earR{r, 0, 0};
        const Vec3 toL = rel - earL, toR = rel - earR;
        const float dL = std::max(toL.length(), 0.01f), dR = std::max(toR.length(), 0.01f);
        dirL = toL / dL;
        dirR = toR / dR;
        // Blend the parallax/level effect in smoothly below the radius.
        const float w = clamp((cfg.nearFieldRadius - dist) / (0.5f * cfg.nearFieldRadius), 0.0f, 1.0f);
        const float d0 = std::max(dist, 0.01f);
        gL = lerp(1.0f, clamp(d0 / dL, 0.25f, 4.0f), w);
        gR = lerp(1.0f, clamp(d0 / dR, 0.25f, 4.0f), w);
        const Vec3 c = rel / d0;
        dirL = (dirL * w + c * (1 - w)).normalized();
        dirR = (dirR * w + c * (1 - w)).normalized();
    } else {
        dirL = dirR = rel.normalized();
    }
    const float spread = v.controls.spreadDeg.value_or(v.layer->spreadDeg);
    // Rate limit: a new pair of HRIRs costs 16 FFTs; direction changes within
    // a couple of milliseconds are far below what the ear resolves.
    v.crossfade = false;
    if (!first && ++v.hrtfAge < std::max(1, cfg.hrtfUpdateInterval)) return;
    const float thresholdSq = 7e-6f;  // ~0.15 degrees
    const bool changed = (dirL - v.lastDirL).lengthSquared() > thresholdSq ||
                         (dirR - v.lastDirR).lengthSquared() > thresholdSq ||
                         std::fabs(gL - v.lastEarGainL) > 2e-3f || std::fabs(gR - v.lastEarGainR) > 2e-3f ||
                         spread != v.lastSpread;
    if (!changed && !first) return;
    v.hrtfAge = 0;
    v.lastDirL = dirL; v.lastDirR = dirR; v.lastEarGainL = gL; v.lastEarGainR = gR; v.lastSpread = spread;

    const int n = hrtf.filterLength();
    if ((dirL - dirR).lengthSquared() < 1e-10f) {
        hrtf.getFilterSpread(dirL, spread, irL.data(), irR.data());
    } else {
        hrtf.getFilterSpread(dirL, spread, irL.data(), irTmpR.data());
        hrtf.getFilterSpread(dirR, spread, irTmpL.data(), irR.data());
    }
    for (int i = 0; i < n; ++i) { irL[i] *= gL; irR[i] *= gR; }
    std::swap(v.hrtfL[0], v.hrtfL[1]);
    std::swap(v.hrtfR[0], v.hrtfR[1]);
    v.hrtfL[0].set(irL.data(), n, *fft);
    v.hrtfR[0].set(irR.data(), n, *fft);
    v.crossfade = !first;
}

void Renderer::Impl::rebuildTaps(Voice& v, const Vec3& srcPos) {
    const Layer& L = *v.layer;
    const Room& room = scene.room;
    int ord = L.reflectionOrder >= 0 ? L.reflectionOrder : room.reflectionOrder;
    if (!room.reflectionsEnabled || useSteam) ord = 0;
    const auto images = computeImages(room, srcPos, ord);
    if (images.size() != v.taps.size()) v.taps.assign(images.size(), Tap{});
    for (size_t i = 0; i < images.size(); ++i) {
        Tap& t = v.taps[i];
        t.image = images[i];
        t.isDirect = (i == 0);
        t.hasWall = !t.isDirect;
    }
    v.imagesForPosition = srcPos;
}

void Renderer::Impl::computeTargets(Voice& v, const Pose& pose, double /*time*/) {
    const Layer& L = *v.layer;
    // A layer moved by a live scene update glides to its new position (time
    // constant 40 ms) instead of jumping, so dragging it in the editor does
    // not produce a burst of Doppler shift. Static scenes never move.
    if (first) {
        v.position = L.position;
    } else if ((L.position - v.position).lengthSquared() > 1e-8f) {
        const float a = 1.0f - std::exp(-static_cast<float>(B) / (0.04f * fs));
        v.position = v.position + (L.position - v.position) * a;
    } else {
        v.position = L.position;
    }
    const Vec3 srcPos = v.position + v.controls.positionOffset;

    // (Re)build the image set when the source moved.
    if ((srcPos - v.imagesForPosition).lengthSquared() > 1e-10f) rebuildTaps(v, srcPos);

    const bool muted = L.mute || v.controls.mute;
    v.levelTarget = muted ? 0.0f : dbToGain(L.levelDb + v.controls.levelOffsetDb);
    const float dopplerAmount = clamp(v.controls.dopplerAmount.value_or(L.dopplerAmount), 0.0f, 1.0f);
    const float refGain = std::pow(std::max(L.referenceDistance, 0.01f), L.rolloff);

    float imageEnergy = 0;
    for (size_t i = 0; i < v.taps.size(); ++i) {
        Tap& t = v.taps[i];
        const Vec3 relWorld = t.image.position - pose.position;
        const float dist = std::max(relWorld.length(), 0.001f);
        const Vec3 rel = pose.orientation.inverseRotate(relWorld);
        t.distance = dist;

        // Doppler comes from the delay changing with distance. A Doppler amount
        // below 1 scales that change by holding part of the delay at the
        // distance the path started with, so the pitch shift scales too.
        if (first) t.distRef = dist;
        const float dDelay = dopplerAmount * dist + (1 - dopplerAmount) * t.distRef;
        t.delayTarget = dDelay / speedOfSound * fs;

        // Distance gain (clamped inside minDistance) and directivity.
        float g = refGain / std::pow(std::max(dist, L.minDistance), L.rolloff);
        if (L.directivity > 0) {
            const Vec3 listenerImage = t.isDirect ? pose.position : mirrorPoint(scene.room, pose.position, t.image.index);
            const Vec3 away = (listenerImage - srcPos).normalized();
            const float cosTheta = L.directivityForward.normalized().dot(away);
            const float k = clamp(L.directivity, 0.0f, 1.0f);
            g *= (1.0f - 0.5f * k) + 0.5f * k * cosTheta;
        }
        if (!t.isDirect) {
            g *= reflGain;
            const float midRefl = bandAverage(t.image.reflectance, 2, 3);
            imageEnergy += (g * midRefl) * (g * midRefl);
        }
        t.gainTarget = g;
        if (t.isDirect) {
            air.apply(dist, t.air);
        } else {
            // Reflections: wall reflectance and air absorption folded into one
            // 3-band shaper (cheaper than the direct path's 4-pole air fit).
            const auto& rf = t.image.reflectance;
            t.colour.set(bandAverage(rf, 0, 1) * airBandGain[0] (dist),
                         bandAverage(rf, 2, 3) * airBandGain[1] (dist),
                         bandAverage(rf, 4, 5) * airBandGain[2] (dist), fs);
        }

        const bool needsSh = !t.isDirect || cfg.mode == OutputMode::Ambisonics;
        if (needsSh) encodeDirection(rel, order, t.shTarget.data());

        if (t.isDirect) {
            if (cfg.mode == OutputMode::Binaural) updateHrtf(v, rel, dist);
            else if (cfg.mode == OutputMode::Speakers) vbap.gains(rel, v.spkTarget.data());
        }
    }

    // Late reverb send: what the diffuse field should carry beyond the
    // modelled early reflections.
    if (fdnEnabled && !muted) {
        const float totalE = reverbTotalEnergy * refGain * refGain;
        const float lateE = std::max(totalE - imageEnergy, 0.15f * totalE);
        v.sendTarget = std::sqrt(lateE) * dbToGain(L.reverbSendDb) * reverbTrim;
    } else {
        v.sendTarget = 0;
    }

    // Ray-traced reflections: the traced IR is relative to a unit source at
    // 1 m, so the feed carries the reference-distance gain and the trims.
    v.steamFeedTarget = (useSteam && scene.room.reflectionsEnabled && !muted)
        ? refGain * reflGain * reverbTrim * dbToGain(L.reverbSendDb) : 0.0f;
#ifdef SP_HAVE_STEAM_AUDIO
    if (useSteam && steam) {
        steam->setSource(static_cast<int>(&v - voices.data()), srcPos, L.directivityForward,
                         0.5f * clamp(L.directivity, 0.0f, 1.0f), L.occlusion, L.occlusionRadius);
    }
#endif

    if (first) {
        v.levelCur = v.levelTarget;
        v.sendCur = v.sendTarget;
        v.steamFeedCur = v.steamFeedTarget;
        for (auto& t : v.taps) { t.delayCur = t.delayTarget; t.gainCur = t.gainTarget; t.shCur = t.shTarget; }
        if (!v.spkCur.empty()) v.spkCur = v.spkTarget;
    }
}

void Renderer::Impl::renderSubBlock(double time) {
    const Pose pose = poses.evaluate(time, listenerControls);
    lastPose = pose;
    const float invB = 1.0f / static_cast<float>(B);

    std::fill(bus.begin(), bus.end(), 0.0f);
    std::fill(revIn.begin(), revIn.end(), 0.0f);
    std::fill(outBlock.begin(), outBlock.end(), 0.0f);
    float* ear[2] = {earBuf[0].data(), earBuf[1].data()};
    std::fill(earBuf[0].begin(), earBuf[0].end(), 0.0f);
    std::fill(earBuf[1].begin(), earBuf[1].end(), 0.0f);

    for (auto& v : voices) computeTargets(v, pose, time);

#ifdef SP_HAVE_STEAM_AUDIO
    if (useSteam && steam) {
        steam->setListener(pose.position, pose.orientation.right(), pose.orientation.up(), pose.orientation.forward());
        if (--directSimCountdown <= 0 || first) {
            steam->simulateDirect();
            directSimCountdown = std::max(1, cfg.hrtfUpdateInterval);
        }
        if (first || time - lastReflectionSim >= cfg.steam.updateInterval) {
            steam->simulateReflections();
            lastReflectionSim = time;
        }
    }
#endif

    for (size_t li = 0; li < voices.size(); ++li) {
        Voice& v = voices[li];
        const float* in = inFifo.data() + li * B;

        // Skip silent, inactive layers entirely once their tails have gone.
        const float dLevel = (v.levelTarget - v.levelCur) * invB;
        const float dSend = (v.sendTarget - v.sendCur) * invB;
        const float dFeed = (v.steamFeedTarget - v.steamFeedCur) * invB;
        float level = v.levelCur, send = v.sendCur, feed = v.steamFeedCur;

        std::fill(directBuf.begin(), directBuf.end(), 0.0f);

        // Feed the delay line, the reverb send and the ray-traced reflections.
        for (int i = 0; i < B; ++i) {
            level += dLevel;
            send += dSend;
            feed += dFeed;
            const float x = in[i] * level;
            v.delay.write(x);
            revIn[i] += x * send;
            steamIn[i] = feed;  // gain ramp; the signal is taken at the direct tap below
        }
        // The delay line now holds this block; reading `delay + (B - 1 - i)`
        // behind the newest sample is the same as reading `delay` behind
        // sample i, so each tap can run as its own contiguous pass.
        for (auto& t : v.taps) {
            float* tb = t.isDirect ? directBuf.data() : tapBuf.data();
            const float dStep = (t.delayTarget - t.delayCur) * invB;
            const float gStep = (t.gainTarget - t.gainCur) * invB;
            float d = t.delayCur, g = t.gainCur;
            // The traced reflection IR is relative to the direct arrival, so
            // its feed is the signal at the direct tap: propagation delay and
            // Doppler included, distance gain and air absorption not.
            const bool feedsSteam = t.isDirect && useSteam;
            for (int i = 0; i < B; ++i) {
                d += dStep;
                g += gStep;
                const float raw = v.delay.read(d + static_cast<float>(B - 1 - i));
                if (feedsSteam) steamIn[i] *= v.steamColour.process(raw);
                const float smp = raw * g;
                tb[i] = t.isDirect ? t.air.process(smp) : t.colour.process(smp);
            }
#ifdef SP_HAVE_STEAM_AUDIO
            if (feedsSteam && steam) steam->applyOcclusion(static_cast<int>(li), tb);
#endif
            if (!t.isDirect || cfg.mode == OutputMode::Ambisonics) {
                for (int c = 0; c < nSh; ++c) {
                    const float sh0 = t.shCur[c], shStep = (t.shTarget[c] - sh0) * invB;
                    float* b = bus.data() + static_cast<size_t>(c) * B;
                    float w = sh0;
                    for (int i = 0; i < B; ++i) {
                        w += shStep;
                        b[i] += tb[i] * w;
                    }
                }
            }
        }
        v.levelCur = v.levelTarget;
        v.sendCur = v.sendTarget;
        v.steamFeedCur = v.steamFeedTarget;
        for (auto& t : v.taps) { t.delayCur = t.delayTarget; t.gainCur = t.gainTarget; t.shCur = t.shTarget; }
#ifdef SP_HAVE_STEAM_AUDIO
        if (useSteam && steam) steam->pushDry(static_cast<int>(li), steamIn.data());
#endif

        // Direct path output.
        if (cfg.mode == OutputMode::Binaural) {
            v.directIn.push(directBuf.data(), *fft);
            for (int e = 0; e < 2; ++e) {
                PartitionedFilter* cur = e == 0 ? &v.hrtfL[0] : &v.hrtfR[0];
                PartitionedFilter* prev = e == 0 ? &v.hrtfL[1] : &v.hrtfR[1];
                std::fill(acc.begin(), acc.end(), cfloat{});
                convolveAccumulate(v.directIn, *cur, convSpec.partitions, acc.data());
                spectrumToBlock(acc.data(), *fft, B, tmpBlock.data());
                if (v.crossfade) {
                    std::fill(acc2.begin(), acc2.end(), cfloat{});
                    convolveAccumulate(v.directIn, *prev, convSpec.partitions, acc2.data());
                    float* old = directBuf.data();  // reuse as scratch; direct input already consumed
                    spectrumToBlock(acc2.data(), *fft, B, old);
                    for (int i = 0; i < B; ++i) {
                        const float w = static_cast<float>(i + 1) * invB;
                        ear[e][i] += old[i] * (1 - w) + tmpBlock[i] * w;
                    }
                } else {
                    for (int i = 0; i < B; ++i) ear[e][i] += tmpBlock[i];
                }
            }
        } else if (cfg.mode == OutputMode::Speakers) {
            for (int ch = 0; ch < nOut; ++ch) {
                const float g0 = v.spkCur[ch], g1 = v.spkTarget[ch];
                if (g0 == 0 && g1 == 0) continue;
                float* o = outBlock.data() + static_cast<size_t>(ch) * B;
                for (int i = 0; i < B; ++i) o[i] += directBuf[i] * (g0 + (g1 - g0) * static_cast<float>(i + 1) * invB);
            }
            v.spkCur = v.spkTarget;
        }
    }

    // Ray-traced reflections (early and late) into the bus.
#ifdef SP_HAVE_STEAM_AUDIO
    if (useSteam && steam) {
        steam->endSubBlock();
        if (steam->pullReflections(steamAmbiPtrs.data()))
            for (size_t k = 0; k < bus.size(); ++k) bus[k] += steamAmbi[k];
    }
#endif

    // Late reverb into the bus.
    if (fdnEnabled) {
        float amb[kMaxAmbiChannels];
        for (int i = 0; i < B; ++i) {
            std::fill(amb, amb + nSh, 0.0f);
            fdn.process(revIn[i], 1.0f, amb);
            for (int c = 0; c < nSh; ++c) bus[static_cast<size_t>(c) * B + i] += amb[c];
        }
    }

    // Bus decode.
    if (cfg.mode == OutputMode::Binaural) {
        for (int c = 0; c < nSh; ++c) busIn[c].push(bus.data() + static_cast<size_t>(c) * B, *fft);
        for (int e = 0; e < 2; ++e) {
            std::fill(acc.begin(), acc.end(), cfloat{});
            for (int c = 0; c < nSh; ++c)
                convolveAccumulate(busIn[c], ambiBinPart[static_cast<size_t>(e) * nSh + c], convSpec.partitions, acc.data());
            spectrumToBlock(acc.data(), *fft, B, tmpBlock.data());
            for (int i = 0; i < B; ++i) ear[e][i] += tmpBlock[i];
        }
        std::copy(ear[0], ear[0] + B, outBlock.begin());
        std::copy(ear[1], ear[1] + B, outBlock.begin() + B);
    } else if (cfg.mode == OutputMode::Speakers) {
        for (int ch = 0; ch < nOut; ++ch) {
            const float* row = ambiDecoder.row(ch);
            float* o = outBlock.data() + static_cast<size_t>(ch) * B;
            for (int c = 0; c < nSh; ++c) {
                const float w = row[c];
                if (w == 0) continue;
                const float* b = bus.data() + static_cast<size_t>(c) * B;
                for (int i = 0; i < B; ++i) o[i] += w * b[i];
            }
        }
        if (speakerCompensation) {
            for (int ch = 0; ch < nOut; ++ch) {
                float* o = outBlock.data() + static_cast<size_t>(ch) * B;
                for (int i = 0; i < B; ++i) o[i] = speakerDelays[ch].process(o[i], speakerDelaySamples[ch]) * speakerGains[ch];
            }
        }
    } else {
        for (int c = 0; c < nSh; ++c) {
            const float s = n3dToSn3d(c);
            const float* b = bus.data() + static_cast<size_t>(c) * B;
            float* o = outBlock.data() + static_cast<size_t>(c) * B;
            for (int i = 0; i < B; ++i) o[i] = b[i] * s;
        }
    }

    std::copy(outBlock.begin(), outBlock.end(), outFifo.begin());
    first = false;
}

void Renderer::Impl::resetState() {
    for (auto& v : voices) {
        v.delay.clear();
        for (auto& t : v.taps) { t.air.reset(); t.colour.reset(); }
        v.directIn.clear();
        v.crossfade = false;
    }
    for (auto& b : busIn) b.clear();
    if (fdnEnabled) fdn.reset();
#ifdef SP_HAVE_STEAM_AUDIO
    if (steam) steam->reset();
#endif
    lastReflectionSim = -1e9;
    directSimCountdown = 0;
    std::fill(inFifo.begin(), inFifo.end(), 0.0f);
    std::fill(outFifo.begin(), outFifo.end(), 0.0f);
    fifoFill = 0;
    first = true;
}

// ------------------------------------------------------------- Renderer

Renderer::Renderer(const Scene& scene, const RenderConfig& config, double duration)
    : impl_(std::make_unique<Impl>(scene, config, duration)) {}

Renderer::~Renderer() = default;

int Renderer::numInputs() const { return static_cast<int>(impl_->scene.layers.size()); }
int Renderer::numOutputs() const { return impl_->nOut; }
int Renderer::latencySamples() const { return impl_->B; }
const RenderConfig& Renderer::config() const { return impl_->cfg; }
const PoseEvaluator& Renderer::poseEvaluator() const { return impl_->poses; }
Pose Renderer::lastPose() const { return impl_->lastPose; }
const ListenerControls& Renderer::listenerControls() const { return impl_->listenerControls; }
void Renderer::setListenerControls(const ListenerControls& c) { impl_->listenerControls = c; }
void Renderer::reset() { impl_->resetState(); }

void Renderer::setLayerControls(int layer, const LayerControls& c) {
    if (layer >= 0 && layer < static_cast<int>(impl_->voices.size())) impl_->voices[layer].controls = c;
}

std::unique_ptr<SceneUpdate> Renderer::prepareUpdate(const Scene& scene) const {
    // Reads only members that applyUpdate() never touches (layer count, room,
    // environment, duration, maxDistance), so this may run concurrently with
    // process() and applyUpdate().
    const Impl& im = *impl_;
    if (scene.layers.size() != im.voices.size()) return nullptr;
    if (!sameRoom(scene.room, im.scene.room) || !sameEnvironment(scene.environment, im.scene.environment))
        return nullptr;
    auto u = std::make_unique<SceneUpdate>();
    u->scene = scene;
    u->poses = PoseEvaluator(scene, im.duration > 0 ? im.duration : 600.0);
    if (estimateMaxDistance(scene, u->poses, im.duration, im.listenerControls) > im.maxDistance) return nullptr;
    return u;
}

void Renderer::applyUpdate(SceneUpdate& u) {
    Impl& im = *impl_;
    if (u.scene.layers.size() != im.voices.size()) return;
    // Vector and object swaps exchange heap buffers without allocating.
    std::swap(im.scene.layers, u.scene.layers);
    std::swap(im.scene.listener, u.scene.listener);
    std::swap(im.scene.name, u.scene.name);
    std::swap(im.poses, u.poses);
    for (size_t i = 0; i < im.voices.size(); ++i) im.voices[i].layer = &im.scene.layers[i];
}

bool Renderer::steamAudioAvailable() {
#ifdef SP_HAVE_STEAM_AUDIO
    return true;
#else
    return false;
#endif
}

Renderer::Stats Renderer::stats() const {
    Stats s;
    s.backend = impl_->backend;
    s.numImagesPerLayer = impl_->voices.empty() ? 0 : static_cast<int>(impl_->voices[0].taps.size()) - 1;
    s.reverbRt60Mid = impl_->fdnEnabled ? bandAverage(impl_->roomStats.rt60, 2, 3) : 0.0f;
    s.reverbGain = impl_->fdnEnabled ? std::sqrt(impl_->reverbTotalEnergy) : 0.0f;
    s.maxDistance = impl_->maxDistance;
    s.irSeconds = impl_->steamIrSeconds;
#ifdef SP_HAVE_STEAM_AUDIO
    if (impl_->steam) {
        s.numTriangles = impl_->steam->numTriangles();
        s.bounces = impl_->steam->bounces();
        s.reflectionLatency = impl_->steam->reflectionLatency();
    }
#endif
    s.note = impl_->note;
    return s;
}

void Renderer::process(const float* const* inputs, float* const* outputs, int numFrames, double timeSeconds) {
    DenormalGuard denormals;
    Impl& im = *impl_;
    const int B = im.B;
    const size_t nLayers = im.voices.size();
    int done = 0;
    while (done < numFrames) {
        if (im.fifoFill == 0) im.subBlockStartTime = timeSeconds + static_cast<double>(done) / im.cfg.sampleRate;
        const int k = std::min(B - im.fifoFill, numFrames - done);
        // Output what the previous sub-block produced (constant latency of B).
        for (int ch = 0; ch < im.nOut; ++ch) {
            if (!outputs[ch]) continue;
            std::memcpy(outputs[ch] + done, im.outFifo.data() + static_cast<size_t>(ch) * B + im.fifoFill, sizeof(float) * k);
        }
        for (size_t l = 0; l < nLayers; ++l) {
            float* dst = im.inFifo.data() + l * B + im.fifoFill;
            if (inputs && inputs[l]) std::memcpy(dst, inputs[l] + done, sizeof(float) * k);
            else std::fill(dst, dst + k, 0.0f);
        }
        im.fifoFill += k;
        done += k;
        if (im.fifoFill == B) {
            im.renderSubBlock(im.subBlockStartTime);
            im.fifoFill = 0;
        }
    }
}

}  // namespace sp
