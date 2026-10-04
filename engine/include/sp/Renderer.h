// The spatial renderer: "render these layers for this listener".
//
// The engine has no idea of files, transports or hosts. The caller hands it
// one mono buffer per layer and the timeline time of the block; the renderer
// evaluates the listener pose, moves every layer's direct path and
// reflections, and writes binaural, loudspeaker or Ambisonics output.
//
// Threading: construct on any thread (allocates, loads the HRTF); process()
// on the audio thread (no allocation). One Renderer serves one output.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sp/Pose.h"
#include "sp/Scene.h"
#include "sp/SpeakerLayout.h"

namespace sp {

enum class OutputMode { Binaural, Speakers, Ambisonics };

struct RenderConfig {
    double sampleRate = 48000.0;
    OutputMode mode = OutputMode::Binaural;
    SpeakerLayout layout = SpeakerLayout::preset("7.1.4");  // Speakers mode
    int ambisonicsOrder = 3;        // bus order (1..3); also the Ambisonics output order
    std::string hrtfPath;           // SOFA file; required for Binaural
    int subBlockSize = 32;          // pose/parameter update interval in samples
    float maxDistance = 0;          // metres of delay line; 0 = derived from the scene
    bool nearField = true;          // per-ear parallax and level inside ~1.5 m
    float nearFieldRadius = 1.5f;
    int vbapResolutionDeg = 1;
    // HRTF filters are re-derived at most every N sub-blocks (default 4 =
    // 128 samples, 2.7 ms at 48 kHz) and only when the direction moved by
    // more than ~0.15 degrees; each update costs 16 small FFTs per layer.
    int hrtfUpdateInterval = 4;
};

// Live per-layer overrides (the plugin's automatable layer parameters).
struct LayerControls {
    float levelOffsetDb = 0;
    bool mute = false;
    Vec3 positionOffset;
    std::optional<float> dopplerAmount;
    std::optional<float> spreadDeg;
};

class Renderer {
public:
    // `duration` (seconds) bounds the speed-curve integration; pass the
    // timeline length you intend to render, or 0 for "unknown" (10 minutes).
    Renderer(const Scene& scene, const RenderConfig& config, double duration = 0);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    int numInputs() const;    // = scene.layers.size()
    int numOutputs() const;   // 2, layout channels, or (order+1)^2
    int latencySamples() const;
    const RenderConfig& config() const;

    // Render `numFrames` samples. `inputs[i]` is layer i's mono audio (may be
    // null for silence). `outputs[c]` receives channel c. `timeSeconds` is the
    // timeline time of the first frame. Any frame count is accepted.
    void process(const float* const* inputs, float* const* outputs, int numFrames, double timeSeconds);

    // Live controls, safe to call between process() calls.
    void setListenerControls(const ListenerControls& c);
    void setLayerControls(int layer, const LayerControls& c);
    const ListenerControls& listenerControls() const;

    // Pose used for the most recent sub-block (for the UI).
    Pose lastPose() const;
    const PoseEvaluator& poseEvaluator() const;

    // Clears all delay lines and filter states (for transport jumps).
    void reset();

    // Diagnostics
    struct Stats {
        int numImagesPerLayer = 0;
        float reverbRt60Mid = 0;      // seconds
        float reverbGain = 0;         // linear, before trims
        float maxDistance = 0;        // metres of delay line
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sp
