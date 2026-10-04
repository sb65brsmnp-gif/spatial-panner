// An Ambisonic recording the plugin plays from disk itself, for layers whose
// channel count exceeds what the track carries (Logic has no 9- or
// 16-channel track for second and third order). Decoded on a background
// thread at the engine's rate; the audio thread reads it at the host's
// timeline position, with the layer's start time and loop setting.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace spplug {

class LayerFile {
public:
    struct Audio {
        std::vector<std::vector<float>> channels;
        double rate = 0;
        size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
    };

    LayerFile();
    ~LayerFile();

    // Message (or host) thread. `paths`: one multichannel file, or one mono
    // file per channel. `channels`: how many the layer wants; missing ones
    // stay silent. The same request again is a no-op.
    void load(const std::vector<std::string>& paths, double rate, int channels);
    void clear();
    bool loading() const { return loading_.load(std::memory_order_relaxed); }
    juce::String error() const;   // message thread

    // Audio thread. Fills `out[c]` for c < numCh with n frames from timeline
    // sample `pos`, the file starting at sample `start`. Silence (and false)
    // while nothing is loaded.
    bool read(float* const* out, int numCh, int n, juce::int64 pos, juce::int64 start, bool loop);

    // Any thread: decodes `paths` as `load` would. Used by the tests.
    static std::shared_ptr<const Audio> decode(const std::vector<std::string>& paths, double rate, int channels, juce::String& error);

private:
    juce::ThreadPool pool_{juce::ThreadPoolOptions{}.withThreadName("layer file").withNumberOfThreads(1)};
    juce::SpinLock lock_;
    std::shared_ptr<const Audio> audio_;     // under lock_
    std::shared_ptr<const Audio> retired_;   // the previous one, released off the audio thread
    std::vector<std::string> paths_;
    double rate_ = 0;
    int channels_ = 0;
    int generation_ = 0;
    std::atomic<bool> loading_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    juce::CriticalSection errorLock_;
    juce::String error_;
};

}  // namespace spplug
