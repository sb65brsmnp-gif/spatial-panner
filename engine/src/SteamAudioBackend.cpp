#include "SteamAudioBackend.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <phonon.h>

namespace sp::dsp {

namespace {

IPLVector3 toIpl(const Vec3& v) { return IPLVector3{v.x, v.y, v.z}; }

// Our six octave bands (125 .. 4k) to Steam Audio's low / mid / high.
void toBands3(const std::array<float, kNumBands>& a, float out[3]) {
    out[0] = 0.5f * (a[0] + a[1]);
    out[1] = 0.5f * (a[2] + a[3]);
    out[2] = 0.5f * (a[4] + a[5]);
}

IPLReflectionEffectType toIpl(SteamAudioSettings::Reverb r) {
    switch (r) {
        case SteamAudioSettings::Reverb::Hybrid: return IPL_REFLECTIONEFFECTTYPE_HYBRID;
        case SteamAudioSettings::Reverb::Parametric: return IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
        default: return IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    }
}

void check(IPLerror e, const char* what) {
    if (e != IPL_STATUS_SUCCESS) throw std::runtime_error(std::string("Steam Audio: ") + what + " failed (" + std::to_string(e) + ")");
}

}  // namespace

struct SteamAudioBackend::Impl {
    IPLContext context = nullptr;
    IPLScene scene = nullptr;
    IPLStaticMesh mesh = nullptr;
    IPLSimulator simulator = nullptr;
    IPLReflectionMixer mixer = nullptr;
    IPLAudioSettings audioDirect{};   // sub-block sized: direct effects
    IPLAudioSettings audioRefl{};     // reflection frame: simulator IRs, reflection effects, mixer
    IPLReflectionEffectSettings reflSettings{};

    struct Source {
        IPLSource source = nullptr;
        IPLDirectEffect direct = nullptr;
        IPLReflectionEffect reflection = nullptr;
        IPLSimulationInputs inputs{};
        IPLDirectEffectParams directParams{};
        bool haveDirect = false;
        std::vector<float> dry;   // frame samples of dry input being collected
        bool active = false;      // any non-zero dry input in the current frame, or a tail still ringing
        int tailFrames = 0;       // frames since the input went silent
    };
    std::vector<Source> sources;
    IPLSimulationSharedInputs shared{};
    std::vector<float> ambiScratch;        // nCh x frame: effect output / mixer output
    std::vector<float*> ambiPtrs;
    std::vector<float> accum;              // nCh x frame accumulated reflections (non-mixer path)
    std::vector<float> outFrame;           // nCh x frame: the frame being handed out per sub-block
    bool useMixer = false;
    int fill = 0;                          // dry samples collected in the current frame
    int outPos = 0;                        // read position in outFrame
    bool haveOutput = false;

    // Worker for asynchronous reflection simulation.
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    bool requested = false, running = false, quit = false;

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(m);
            quit = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
        for (auto& s : sources) {
            if (s.reflection) iplReflectionEffectRelease(&s.reflection);
            if (s.direct) iplDirectEffectRelease(&s.direct);
            if (s.source) {
                iplSourceRemove(s.source, simulator);
                iplSourceRelease(&s.source);
            }
        }
        if (mixer) iplReflectionMixerRelease(&mixer);
        if (simulator) iplSimulatorRelease(&simulator);
        if (mesh) iplStaticMeshRelease(&mesh);
        if (scene) iplSceneRelease(&scene);
        if (context) iplContextRelease(&context);
    }
};

SteamAudioBackend::SteamAudioBackend(const MeshGeometry& geometry, const SteamBackendSettings& settings)
    : im_(std::make_unique<Impl>()), s_(settings) {
    Impl& im = *im_;
    nCh_ = (s_.ambiOrder + 1) * (s_.ambiOrder + 1);
    s_.irSeconds = std::clamp(s_.irSeconds, 0.1f, 10.0f);
    numTriangles_ = static_cast<int>(geometry.triangles.size());
    s_.subBlock = std::max(1, s_.subBlock);
    // Reflection frame: a multiple of the sub-block, at least the sub-block.
    frame_ = std::max(s_.steam.frameSize, s_.subBlock);
    frame_ = ((frame_ + s_.subBlock - 1) / s_.subBlock) * s_.subBlock;
    // Bounces: enough for a ray to travel the IR length in this room.
    if (s_.steam.bounces > 0) bounces_ = s_.steam.bounces;
    else bounces_ = static_cast<int>(std::clamp(std::ceil(s_.irSeconds * s_.speedOfSound / std::max(s_.meanFreePath, 0.5f) * 1.25f), 8.0f, 96.0f));

    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    check(iplContextCreate(&cs, &im.context), "context");

    im.audioDirect.samplingRate = static_cast<IPLint32>(s_.sampleRate);
    im.audioDirect.frameSize = s_.subBlock;
    im.audioRefl.samplingRate = im.audioDirect.samplingRate;
    im.audioRefl.frameSize = frame_;

    // Geometry
    IPLSceneSettings ss{};
    ss.type = IPL_SCENETYPE_DEFAULT;
    check(iplSceneCreate(im.context, &ss, &im.scene), "scene");
    if (!geometry.empty()) {
        std::vector<IPLVector3> verts(geometry.vertices.size());
        for (size_t i = 0; i < verts.size(); ++i) verts[i] = toIpl(geometry.vertices[i]);
        std::vector<IPLTriangle> tris(geometry.triangles.size());
        std::vector<IPLint32> matIdx(geometry.triangles.size());
        for (size_t i = 0; i < tris.size(); ++i) {
            tris[i].indices[0] = geometry.triangles[i][0];
            tris[i].indices[1] = geometry.triangles[i][1];
            tris[i].indices[2] = geometry.triangles[i][2];
            const int m = i < geometry.materialIndices.size() ? geometry.materialIndices[i] : 0;
            matIdx[i] = std::clamp(m, 0, static_cast<int>(geometry.materials.size()) - 1);
        }
        std::vector<IPLMaterial> mats(std::max<size_t>(1, geometry.materials.size()));
        for (size_t i = 0; i < geometry.materials.size(); ++i) {
            const Material& src = geometry.materials[i];
            toBands3(src.absorption, mats[i].absorption);
            mats[i].scattering = std::clamp(src.scattering, 0.0f, 1.0f);
            for (int b = 0; b < 3; ++b) mats[i].transmission[b] = std::clamp(src.transmission[b], 0.0f, 1.0f);
        }
        if (geometry.materials.empty()) {
            mats[0].absorption[0] = mats[0].absorption[1] = mats[0].absorption[2] = 0.1f;
            mats[0].scattering = 0.1f;
        }
        IPLStaticMeshSettings ms{};
        ms.numVertices = static_cast<IPLint32>(verts.size());
        ms.numTriangles = static_cast<IPLint32>(tris.size());
        ms.numMaterials = static_cast<IPLint32>(mats.size());
        ms.vertices = verts.data();
        ms.triangles = tris.data();
        ms.materialIndices = matIdx.data();
        ms.materials = mats.data();
        check(iplStaticMeshCreate(im.scene, &ms, &im.mesh), "static mesh");
        iplStaticMeshAdd(im.mesh, im.scene);
    }
    iplSceneCommit(im.scene);

    // Simulator
    IPLSimulationSettings sim{};
    sim.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
    sim.sceneType = IPL_SCENETYPE_DEFAULT;
    sim.reflectionType = toIpl(s_.steam.reverb);
    sim.maxNumOcclusionSamples = std::max(1, s_.steam.occlusionSamples);
    sim.maxNumRays = std::max(64, s_.steam.rays);
    sim.numDiffuseSamples = 32;
    sim.maxDuration = s_.irSeconds;
    sim.maxOrder = s_.ambiOrder;
    sim.maxNumSources = std::max(1, s_.numSources);
    int threads = s_.steam.threads;
    if (threads <= 0) threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
    sim.numThreads = threads;
    sim.rayBatchSize = 16;
    sim.numVisSamples = 4;
    sim.samplingRate = im.audioRefl.samplingRate;
    sim.frameSize = im.audioRefl.frameSize;
    check(iplSimulatorCreate(im.context, &sim, &im.simulator), "simulator");
    iplSimulatorSetScene(im.simulator, im.scene);

    im.reflSettings.type = sim.reflectionType;
    im.reflSettings.irSize = static_cast<IPLint32>(std::ceil(s_.irSeconds * s_.sampleRate));
    im.reflSettings.numChannels = nCh_;
    im.useMixer = sim.reflectionType == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    if (im.useMixer) check(iplReflectionMixerCreate(im.context, &im.audioRefl, &im.reflSettings, &im.mixer), "reflection mixer");

    im.sources.resize(static_cast<size_t>(std::max(0, s_.numSources)));
    for (auto& src : im.sources) {
        IPLSourceSettings srcs{};
        srcs.flags = sim.flags;
        check(iplSourceCreate(im.simulator, &srcs, &src.source), "source");
        iplSourceAdd(src.source, im.simulator);
        IPLDirectEffectSettings ds{};
        ds.numChannels = 1;
        check(iplDirectEffectCreate(im.context, &im.audioDirect, &ds, &src.direct), "direct effect");
        check(iplReflectionEffectCreate(im.context, &im.audioRefl, &im.reflSettings, &src.reflection), "reflection effect");
        src.dry.assign(frame_, 0.0f);

        IPLSimulationInputs& in = src.inputs;
        in.flags = sim.flags;
        in.directFlags = s_.steam.occlusion
            ? static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION)
            : static_cast<IPLDirectSimulationFlags>(0);
        in.source.right = {1, 0, 0};
        in.source.up = {0, 1, 0};
        in.source.ahead = {0, 0, -1};
        in.source.origin = {0, 0, 0};
        in.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
        if (s_.airAbsorption) {
            in.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
        } else {
            in.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_EXPONENTIAL;
            in.airAbsorptionModel.coefficients[0] = in.airAbsorptionModel.coefficients[1] = in.airAbsorptionModel.coefficients[2] = 0.0f;
        }
        in.directivity.dipoleWeight = 0;
        in.directivity.dipolePower = 1;
        in.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
        in.occlusionRadius = 0.5f;
        in.numOcclusionSamples = sim.maxNumOcclusionSamples;
        in.reverbScale[0] = in.reverbScale[1] = in.reverbScale[2] = 1.0f;
        in.hybridReverbTransitionTime = std::clamp(s_.steam.hybridTransitionSeconds, 0.05f, s_.irSeconds);
        in.hybridReverbOverlapPercent = 0.25f;
        in.baked = IPL_FALSE;
        in.numTransmissionRays = 2;
        iplSourceSetInputs(src.source, sim.flags, &in);
    }
    iplSimulatorCommit(im.simulator);

    im.shared.listener.right = {1, 0, 0};
    im.shared.listener.up = {0, 1, 0};
    im.shared.listener.ahead = {0, 0, -1};
    im.shared.listener.origin = {0, 1.6f, 0};
    im.shared.numRays = sim.maxNumRays;
    im.shared.numBounces = bounces_;
    im.shared.duration = s_.irSeconds;
    im.shared.order = s_.ambiOrder;
    im.shared.irradianceMinDistance = 1.0f;
    iplSimulatorSetSharedInputs(im.simulator, sim.flags, &im.shared);

    im.ambiScratch.assign(static_cast<size_t>(nCh_) * frame_, 0.0f);
    im.accum.assign(static_cast<size_t>(nCh_) * frame_, 0.0f);
    im.outFrame.assign(static_cast<size_t>(nCh_) * frame_, 0.0f);
    im.ambiPtrs.resize(nCh_);
    for (int c = 0; c < nCh_; ++c) im.ambiPtrs[c] = im.ambiScratch.data() + static_cast<size_t>(c) * frame_;

    if (s_.steam.asyncSimulation) {
        im.worker = std::thread([this] {
            Impl& w = *im_;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lk(w.m);
                    w.cv.wait(lk, [&] { return w.requested || w.quit; });
                    if (w.quit) return;
                    w.requested = false;
                    w.running = true;
                }
                iplSimulatorRunReflections(w.simulator);
                haveReflections_.store(true, std::memory_order_release);
                std::lock_guard<std::mutex> lk(w.m);
                w.running = false;
            }
        });
    }
}

SteamAudioBackend::~SteamAudioBackend() = default;

void SteamAudioBackend::setListener(const Vec3& position, const Vec3& right, const Vec3& up, const Vec3& ahead) {
    Impl& im = *im_;
    im.shared.listener.origin = toIpl(position);
    im.shared.listener.right = toIpl(right);
    im.shared.listener.up = toIpl(up);
    im.shared.listener.ahead = toIpl(ahead);
    iplSimulatorSetSharedInputs(im.simulator, static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS), &im.shared);
}

void SteamAudioBackend::setSource(int i, const Vec3& position, const Vec3& ahead, float dipoleWeight, bool occlusion, float occlusionRadius) {
    Impl& im = *im_;
    if (i < 0 || i >= static_cast<int>(im.sources.size())) return;
    Impl::Source& s = im.sources[i];
    IPLSimulationInputs& in = s.inputs;
    const Vec3 f = ahead.lengthSquared() > 1e-12f ? ahead.normalized() : Vec3{0, 0, -1};
    Vec3 upRef = std::fabs(f.y) > 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Vec3 r = f.cross(upRef).normalized();
    const Vec3 u = r.cross(f).normalized();
    in.source.origin = toIpl(position);
    in.source.ahead = toIpl(f);
    in.source.right = toIpl(r);
    in.source.up = toIpl(u);
    in.directivity.dipoleWeight = std::clamp(dipoleWeight, 0.0f, 1.0f);
    in.directivity.dipolePower = 1.0f;
    in.directFlags = (occlusion && s_.steam.occlusion)
        ? static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION)
        : static_cast<IPLDirectSimulationFlags>(0);
    in.occlusionRadius = std::max(0.0f, occlusionRadius);
    in.occlusionType = in.occlusionRadius > 0.01f ? IPL_OCCLUSIONTYPE_VOLUMETRIC : IPL_OCCLUSIONTYPE_RAYCAST;
    iplSourceSetInputs(s.source, static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS), &in);
}

void SteamAudioBackend::simulateDirect() {
    Impl& im = *im_;
    iplSimulatorRunDirect(im.simulator);
    for (auto& s : im.sources) {
        IPLSimulationOutputs out{};
        iplSourceGetOutputs(s.source, IPL_SIMULATIONFLAGS_DIRECT, &out);
        s.directParams = out.direct;
        s.haveDirect = true;
    }
}

void SteamAudioBackend::simulateReflections() {
    Impl& im = *im_;
    if (s_.steam.asyncSimulation) {
        requestReflections();
        return;
    }
    iplSimulatorRunReflections(im.simulator);
    haveReflections_.store(true, std::memory_order_release);
}

void SteamAudioBackend::requestReflections() {
    Impl& im = *im_;
    if (!im.worker.joinable()) {
        simulateReflections();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(im.m);
        if (im.running) return;  // a run is in flight; this request is dropped, the next one lands
        im.requested = true;
    }
    im.cv.notify_one();
}

void SteamAudioBackend::applyOcclusion(int i, float* inout) {
    Impl& im = *im_;
    if (i < 0 || i >= static_cast<int>(im.sources.size())) return;
    Impl::Source& s = im.sources[i];
    if (!s.haveDirect || !s_.steam.occlusion) return;
    IPLDirectEffectParams p = s.directParams;
    p.flags = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
    p.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    float* ch[1] = {inout};
    IPLAudioBuffer buf{1, s_.subBlock, ch};
    iplDirectEffectApply(s.direct, &p, &buf, &buf);
}

void SteamAudioBackend::pushDry(int i, const float* in) {
    Impl& im = *im_;
    if (i < 0 || i >= static_cast<int>(im.sources.size())) return;
    Impl::Source& s = im.sources[i];
    float* dst = s.dry.data() + im.fill;
    bool nonZero = false;
    for (int k = 0; k < s_.subBlock; ++k) {
        dst[k] = in[k];
        nonZero |= in[k] != 0.0f;
    }
    if (nonZero) {
        s.active = true;
        s.tailFrames = 0;
    }
}

void SteamAudioBackend::endSubBlock() {
    Impl& im = *im_;
    im.fill += s_.subBlock;
    if (im.fill < frame_) return;
    im.fill = 0;

    // A frame of dry input is complete: convolve every layer that has signal
    // or a tail still ringing, then collect the mix for the next frame_ samples.
    const int tailFramesToKeep = static_cast<int>(std::ceil(s_.irSeconds * s_.sampleRate / frame_)) + 1;
    bool any = false;
    if (haveReflections()) {
        for (auto& s : im.sources) {
            if (!s.active) continue;
            IPLSimulationOutputs out{};
            iplSourceGetOutputs(s.source, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
            IPLReflectionEffectParams p = out.reflections;
            p.type = im.reflSettings.type;
            p.numChannels = nCh_;
            p.irSize = im.reflSettings.irSize;
            if (im.reflSettings.type != IPL_REFLECTIONEFFECTTYPE_PARAMETRIC && !p.ir) continue;
            float* inCh[1] = {s.dry.data()};
            IPLAudioBuffer inBuf{1, frame_, inCh};
            // The effect reads `out` even when it mixes into `mixer`, so always hand it the scratch buffer.
            IPLAudioBuffer outBuf{nCh_, frame_, im.ambiPtrs.data()};
            if (im.useMixer) {
                iplReflectionEffectApply(s.reflection, &p, &inBuf, &outBuf, im.mixer);
            } else {
                iplReflectionEffectApply(s.reflection, &p, &inBuf, &outBuf, nullptr);
                for (size_t k = 0; k < im.accum.size(); ++k) im.accum[k] += im.ambiScratch[k];
            }
            any = true;
            // Keep convolving silent input until the tail has rung out.
            bool silent = true;
            for (int k = 0; k < frame_ && silent; ++k) silent = s.dry[k] == 0.0f;
            if (silent && ++s.tailFrames > tailFramesToKeep) s.active = false;
        }
    }
    for (auto& s : im.sources) std::fill(s.dry.begin(), s.dry.end(), 0.0f);

    if (im.useMixer) {
        // The mixer holds state even when nothing was fed this frame; always drain it.
        IPLReflectionEffectParams p{};
        p.type = im.reflSettings.type;
        p.numChannels = nCh_;
        p.irSize = im.reflSettings.irSize;
        IPLAudioBuffer outBuf{nCh_, frame_, im.ambiPtrs.data()};
        iplReflectionMixerApply(im.mixer, &p, &outBuf);
        std::copy(im.ambiScratch.begin(), im.ambiScratch.end(), im.outFrame.begin());
    } else {
        std::copy(im.accum.begin(), im.accum.end(), im.outFrame.begin());
        std::fill(im.accum.begin(), im.accum.end(), 0.0f);
    }
    im.haveOutput = im.haveOutput || any;
    im.outPos = 0;
}

bool SteamAudioBackend::pullReflections(float* const* ambiOut) {
    Impl& im = *im_;
    if (!im.haveOutput || im.outPos + s_.subBlock > frame_) return false;
    for (int c = 0; c < nCh_; ++c) {
        const float* src = im.outFrame.data() + static_cast<size_t>(c) * frame_ + im.outPos;
        std::copy(src, src + s_.subBlock, ambiOut[c]);
    }
    im.outPos += s_.subBlock;
    return true;
}

void SteamAudioBackend::reset() {
    Impl& im = *im_;
    for (auto& s : im.sources) {
        iplDirectEffectReset(s.direct);
        iplReflectionEffectReset(s.reflection);
        std::fill(s.dry.begin(), s.dry.end(), 0.0f);
        s.active = false;
        s.tailFrames = 0;
    }
    if (im.mixer) iplReflectionMixerReset(im.mixer);
    std::fill(im.accum.begin(), im.accum.end(), 0.0f);
    std::fill(im.outFrame.begin(), im.outFrame.end(), 0.0f);
    im.fill = 0;
    im.outPos = 0;
    im.haveOutput = false;
}

}  // namespace sp::dsp
