// Native functions the editor (ui/, running in the WebView) calls, and the
// events the app pushes to it. The wire format is JSON text both ways; see
// ui/src/bridge/backend.ts for the other side.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "Session.h"

namespace spapp {

class Bridge {
public:
    Bridge(Session& session, juce::AudioDeviceManager& devices);

    // Registers every native function on the WebView options.
    juce::WebBrowserComponent::Options addTo(juce::WebBrowserComponent::Options options);

    void setBrowser(juce::WebBrowserComponent* b) { browser_ = b; }

    // Pushes the ~30 Hz transport/pose/meters event.
    void sendTick();
    void sendMessage(const juce::String& text, const juce::String& level);

    // Opens a scene file given on the command line or from the Finder: the
    // editor picks it up when it starts (startupScene) or right away.
    void openFile(const juce::File& f);

    // Exposed for tests: the functions without the WebView around them.
    std::string call(const std::string& name, const std::string& argJson, std::function<void(std::string)> async = {});

private:
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

    void emit(const juce::Identifier& id, const std::string& json);
    void chooseAudioFiles(std::function<void(std::string)> done);
    void openScene(std::function<void(std::string)> done);
    std::string readScene(const juce::File& f);
    void saveScene(const std::string& arg, std::function<void(std::string)> done);
    void bounce(const std::string& arg, std::function<void(std::string)> done);
    void showAudioSettings();

    Session& session_;
    juce::AudioDeviceManager& devices_;
    juce::WebBrowserComponent* browser_ = nullptr;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::File lastDir_;
    juce::File pendingOpen_;
    bool pageReady_ = false;
};

}  // namespace spapp
