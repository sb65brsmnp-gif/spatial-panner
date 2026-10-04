// The app window's content: the editor in a WebView, wired to the session.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "Bridge.h"
#include "Session.h"

namespace spapp {

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      public juce::MenuBarModel,
                      private juce::Timer,
                      private juce::ChangeListener {
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void openFile(const juce::File& f) { bridge_.openFile(f); }

    // Files dragged from the Finder: audio files become layers where they
    // are dropped, a scene file opens.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { bridge_.dropHover(true); }
    void fileDragExit(const juce::StringArray&) override { bridge_.dropHover(false); }
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // The File menu in the macOS menu bar (with Open Recent).
    juce::StringArray getMenuBarNames() override { return {"File"}; }
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int itemId, int index) override;

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
    int ticks_ = 0;
};

}  // namespace spapp
