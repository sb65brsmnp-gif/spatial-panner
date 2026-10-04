// Decoded layer audio, shared by every renderer program that uses it.
//
// Files are read on a background thread, kept as mono or as a left/right
// pair (further channels are dropped) and resampled to the device rate.
// Everything is held in memory; a 3-minute stereo stem at 48 kHz is ~70 MB.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace spapp {

struct LayerAudio {
    std::vector<std::vector<float>> channels;  // 1 or 2 channels, at `sampleRate`
    double sampleRate = 0;
    int numChannels() const { return static_cast<int>(channels.size()); }
    size_t numSamples() const { return channels.empty() ? 0 : channels[0].size(); }
    // Channel `c`, or the last one a mono file has (a stereo layer playing a
    // mono file plays it from both ends).
    const std::vector<float>& channel(int c) const { return channels[static_cast<size_t>(std::min(c, numChannels() - 1))]; }
};

struct AudioFileInfo {
    juce::String path, name, error;
    double duration = 0;
    int channels = 0;
    double sampleRate = 0;
};

class AudioLibrary {
public:
    AudioLibrary();
    ~AudioLibrary();

    // Reads the header only (fast, any thread).
    AudioFileInfo info(const juce::String& path);

    // Decoded audio at the current target rate, or null while it loads (or
    // when it failed). Starts loading on first request. Message thread.
    std::shared_ptr<const LayerAudio> get(const juce::String& path);

    // Changes the rate audio is delivered at; drops and reloads everything.
    void setTargetRate(double rate);
    double targetRate() const { return rate_; }

    // Called on the message thread whenever a file finishes loading.
    std::function<void()> onLoaded;

    // Synchronous decode at any rate (for bounces; any thread).
    static std::shared_ptr<const LayerAudio> decode(juce::AudioFormatManager& fm, const juce::String& path, double rate,
                                                    juce::String& error);

    bool anyLoading() const;

private:
    struct Entry {
        std::shared_ptr<const LayerAudio> audio;
        bool loading = false;
        juce::String error;
    };
    juce::AudioFormatManager formats_;
    juce::ThreadPool pool_{juce::ThreadPoolOptions{}.withThreadName("audio loader").withNumberOfThreads(2)};
    std::map<juce::String, Entry> entries_;
    double rate_ = 48000;
    int generation_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace spapp
