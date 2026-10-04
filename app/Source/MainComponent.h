// The app window's content: the editor in a WebView, wired to the session.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "Bridge.h"
#include "Session.h"

namespace spapp {

class MainComponent : public juce::Component, private juce::Timer, private juce::ChangeListener {
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void openFile(const juce::File& f) { bridge_.openFile(f); }

    // Asks the editor whether there are unsaved changes, then calls `quit`
    // if there are none or the user agrees to discard them.
    void requestQuit(std::function<void()> quit);

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    std::optional<juce::WebBrowserComponent::Resource> resource(const juce::String& url);

    juce::AudioDeviceManager devices_;
    std::unique_ptr<juce::PropertiesFile> props_;
    Session session_{devices_};
    Bridge bridge_{session_, devices_};
    std::unique_ptr<juce::WebBrowserComponent> browser_;
};

}  // namespace spapp
