// The standalone app's audio side: layer audio, transport, and the engine
// renderer, driven by the editor.
//
// Threading. The message thread owns the scene and builds "programs" (a
// Renderer plus the layer audio it plays); the audio thread plays the current
// program. They talk through single-producer/single-consumer queues only:
//   * Edits the engine can take live (positions, levels, paths, speed, head)
//     become a Patch, prepared off the audio thread (Renderer::prepareUpdate)
//     and swapped in by the audio thread without allocating.
//   * Edits that need a new Renderer (room, layer count, output mode, sample
//     rate) build a new program on a worker thread; the audio thread then
//     crossfades from the old program to the new one over ~40 ms.
//   * Whatever the audio thread retires goes back on a garbage queue and is
//     freed on the message thread.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include "AudioLibrary.h"
#include "sp/Renderer.h"

namespace spapp {

struct OutputSetup {
    sp::OutputMode mode = sp::OutputMode::Binaural;
    juce::String layout = "7.1.4";
    bool operator==(const OutputSetup& o) const { return mode == o.mode && layout == o.layout; }
};

struct Program;
struct Patch;
struct LayerData;

// Lock-free single-producer/single-consumer ring of pointers.
template <typename T, size_t N>
class SpscQueue {
public:
    bool push(T v) {
        const size_t w = write_.load(std::memory_order_relaxed), next = (w + 1) % N;
        if (next == read_.load(std::memory_order_acquire)) return false;
        items_[w] = v;
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        v = items_[r];
        read_.store((r + 1) % N, std::memory_order_release);
        return true;
    }
private:
    std::array<T, N> items_{};
    std::atomic<size_t> write_{0}, read_{0};
};

class Session : public juce::AudioIODeviceCallback, private juce::Timer {
public:
    explicit Session(juce::AudioDeviceManager& devices);
    ~Session() override;

    // ---- message thread

    // Hands the editor's scene to the engine. `duration` is the timeline
    // length in seconds (where playback stops or loops).
    void setScene(const sp::Scene& scene, double duration);
    void setOutput(const OutputSetup& out);
    const OutputSetup& output() const { return output_; }

    enum class Transport { Play, Pause, Stop, Seek, Loop };
    void transport(Transport cmd, double value = 0);

    struct Tick {
        double time = 0;
        bool playing = false;
        std::array<float, 8> pose{};  // x y z yaw pitch roll distance speed
        std::vector<float> metersDb;
        float cpu = 0;
    };
    Tick tick();

    juce::String statusText() const;
    double sampleRate() const { return deviceRate_.load(); }
    int outputChannels() const;
    juce::String deviceName() const;
    float cpuLoad() const { return cpu_.load(); }
    AudioLibrary& library() { return library_; }

    // Offline render of the current scene to a WAV file on a worker thread.
    // `progress` runs on the message thread (0..1); `done` with an empty
    // error on success.
    void bounce(const juce::File& file, OutputSetup out, double start, double end, double rate,
                std::function<void(float)> progress, std::function<void(juce::String)> done);
    bool bouncing() const { return bounceThread_ != nullptr && bounceThread_->isThreadRunning(); }

    // Builds a RenderConfig for a setup (HRTF location resolved here).
    static sp::RenderConfig makeConfig(const OutputSetup& out, double rate);
    static juce::File hrtfFile();

    std::function<void(juce::String message, bool error)> onMessage;

    // For tests: renderers built so far, and whether one is being built.
    int buildsStarted() const { return buildsStarted_; }
    bool busy() const { return building_ || library_.anyLoading(); }

    // ---- audio thread (AudioIODeviceCallback)
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    struct Command { Transport kind; double value; };
    struct Message { Program* program = nullptr; Patch* patch = nullptr; };
    struct Garbage { Program* program = nullptr; Patch* patch = nullptr; };

    void timerCallback() override;
    void tryUpdate();
    void startBuild();
    void fillLayerData(LayerData& d, const sp::Scene& s, double rate);
    void collectGarbage();

    // audio-thread helpers
    void renderChunk(float* const* out, int numOut, int offset, int n);
    void fillInputs(Program& p, int n, float gainStart, float gainStep, bool metering);
    void retire(Program* p);
    void resetRenderers();
    void handleCommands();

    juce::AudioDeviceManager& devices_;
    AudioLibrary library_;

    // message-thread state
    sp::Scene scene_;
    double duration_ = 30;
    int revision_ = 0;
    OutputSetup output_;
    Program* published_ = nullptr;     // the program most recently sent to the audio thread
    bool building_ = false;
    int buildsStarted_ = 0;
    int buildGeneration_ = 0;
    bool haveScene_ = false;
    juce::ThreadPool builder_{juce::ThreadPoolOptions{}.withThreadName("renderer builder").withNumberOfThreads(1)};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<juce::Thread> bounceThread_;

    SpscQueue<Message, 64> toAudio_;
    juce::SpinLock audioLock_;          // lets the message thread drain the queues while no device runs
    std::atomic<bool> audioRunning_{false};
    SpscQueue<Command, 128> commands_;
    SpscQueue<Garbage, 256> garbage_;

    // shared
    std::atomic<double> deviceRate_{48000};
    std::atomic<int> deviceBlock_{512};
    std::atomic<double> time_{0};
    std::atomic<bool> playing_{false};
    std::atomic<float> cpu_{0};
    std::array<std::atomic<float>, 8> pose_{};
    static constexpr int kMaxMeters = 1024;
    std::array<std::atomic<float>, kMaxMeters> meters_{};

    // audio-thread state
    Program* current_ = nullptr;
    Program* fading_ = nullptr;
    int fadePos_ = 0;
    juce::int64 pos_ = 0;           // playhead in samples
    bool audioPlaying_ = false;
    bool loop_ = false;
    float inGain_ = 0;              // ramps inputs in and out on play/pause
    juce::int64 tailLeft_ = 0;      // samples of reverb tail to keep rendering after pause
};

}  // namespace spapp
