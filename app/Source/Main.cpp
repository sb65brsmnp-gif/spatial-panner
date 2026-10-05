// Spatial Panner standalone app: the 3D scene editor and the spatial audio
// engine in one window.
#include <juce_gui_extra/juce_gui_extra.h>

#include "MainComponent.h"

namespace spapp {

class MainWindow : public juce::DocumentWindow {
public:
    explicit MainWindow(const juce::String& name)
        : DocumentWindow(name, juce::Colour(0xff15171c), DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        content_ = new MainComponent();
        setContentOwned(content_, true);
        setResizable(true, true);
        setResizeLimits(960, 600, 10000, 10000);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    MainComponent* content_ = nullptr;
};

class App : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Spatial Panner"; }
    const juce::String getApplicationVersion() override { return SP_APP_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override {
        window_ = std::make_unique<MainWindow>(getApplicationName());
        openFromCommandLine(commandLine);
    }

    // A scene file given on the command line, or opened from the Finder
    // (double-click or Open With; at launch too, macOS sends it after initialise).
    void anotherInstanceStarted(const juce::String& commandLine) override { openFromCommandLine(commandLine); }

    void openFromCommandLine(const juce::String& commandLine) {
        for (const auto& arg : juce::StringArray::fromTokens(commandLine, true)) {
            const juce::File f(juce::File::getCurrentWorkingDirectory().getChildFile(arg.unquoted()));
            if (Bridge::isSceneFile(f) && f.existsAsFile() && window_ && window_->content_) {
                window_->content_->openFile(f);
                // Opened from the Finder while running: bring the window forward.
                if (window_->isMinimised()) window_->setMinimised(false);
                window_->toFront(true);
                break;
            }
        }
    }
    void shutdown() override { window_.reset(); }
    // Window close and Cmd+Q both land here; unsaved changes are checked first.
    void systemRequestedQuit() override {
        if (window_ && window_->content_) window_->content_->requestQuit([] { juce::JUCEApplicationBase::quit(); });
        else quit();
    }

private:
    std::unique_ptr<MainWindow> window_;
};

}  // namespace spapp

START_JUCE_APPLICATION(spapp::App)
