// Native functions the editor (ui/, running in the WebView) calls, and the
// events the app pushes to it. The wire format is JSON text both ways; see
// ui/src/bridge/backend.ts for the other side.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <nlohmann/json.hpp>

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

    // Audio files dropped on the window at (x, y) in the editor's pixels;
    // the editor adds the mono and stereo ones as layers there.
    void dropFiles(const juce::StringArray& paths, int x, int y);
    // A file drag is over the window (true) or has left it (false).
    void dropHover(bool over);
    // A command from the app's menu bar: "new", "open", "save", "saveAs",
    // or "openRecent" with the path.
    void menuCommand(const juce::String& action, const juce::String& path = {});

    // Recently opened and saved scenes, newest first. `settings` keeps them
    // between launches (null in tests); `onRecentChanged` runs after a change.
    void setSettings(juce::PropertiesFile* settings);
    juce::StringArray recentScenes() const;
    void clearRecentScenes();
    std::function<void()> onRecentChanged;

    // Scene files the app opens: its own .spscene, and .json from before.
    static bool isSceneFile(const juce::File& f) { return f.hasFileExtension("spscene;json"); }

    // Exposed for tests: the functions without the WebView around them.
    std::string call(const std::string& name, const std::string& argJson, std::function<void(std::string)> async = {});

private:
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

    void emit(const juce::Identifier& id, const std::string& json);
    void chooseAudioFiles(const nlohmann::json& a, std::function<void(std::string)> done);
    void chooseFile(const nlohmann::json& args, std::function<void(std::string)> done);
    void openScene(std::function<void(std::string)> done);
    std::string readScene(const juce::File& f);
    void noteRecent(const juce::File& f);
    void saveScene(const std::string& arg, std::function<void(std::string)> done);
    void bounce(const std::string& arg, std::function<void(std::string)> done);
    void confirm(const nlohmann::json& args, std::function<void(std::string)> done);
    bool beginChooser(std::unique_ptr<juce::FileChooser> chooser, int flags, std::function<void(const juce::FileChooser&)> fn,
                      const std::function<void(std::string)>& done);
    void showAudioSettings();

    Session& session_;
    juce::AudioDeviceManager& devices_;
    juce::WebBrowserComponent* browser_ = nullptr;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool chooserOpen_ = false;
    juce::File lastDir_;
    juce::File pendingOpen_;
    bool pageReady_ = false;
    juce::RecentlyOpenedFilesList recent_;
    juce::PropertiesFile* settings_ = nullptr;
};

}  // namespace spapp
