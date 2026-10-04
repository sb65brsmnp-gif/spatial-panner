// The Spatial Panner plugin. One binary, two roles:
//
//   * Layer: on each track whose audio is a layer. Renders that track's audio
//     for the listener (binaural on stereo tracks, the track's speaker layout
//     on surround tracks).
//   * Scene: one per session. Holds the scene (layers' places, room, the
//     listener's paths, speed and head movement), opens the 3D editor, and
//     carries the listener automation (speed, path position, head turn/tilt/
//     roll, active path). It is normally also a layer for its own track; it
//     can instead pass its track's audio through untouched.
//
// Every instance evaluates the listener at the host playhead itself, so they
// stay in sync without passing audio around (docs/plugin.md).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

#include "LayerEngine.h"
#include "SharedSession.h"

namespace spplug {

class SpatialPannerProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    enum class Role { Undecided, Layer, Scene };

    SpatialPannerProcessor();
    ~SpatialPannerProcessor() override;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "Spatial Panner"; }
    void prepareToPlay(double sampleRate, int maxBlock) override;
    void releaseResources() override {}
    void setNonRealtime(bool nonRealtime) noexcept override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    double getTailLengthSeconds() const override { return engine_.tailSeconds(); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    void updateTrackProperties(const TrackProperties&) override;

    juce::AudioProcessorValueTreeState& parameters() { return params_; }

    // ---- roles (message thread)
    Role role() const { return role_; }
    bool sceneIsLayer() const { return sceneIsLayer_; }
    // Layer, or Scene (taking the scene over from whichever instance holds it).
    void setRole(Role r, bool sceneIsLayer = true);
    bool stereoAsSpeakers() const { return stereoAsSpeakers_; }
    void setStereoAsSpeakers(bool b);
    juce::String statusText() const;
    juce::String outputText() const;
    std::function<void()> onRoleChanged;   // the editor swaps its content

    // ---- the scene (message thread)
    // Scene instance: the document it owns. Others: the copy they render.
    nlohmann::json sceneDoc() const;
    // Scene instance only: replaces the document (from the editor). Returns
    // false with `error` when the engine rejects it.
    bool setSceneDoc(const nlohmann::json& doc, std::string& error);
    // Called with the document when the plugin changed it (a track was added
    // or renamed): the editor reloads it.
    std::function<void(const nlohmann::json&)> onSceneChanged;
    void clearAutomationHistory();

    struct HostTrack { std::string id, name; float meterDb = -120; };
    std::vector<HostTrack> hostTracks();   // live layer instances (meters read and reset)
    const std::string& layerId() const { return layerId_; }

    struct Transport { double time = 0; bool playing = false; };
    Transport transport() const { return {lastTime_.load(), lastPlaying_.load()}; }
    std::array<float, 6> listenerPose() const { return engine_.pose(); }
    float cpuLoad() const { return cpu_.load(); }

    // ---- tests
    static void setSessionPathForTesting(const std::string& path);
    void tickForTesting() { timerCallback(); }
    LayerEngine& engineForTesting() { return engine_; }
    SharedSession& session() { return *session_; }

private:
    void timerCallback() override;
    void decideRole();
    void becomeScene(bool force);
    void becomeLayer();
    void manageSlot();
    void adoptPublishedScene();
    void applyDocToEngine(const nlohmann::json& doc);
    void publish();
    void reconfigureEngine();
    EngineConfig engineConfig() const;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState params_;
    std::shared_ptr<SharedSession> session_;
    LayerEngine engine_;
    const uint64_t token_;

    // message-thread state
    Role role_ = Role::Undecided;
    bool sceneIsLayer_ = true;
    bool stereoAsSpeakers_ = false;
    std::string layerId_, cloneOf_, trackName_;
    int slot_ = -1;
    uint32_t constructedMs_ = 0;
    bool stateRestored_ = false;
    nlohmann::json doc_;              // owned (scene) or adopted copy (layer)
    std::string docText_;             // doc_ as last applied, for change detection
    std::string historyText_;         // restored automation history, written on becoming the scene
    uint64_t seenRevision_ = 0;
    uint32_t seenGeneration_ = 0;
    bool needsPublish_ = false;
    juce::String status_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // for callAsync from host threads
    mutable juce::CriticalSection docLock_;  // doc_ vs getStateInformation on another thread

    // set from the message thread, read by the audio thread
    std::atomic<int> slotForAudio_{-1};
    std::atomic<bool> recordHistory_{false};   // this instance owns the scene
    std::atomic<bool> passThrough_{false};      // scene without a layer of its own
    // Set when the host switches to an offline bounce while the renderer
    // simulates Steam Audio asynchronously: the next block waits for the
    // offline renderer so the bounce is deterministic.
    std::atomic<bool> awaitOfflineProgram_{false};
    std::atomic<float> docLevelGain_{1.0f}, docDoppler_{1.0f}, docSpread_{0.0f};

    // audio thread
    double sampleRate_ = 48000;
    juce::int64 expected_ = -1;
    juce::int64 freeRun_ = 0;
    std::vector<float> mono_;
    std::atomic<double> lastTime_{0};
    std::atomic<bool> lastPlaying_{false};
    std::atomic<float> cpu_{0};

    std::atomic<float>* pSpeed_;
    std::atomic<float>* pPosition_;
    std::atomic<float>* pYaw_;
    std::atomic<float>* pPitch_;
    std::atomic<float>* pRoll_;
    std::atomic<float>* pPath_;
    std::atomic<float>* pLevel_;
    std::atomic<float>* pMute_;
    std::atomic<float>* pDoppler_;
    std::atomic<float>* pWidth_;
    std::atomic<float>* pX_;
    std::atomic<float>* pY_;
    std::atomic<float>* pZ_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialPannerProcessor)
};

// Engine speaker layout for a host bus (by channel type, so channel order
// follows the host). Empty when a channel has no known position.
sp::SpeakerLayout speakerLayoutFor(const juce::AudioChannelSet& set);

}  // namespace spplug
