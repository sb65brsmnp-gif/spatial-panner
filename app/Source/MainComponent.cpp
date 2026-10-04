#include "MainComponent.h"

#include "BinaryData.h"
#include "MacSupport.h"

namespace spapp {

MainComponent::MainComponent() {
    juce::PropertiesFile::Options po;
    po.applicationName = "Spatial Panner";
    po.filenameSuffix = ".settings";
    po.osxLibrarySubFolder = "Application Support";
    po.folderName = "Spatial Panner";
    props_ = std::make_unique<juce::PropertiesFile>(po);

    // Up to 16 outputs (9.1.6 or 3rd-order ambiX); restores the last device.
    const auto saved = props_->getXmlValue("audioDevice");
    const auto error = devices_.initialise(0, 16, saved.get(), true);
    if (error.isNotEmpty()) juce::Logger::writeToLog("Audio device: " + error);
    devices_.addChangeListener(this);

    session_.onMessage = [this](juce::String m, bool isError) { bridge_.sendMessage(m, isError ? "error" : "info"); };
    bridge_.setSettings(props_.get());
    bridge_.onRecentChanged = [this] { menuItemsChanged(); };

    auto options = juce::WebBrowserComponent::Options{}
                       .withNativeIntegrationEnabled()
                       .withKeepPageLoadedWhenBrowserIsHidden()
                       .withResourceProvider([this](const juce::String& url) { return resource(url); });
    options = bridge_.addTo(options);
    browser_ = std::make_unique<juce::WebBrowserComponent>(options);
    bridge_.setBrowser(browser_.get());
    addAndMakeVisible(*browser_);

    // SP_UI_DEV_URL=http://localhost:5173 loads the editor from `npm run dev`
    // (hot reload) instead of the copy built into the app.
    const auto devUrl = juce::SystemStats::getEnvironmentVariable("SP_UI_DEV_URL", {});
    browser_->goToURL(devUrl.isNotEmpty() ? devUrl : juce::WebBrowserComponent::getResourceProviderRoot());

    setSize(1440, 900);
    startTimerHz(30);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#endif
}

MainComponent::~MainComponent() {
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    stopTimer();
    devices_.removeChangeListener(this);
    bridge_.setBrowser(nullptr);
}

std::optional<juce::WebBrowserComponent::Resource> MainComponent::resource(const juce::String& url) {
    if (url == "/" || url == "/index.html") {
        const auto* data = reinterpret_cast<const std::byte*>(BinaryData::index_html);
        return juce::WebBrowserComponent::Resource{std::vector<std::byte>(data, data + BinaryData::index_htmlSize), "text/html; charset=utf-8"};
    }
    return std::nullopt;
}

void MainComponent::resized() { browser_->setBounds(getLocalBounds()); }

void MainComponent::timerCallback() {
    bridge_.sendTick();
#if JUCE_MAC
    // About once a second: keep Finder drags coming to this window, not the web view.
    if (++ticks_ % 30 == 1) passFileDragsToWindow(*this);
#endif
}

namespace {
const char* kAudioExtensions = "wav;wave;aif;aiff;flac;mp3;m4a;ogg;caf";
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& p : files) {
        const juce::File f(p);
        if (f.hasFileExtension(kAudioExtensions) || Bridge::isSceneFile(f)) return true;
    }
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int x, int y) {
    bridge_.dropHover(false);
    juce::StringArray audio;
    for (const auto& p : files) {
        const juce::File f(p);
        if (f.hasFileExtension(kAudioExtensions)) audio.add(p);
    }
    if (!audio.isEmpty()) { bridge_.dropFiles(audio, x, y); return; }
    for (const auto& p : files)
        if (Bridge::isSceneFile(juce::File(p))) { bridge_.openFile(juce::File(p)); return; }
}

enum MenuIds { kNew = 1, kOpen, kSave, kSaveAs, kClearRecent, kRecentBase = 100 };

juce::PopupMenu MainComponent::getMenuForIndex(int, const juce::String&) {
    juce::PopupMenu m;
    m.addItem(kNew, "New Scene");
    m.addItem(kOpen, juce::String::fromUTF8("Open\xe2\x80\xa6"));
    juce::PopupMenu recent;
    const auto files = bridge_.recentScenes();
    for (int i = 0; i < files.size(); ++i) recent.addItem(kRecentBase + i, juce::File(files[i]).getFileName());
    if (!files.isEmpty()) recent.addSeparator();
    recent.addItem(kClearRecent, "Clear Menu", !files.isEmpty());
    m.addSubMenu("Open Recent", recent);
    m.addSeparator();
    m.addItem(kSave, "Save");
    m.addItem(kSaveAs, juce::String::fromUTF8("Save As\xe2\x80\xa6"));
    return m;
}

void MainComponent::menuItemSelected(int id, int) {
    switch (id) {
        case kNew: bridge_.menuCommand("new"); break;
        case kOpen: bridge_.menuCommand("open"); break;
        case kSave: bridge_.menuCommand("save"); break;
        case kSaveAs: bridge_.menuCommand("saveAs"); break;
        case kClearRecent: bridge_.clearRecentScenes(); break;
        default: {
            const auto files = bridge_.recentScenes();
            const int i = id - kRecentBase;
            if (i >= 0 && i < files.size()) bridge_.menuCommand("openRecent", files[i]);
        }
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*) {
    if (auto xml = devices_.createStateXml()) props_->setValue("audioDevice", xml.get());
    props_->saveIfNeeded();
}

void MainComponent::requestQuit(std::function<void()> quit) {
    browser_->evaluateJavascript("window.spEditor ? window.spEditor.store.dirty : false",
                                 [quit](juce::WebBrowserComponent::EvaluationResult r) {
                                     const auto* v = r.getResult();
                                     if (!v || !static_cast<bool>(*v)) { quit(); return; }
                                     juce::AlertWindow::showAsync(
                                         juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon,
                                                                                     "Unsaved changes",
                                                                                     "Quit without saving the scene?", "Quit",
                                                                                     "Cancel"),
                                         [quit](int result) { if (result == 1) quit(); });
                                 });
}

}  // namespace spapp
