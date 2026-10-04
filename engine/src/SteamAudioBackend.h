// Steam Audio back-end: ray-traced reflections (rendered as a multichannel
// Ambisonics impulse response per layer) and direct-path occlusion /
// transmission against the scene geometry.
//
// The renderer keeps everything else (delay lines and Doppler on the direct
// path, distance, directivity, air absorption, HRTF / VBAP / SH encoding,
// bus decoding). This class only answers two questions per layer: "how
// much of the direct sound gets through the geometry" and "what reflected
// sound field does this layer's dry signal produce".
//
// Threading: construct and destroy anywhere. simulateDirect(),
// simulateReflections(), the set*() calls and the audio calls run on the
// audio thread. With asyncSimulation, requestReflections() wakes a worker
// that runs the ray tracing; results are picked up by the audio thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sp/Renderer.h"
#include "sp/Scene.h"

namespace sp::dsp {

struct SteamBackendSettings {
    float sampleRate = 48000;
    int subBlock = 32;         // the renderer's sub-block: direct effects and the audio API run at this size
    int ambiOrder = 3;         // Ambisonics order of the reflection IR (<= 3)
    int numSources = 0;
    float irSeconds = 2.0f;    // IR length actually used
    float meanFreePath = 10;   // metres; sizes the bounce count when steam.bounces == 0
    float speedOfSound = 343;
    bool airAbsorption = true; // apply Steam's 3-band air absorption to the traced reflections
    SteamAudioSettings steam;
};

class SteamAudioBackend {
public:
    SteamAudioBackend(const MeshGeometry& geometry, const SteamBackendSettings& settings);
    ~SteamAudioBackend();
    SteamAudioBackend(const SteamAudioBackend&) = delete;
    SteamAudioBackend& operator=(const SteamAudioBackend&) = delete;

    int numChannels() const { return nCh_; }
    float irSeconds() const { return s_.irSeconds; }
    int numTriangles() const { return numTriangles_; }
    int bounces() const { return bounces_; }
    int reflectionFrame() const { return frame_; }       // convolution block, samples
    int reflectionLatency() const { return frame_ - s_.subBlock; }

    // --- simulation inputs (world frame, Steam Audio shares our axes)
    void setListener(const Vec3& position, const Vec3& right, const Vec3& up, const Vec3& ahead);
    // `dipoleWeight` 0 = omni, 0.5 = cardioid.
    void setSource(int i, const Vec3& position, const Vec3& ahead, float dipoleWeight, bool occlusion, float occlusionRadius);

    // --- simulation runs
    void simulateDirect();         // occlusion / transmission for all sources; cheap, blocking
    void simulateReflections();    // ray tracing for all sources; expensive, blocking
    void requestReflections();     // asyncSimulation: run on the worker if it is idle
    bool haveReflections() const { return haveReflections_.load(std::memory_order_acquire); }

    // --- audio, subBlock samples each
    // Direct-path occlusion and transmission, in place. No-op until the first simulateDirect().
    void applyOcclusion(int i, float* inout);
    // This layer's dry signal for this sub-block (reference-distance gain and trims applied).
    void pushDry(int i, const float* in);
    // Call once per sub-block after every pushDry(): runs the convolution when
    // a reflection frame is complete.
    void endSubBlock();
    // The reflections of all layers for this sub-block, ACN / N3D, numChannels()
    // x subBlock, lagging the dry input by reflectionLatency() samples.
    // Returns false (and writes nothing) when there is nothing to output yet.
    bool pullReflections(float* const* ambiOut);

    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> im_;
    SteamBackendSettings s_;
    int nCh_ = 16;
    int numTriangles_ = 0;
    int bounces_ = 32;
    int frame_ = 256;
    std::atomic<bool> haveReflections_{false};
};

}  // namespace sp::dsp
