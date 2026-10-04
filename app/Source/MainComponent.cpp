#include "MainComponent.h"

#include "BinaryData.h"

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
}

MainComponent::~MainComponent() {
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

void MainComponent::timerCallback() { bridge_.sendTick(); }

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
