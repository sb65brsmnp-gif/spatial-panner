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

// How reflections and reverb are produced.
//   Builtin:    image sources + FDN. Exact for box rooms (and the ground
//               plane outdoors), Doppler on every reflection, cheap.
//   SteamAudio: ray tracing against the room geometry (any mesh, objects)
//               with a convolution / hybrid / parametric reverb from the
//               traced impulse response, plus occlusion and transmission
//               of the direct path. Needs the engine built with
//               SP_WITH_STEAM_AUDIO (see Renderer::steamAudioAvailable()).
//   Auto:       SteamAudio when the room is a mesh or has objects and the
//               back-end is available, Builtin otherwise.
enum class ReflectionsBackend { Auto, Builtin, SteamAudio };

struct SteamAudioSettings {
    enum class Reverb { Convolution, Hybrid, Parametric };
    int rays = 4096;                 // rays traced from the listener per simulation
    int bounces = 0;                 // per ray; 0 = enough to cover irSeconds in this room
    float irSeconds = 0;             // impulse response length; 0 = estimated from the room
    // Block size of the reflection convolution (a multiple of subBlockSize).
    // Cost falls with the block; the reflections arrive frameSize -
    // subBlockSize samples late (256: 4.7 ms at 48 kHz).
    int frameSize = 256;
    int threads = 0;                 // ray-tracing threads; 0 = hardware threads - 1
    Reverb reverb = Reverb::Convolution;
    float hybridTransitionSeconds = 0.4f;  // Hybrid: convolution before, parametric after
    float updateInterval = 0.1f;     // seconds between reflection simulations
    bool asyncSimulation = false;    // true: simulate on a worker thread (real time);
                                     // false: in-line in process() (offline rendering)
    bool occlusion = true;           // direct-path occlusion / transmission by geometry
    int occlusionSamples = 16;       // points per source sphere for partial occlusion
};

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

    ReflectionsBackend reflections = ReflectionsBackend::Auto;
    SteamAudioSettings steam;
};

// Live per-layer overrides (the plugin's automatable layer parameters).
struct LayerControls {
    float levelOffsetDb = 0;
    bool mute = false;
    Vec3 positionOffset;
    std::optional<float> dopplerAmount;
    std::optional<float> spreadDeg;
    // Stereo layers: the width is multiplied, the rotation added, and `mono`
    // (when set) replaces Layer::stereo.mono.
    float stereoWidthScale = 1.0f;
    float stereoRotationOffsetDeg = 0;
    std::optional<bool> mono;
};

// A scene edit prepared off the audio thread by Renderer::prepareUpdate and
// applied on it by Renderer::applyUpdate.
struct SceneUpdate {
    Scene scene;
    PoseEvaluator poses;
};

class Renderer {
public:
    // `duration` (seconds) bounds the speed-curve integration; pass the
    // timeline length you intend to render, or 0 for "unknown" (10 minutes).
    Renderer(const Scene& scene, const RenderConfig& config, double duration = 0);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Inputs are one mono buffer per layer channel, layer by layer: layer i
    // takes inputs[inputIndex(i)] .. inputs[inputIndex(i) + channels - 1]
    // (left then right for a stereo layer). numInputs() = inputChannels(scene).
    int numInputs() const;
    int numLayers() const;    // = scene.layers.size()
    int inputIndex(int layer) const;
    int numOutputs() const;   // 2, layout channels, or (order+1)^2
    int latencySamples() const;
    const RenderConfig& config() const;

    // Render `numFrames` samples. `inputs[k]` is input channel k (see
    // inputIndex(); may be null for silence). `outputs[c]` receives channel c.
    // `timeSeconds` is the timeline time of the first frame. Any frame count
    // is accepted.
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

    // Live scene edits (the editor's path while it plays).
    //
    // prepareUpdate() builds what applyUpdate() needs from an edited scene.
    // It allocates, so call it off the audio thread; it is safe to call while
    // process() runs on another thread. It returns null when the edit needs a
    // new Renderer instead: a different number of layers or layer channels,
    // any change to the room or environment, or layers/paths reaching beyond
    // the delay lines.
    // Everything else (layer positions, levels, directivity, spread, Doppler,
    // distance model, sends, paths, speed curve, head track) updates live.
    //
    // applyUpdate() runs on the audio thread between process() calls and does
    // not allocate or free: it swaps the new data in and leaves the old data
    // in `update`, so destroy `update` off the audio thread afterwards.
    // Moved layers glide to their new positions over ~40 ms.
    std::unique_ptr<SceneUpdate> prepareUpdate(const Scene& scene) const;
    void applyUpdate(SceneUpdate& update);

    // True when the engine was built with the Steam Audio back-end.
    static bool steamAudioAvailable();

    // Diagnostics
    struct Stats {
        ReflectionsBackend backend = ReflectionsBackend::Builtin;  // the one in use
        int numImagesPerLayer = 0;    // Builtin
        float reverbRt60Mid = 0;      // seconds (Builtin: Eyring; SteamAudio: IR length basis)
        float reverbGain = 0;         // linear, before trims (Builtin)
        float maxDistance = 0;        // metres of delay line
        float irSeconds = 0;          // SteamAudio: impulse response length
        int numTriangles = 0;         // SteamAudio: geometry handed to the ray tracer
        int bounces = 0;              // SteamAudio: bounces per ray in use
        int reflectionLatency = 0;    // SteamAudio: samples the reflections lag the direct path
        std::string note;             // e.g. why a requested back-end was not used
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sp
