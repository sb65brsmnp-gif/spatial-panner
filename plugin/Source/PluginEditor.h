// The plugin window. A header (role, status, output) on top of either the 3D
// editor (scene instance: the same editor as the app, in a WebView) or a
// top-down map of the scene (layer instances).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

namespace spplug {

class SceneWebView;
class LayerMap;

class PluginEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PluginEditor(SpatialPannerProcessor& p);
    ~PluginEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void rebuildContent();
    void refreshHeader();

    SpatialPannerProcessor& proc_;
    juce::ComboBox role_, output_;
    juce::Label status_, outputLabel_;
    juce::TextButton clearHistory_{"Clear recorded automation"};
    std::unique_ptr<SceneWebView> web_;
    std::unique_ptr<LayerMap> map_;
    SpatialPannerProcessor::Role shownRole_ = SpatialPannerProcessor::Role::Undecided;
    bool shownSceneIsLayer_ = true;
};

}  // namespace spplug
