// One instance's audio engine: a single-layer sp::Renderer fed by the host
// track, with the listener controlled by ListenerTimeline.
//
// Threading follows the app's Session: control methods run on any non-audio
// thread (serialised by a mutex), process() on the audio thread. Renderers
// are built on a shared background pool; live edits become patches
// (Renderer::prepareUpdate/applyUpdate); a new renderer replaces the old one
// with a 40 ms crossfade. The audio thread never allocates, frees or blocks.
#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "ListenerTimeline.h"
#include "SharedSession.h"
#include "sp/Renderer.h"

namespace spplug {

struct EngineConfig {
    double sampleRate = 48000;
    int maxBlock = 512;
    sp::OutputMode mode = sp::OutputMode::Binaural;
    sp::SpeakerLayout layout;   // Speakers mode
    bool render = true;         // false: only the listener timeline (a scene instance that passes audio through)
    std::string hrtfPath;
    // Offline bounce: Steam Audio simulates in line with the audio so every
    // block sees up-to-date reflections. In real time it simulates on a
    // worker thread (about one core at the defaults) and the audio thread
    // only convolves. Ignored by renderers that do not use Steam Audio.
    bool offline = false;

    bool operator==(const EngineConfig& o) const;
    bool operator!=(const EngineConfig& o) const { return !(*this == o); }
};

class LayerEngine {
public:
    static constexpr int kMaxOutputs = 16;
    static constexpr int kMaxInputs = 16;   // an Ambisonic layer's channels (third order)

    LayerEngine();
    ~LayerEngine();

    // ---- control side (any thread but the audio thread)
    void configure(const EngineConfig& cfg);
    // `scene` holds this instance's one layer and the listener exactly as the
    // editor wrote it (its own position mode).
    void setScene(const sp::Scene& scene);
    // Starts builds, sends patches, frees retired renderers. Call regularly
    // (the processor's timer); configure() and setScene() call it too.
    void update();
    // Blocks until the newest build has reached the audio side (offline
    // rendering, tests). Returns false on timeout.
    bool waitUntilCurrent(int timeoutMs);
    std::string lastError() const;
    float tailSeconds() const { return tail_.load(); }
    bool hasProgram() const { return hasProgram_.load(); }
    bool usesSteamAudio() const { return usesSteam_.load(); }  // the current program
    bool simulatesInline() const { return usesSteam_.load() && offlineProgram_.load(); }
    int latencySamples() const { return 32; }   // the renderer's sub-block (RenderConfig default)

    // ---- audio thread
    struct Block {
        // The layer's audio: one buffer for a mono layer, left and right for
        // a stereo one, the recording's channels (ACN order) for an Ambisonic
        // one (the renderer's inputs, see Renderer::inputIndex). Null buffers
        // are silence.
        const float* inputs[kMaxInputs] = {};
        int numInputs = 0;
        float* const* outputs = nullptr;
        int numOutputs = 0;
        int numFrames = 0;
        double time = 0;                // timeline seconds of the first frame
        bool playing = false;
        bool jumped = false;            // transport moved discontinuously: clear tails
        const SharedSession* session = nullptr;
        bool useHistory = false;
        sp::LayerControls layer;
    };
    // Writes (replaces) the outputs; leaves them silent until a renderer exists.
    void process(const Block& b);
    // Wait for the inbox lock (offline rendering only).
    void drainInboxBlocking();

    // Listener pose of the latest block: x y z yaw pitch roll (degrees).
    std::array<float, 6> pose() const;

private:
    struct Program;
    struct Patch;
    struct Inbox;

    void startBuildLocked();
    void retire(Program* p);
    void collectGarbage();
    void handleInbox(bool blocking);
    void renderProgram(Program& p, const Block& b, int numOut);

    // control side
    mutable std::mutex lock_;
    EngineConfig config_;
    bool haveConfig_ = false;
    sp::Scene scene_;
    bool haveScene_ = false;
    uint64_t revision_ = 0;
    bool building_ = false;
    std::string error_;

    std::shared_ptr<Inbox> inbox_;   // shared with build jobs, which outlive nothing they touch

    // audio side
    Program* current_ = nullptr;
    Program* fading_ = nullptr;
    int fadePos_ = 0;
    std::atomic<bool> hasProgram_{false};
    std::atomic<float> tail_{3.0f};
    std::atomic<bool> usesSteam_{false};
    std::atomic<bool> offlineProgram_{false};
    std::array<std::atomic<float>, 6> pose_{};
    std::vector<float> fadeBuf_;
};

// Picks the HRTF shipped inside the plugin bundle (Contents/Resources) or
// next to the binary; empty when none is found.
std::string findHrtf();

}  // namespace spplug
