#include "PluginEditor.h"
#include "sp/Renderer.h"

#include <cmath>

#include <juce_audio_formats/juce_audio_formats.h>

#include "BinaryData.h"
#include "WebViewKeys.h"
#include "SceneDoc.h"
#include "sp/Pose.h"
#include "sp/SceneAnalysis.h"
#include "sp/SceneJson.h"

namespace spplug {

using nlohmann::json;

namespace {

const juce::Colour kBg(0xff16181d), kPanel(0xff1d2027), kText(0xffd8dbe2), kMuted(0xff8a8f9c), kAccent(0xff4f9cf9);

std::string err(const std::string& e) { return json{{"ok", false}, {"error", e}}.dump(); }

// Audio paths are absolute in the editor and relative to the scene file on
// disk (same convention as the app, app/Source/Bridge.cpp).
void resolveAudioPaths(json& scene, const juce::File& dir) {
    if (!scene.contains("layers")) return;
    for (auto& l : scene["layers"]) {
        const std::string a = l.value("audio", "");
        if (a.empty() || juce::File::isAbsolutePath(a)) continue;
        l["audio"] = dir.getChildFile(a).getFullPathName().toStdString();
    }
}

void relativiseAudioPaths(json& scene, const juce::File& dir) {
    if (!scene.contains("layers")) return;
    for (auto& l : scene["layers"]) {
        const std::string a = l.value("audio", "");
        if (a.empty() || !juce::File::isAbsolutePath(a)) continue;
        const auto rel = juce::File(a).getRelativePathFrom(dir);
        if (!rel.startsWith("../../..") && !juce::File::isAbsolutePath(rel)) l["audio"] = rel.replaceCharacter('\\', '/').toStdString();
    }
}

// A mesh room's OBJ file: absolute in the session, relative in a file on
// disk, kept as the file wrote it (as the app does, app/Source/Bridge.cpp).
void resolveMeshPath(json& scene, const json& raw, const juce::File& dir) {
    if (!raw.contains("room") || !raw["room"].contains("mesh") || !scene.contains("room")) return;
    json m = raw["room"]["mesh"];
    if (m.is_string()) m = json{{"file", m}};
    if (!m.is_object() || !m.contains("file")) return;
    const std::string f = m["file"].get<std::string>();
    if (!juce::File::isAbsolutePath(f)) m["file"] = dir.getChildFile(f).getFullPathName().toStdString();
    scene["room"]["mesh"] = m;
}

void relativiseMeshPath(json& scene, const juce::File& dir) {
    if (!scene.contains("room") || !scene["room"].contains("mesh")) return;
    json& m = scene["room"]["mesh"];
    if (!m.is_object() || !m.contains("file")) return;
    const std::string f = m["file"].get<std::string>();
    if (!juce::File::isAbsolutePath(f)) return;
    const auto rel = juce::File(f).getRelativePathFrom(dir);
    if (!rel.startsWith("../../..") && !juce::File::isAbsolutePath(rel)) m["file"] = rel.replaceCharacter('\\', '/').toStdString();
}

// The room's impulse response: the engine makes the path absolute when it
// reads the file; saving makes it relative again.
void relativiseIrPath(json& scene, const juce::File& dir) {
    if (!scene.contains("room") || !scene["room"].contains("impulse_response")) return;
    json& ir = scene["room"]["impulse_response"];
    if (!ir.is_object() || !ir.contains("file") || !ir["file"].is_string()) return;
    const std::string f = ir["file"].get<std::string>();
    if (f.empty()) { scene["room"].erase("impulse_response"); return; }
    if (!juce::File::isAbsolutePath(f)) return;
    const auto rel = juce::File(f).getRelativePathFrom(dir);
    if (!rel.startsWith("../../..") && !juce::File::isAbsolutePath(rel)) ir["file"] = rel.replaceCharacter('\\', '/').toStdString();
}

// Header of an audio file (for the Room tab's impulse response readout).
json audioInfoJson(const juce::File& f) {
    json j{{"path", f.getFullPathName().toStdString()}, {"name", f.getFileNameWithoutExtension().toStdString()},
           {"duration", 0.0}, {"channels", 0}, {"sampleRate", 0}};
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    if (!r) { j["error"] = f.existsAsFile() ? "unsupported audio format" : "file not found"; return j; }
    j["duration"] = r->sampleRate > 0 ? r->lengthInSamples / r->sampleRate : 0.0;
    j["channels"] = static_cast<int>(r->numChannels);
    j["sampleRate"] = r->sampleRate;
    return j;
}

}  // namespace

// ============================================================ 3D editor (scene)

class SceneWebView : public juce::Component, private juce::Timer {
public:
    explicit SceneWebView(SpatialPannerProcessor& p) : proc_(p) {
        lastDir_ = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
        auto options = juce::WebBrowserComponent::Options{}
                           .withNativeIntegrationEnabled()
                           .withKeepPageLoadedWhenBrowserIsHidden()
                           .withInitialisationData("spHost", "plugin")
                           .withResourceProvider([](const juce::String& url) -> std::optional<juce::WebBrowserComponent::Resource> {
                               if (url == "/" || url == "/index.html") {
                                   const auto* d = reinterpret_cast<const std::byte*>(BinaryData::index_html);
                                   return juce::WebBrowserComponent::Resource{std::vector<std::byte>(d, d + BinaryData::index_htmlSize),
                                                                              "text/html; charset=utf-8"};
                               }
                               return std::nullopt;
                           });
        const char* names[] = {"analyze", "setScene", "transport", "setOutput", "info", "audioInfo", "chooseAudioFiles", "chooseFile",
                               "openScene", "saveScene", "bounce", "showAudioSettings", "startupScene", "confirm", "editing"};
        for (const char* n : names) {
            const std::string name = n;
            options = options.withNativeFunction(juce::Identifier(n), [this, name](const juce::Array<juce::var>& args,
                                                                                 juce::WebBrowserComponent::NativeFunctionCompletion done) {
                const std::string arg = args.isEmpty() ? std::string() : args[0].toString().toStdString();
                auto finish = [done](std::string r) { done(juce::var(juce::String(r))); };
                try {
                    call(name, arg, finish);
                } catch (const std::exception& e) {
                    finish(json{{"nativeError", e.what()}}.dump());
                }
            });
        }
        browser_ = std::make_unique<juce::WebBrowserComponent>(options);
        addAndMakeVisible(*browser_);
        const auto devUrl = juce::SystemStats::getEnvironmentVariable("SP_UI_DEV_URL", {});
        browser_->goToURL(devUrl.isNotEmpty() ? devUrl : juce::WebBrowserComponent::getResourceProviderRoot());
        proc_.onSceneChanged = [this](const json& d) { emit("sceneReplaced", d.dump()); };
        startTimerHz(30);
    }

    ~SceneWebView() override {
        stopTimer();
        proc_.onSceneChanged = nullptr;
    }

    void resized() override { browser_->setBounds(getLocalBounds()); }

private:
    void emit(const char* id, const std::string& j) {
        browser_->emitEventIfBrowserIsVisible(juce::Identifier(id), juce::JSON::parse(juce::String::fromUTF8(j.c_str())));
    }

    void timerCallback() override {
#if JUCE_MAC
        // Logic's transport keys go up to Logic (WebViewKeys.mm); the web
        // view may be created late or again, so this is repeated every second.
        if (ticks_++ % 30 == 0) passTransportKeysToHost(*this);
#endif
        const auto tr = proc_.transport();
        const auto p = proc_.listenerPose();
        const auto tracks = proc_.hostTracks();
        const json d = proc_.sceneDoc();
        std::vector<float> meters;
        if (d.contains("layers"))
            for (const auto& l : d["layers"]) {
                float db = -120;
                const std::string id = l.value("host_id", std::string());
                for (const auto& t : tracks)
                    if (t.id == id) db = t.meterDb;
                meters.push_back(db);
            }
        // `active`: Logic is running the plug-in (it does not while stopped,
        // unless the track is record-enabled or input-monitored); the time
        // and pose are stale otherwise.
        json j{{"time", tr.time}, {"playing", tr.playing}, {"active", tr.active}, {"pose", {p[0], p[1], p[2], p[3], p[4], p[5], 0, 0}},
               {"meters", meters}, {"cpu", proc_.cpuLoad()}, {"host", true}};
        emit("tick", j.dump());
    }

    void message(const std::string& text, const char* level = "info") {
        emit("message", json{{"text", text}, {"level", level}}.dump());
    }

    void call(const std::string& name, const std::string& arg, std::function<void(std::string)> done) {
        const json a = arg.empty() ? json::object() : json::parse(arg, nullptr, false);
        if (a.is_discarded()) { done(err("Bad arguments for " + name)); return; }
        if (name == "analyze") {
            try {
                const sp::Scene scene = sp::sceneFromJson(a.at("scene").dump());
                done(sp::analysisToJson(sp::analyzeScene(scene, a.value("duration", 0.0))));
            } catch (const std::exception& e) {
                done(json{{"error", e.what()}}.dump());
            }
            return;
        }
        if (name == "setScene") {
            std::string error;
            const json& d = a.contains("doc") ? a["doc"] : a.at("scene");
            done(proc_.setSceneDoc(d, error) ? json{{"ok", true}}.dump() : err(error));
            return;
        }
        if (name == "info") {
            json tracks = json::array();
            for (const auto& t : proc_.hostTracks()) tracks.push_back({{"id", t.id}, {"name", t.name}});
            const auto out = proc_.getBus(false, 0) ? proc_.getBus(false, 0)->getCurrentLayout() : juce::AudioChannelSet::stereo();
            done(json{{"device", juce::PluginHostType().getHostDescription()},
                      {"sampleRate", proc_.getSampleRate()},
                      {"outputChannels", out.size()},
                      {"cpu", proc_.cpuLoad()},
                      {"output", {{"mode", "binaural"}, {"layout", "7.1.4"}}},
                      {"status", proc_.statusText().toStdString()},
                      {"host", "plugin"},
                      {"steamAudio", sp::Renderer::steamAudioAvailable()},
                      {"tracks", tracks}}.dump());
            return;
        }
        if (name == "startupScene") {
            const json d = proc_.sceneDoc();
            done(json{{"path", nullptr}, {"scene", d}, {"raw", d}}.dump());
            return;
        }
        if (name == "editing") {
            // The page has a text field focused (or not): while it has,
            // Logic's transport keys are typed into it instead of going to
            // Logic (WebViewKeys.mm).
#if JUCE_MAC
            setTextEditing(a.value("on", false));
#endif
            done(json{{"ok", true}}.dump());
            return;
        }
        if (name == "confirm") {
            // WKWebView answers window.confirm with Cancel unless the host
            // implements it (JUCE does not), so the editor asks here.
            auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Spatial Panner",
                                                                     juce::String::fromUTF8(a.value("message", "").c_str()),
                                                                     juce::String::fromUTF8(a.value("ok", "OK").c_str()), "Cancel", this);
            juce::NativeMessageBox::showAsync(opts, [self = juce::Component::SafePointer<juce::Component>(this), done](int button) {
                if (self != nullptr) done(json{{"ok", button == 0}}.dump());  // button index: 0 is OK
            });
            return;
        }
        if (name == "openScene") { openScene(done); return; }
        if (name == "saveScene") { saveScene(a, done); return; }
        if (name == "chooseAudioFiles") { done("[]"); return; }  // tracks supply the audio
        if (name == "audioInfo") {
            json out = json::array();
            for (const auto& p : a.value("paths", std::vector<std::string>{})) out.push_back(audioInfoJson(juce::File(juce::String::fromUTF8(p.c_str()))));
            done(out.dump());
            return;
        }
        if (name == "chooseFile") { chooseFile(a, done); return; }
        if (name == "bounce") {
            message("Bounce from Logic (File > Bounce): every track renders its own layer.");
            done("null");
            return;
        }
        if (name == "setOutput") { done(json{{"ok", true}}.dump()); return; }
        done("");  // transport, showAudioSettings: Logic owns these
    }

    // One file of any kind: {"title", "wildcard"} -> {"path", "name"} or null.
    void chooseFile(const json& a, std::function<void(std::string)> done) {
        chooser_ = std::make_unique<juce::FileChooser>(juce::String::fromUTF8(a.value("title", "Choose a file").c_str()), lastDir_,
                                                       juce::String::fromUTF8(a.value("wildcard", "*").c_str()));
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, done](const juce::FileChooser& fc) {
                                  const auto f = fc.getResult();
                                  if (f == juce::File()) { done("null"); return; }
                                  lastDir_ = f.getParentDirectory();
                                  done(json{{"path", f.getFullPathName().toStdString()}, {"name", f.getFileNameWithoutExtension().toStdString()}}.dump());
                              });
    }

    void openScene(std::function<void(std::string)> done) {
        chooser_ = std::make_unique<juce::FileChooser>("Open a scene", lastDir_, "*.spscene;*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, done](const juce::FileChooser& fc) {
                                  const auto f = fc.getResult();
                                  if (f == juce::File()) { done("null"); return; }
                                  lastDir_ = f.getParentDirectory();
                                  try {
                                      const std::string text = f.loadFileAsString().toStdString();
                                      json raw = json::parse(text);
                                      const juce::File dir = f.getParentDirectory();
                                      json scene = json::parse(sp::sceneToJson(sp::sceneFromJson(text, dir.getFullPathName().toStdString())));
                                      resolveAudioPaths(scene, dir);
                                      resolveMeshPath(scene, raw, dir);
                                      done(json{{"path", f.getFullPathName().toStdString()}, {"scene", scene}, {"raw", raw}}.dump());
                                  } catch (const std::exception& e) {
                                      done(json{{"nativeError", std::string("Could not read ") + f.getFileName().toStdString() + ": " + e.what()}}.dump());
                                  }
                              });
    }

    void saveScene(const json& a, std::function<void(std::string)> done) {
        auto write = [this, a, done](const juce::File& f) {
            try {
                json scene = a.at("scene");
                sp::sceneFromJson(scene.dump());
                relativiseAudioPaths(scene, f.getParentDirectory());
                relativiseMeshPath(scene, f.getParentDirectory());
                relativiseIrPath(scene, f.getParentDirectory());
                if (!f.replaceWithText(juce::String::fromUTF8(scene.dump(2).c_str()) + "\n"))
                    throw std::runtime_error("Cannot write " + f.getFullPathName().toStdString());
                lastDir_ = f.getParentDirectory();
                done(json{{"path", f.getFullPathName().toStdString()}}.dump());
            } catch (const std::exception& e) {
                done(json{{"nativeError", e.what()}}.dump());
            }
        };
        if (a.contains("path") && a["path"].is_string() && !a["path"].get<std::string>().empty()) {
            write(juce::File(a["path"].get<std::string>()));
            return;
        }
        const std::string nm = a.contains("scene") ? a["scene"].value("name", "scene") : "scene";
        chooser_ = std::make_unique<juce::FileChooser>("Export the scene", lastDir_.getChildFile(juce::File::createLegalFileName(nm) + ".spscene"), "*.spscene");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [write, done](const juce::FileChooser& fc) {
                                  auto f = fc.getResult();
                                  if (f == juce::File()) { done("null"); return; }
                                  if (!f.hasFileExtension("spscene;json")) f = f.withFileExtension("spscene");
                                  write(f);
                              });
    }

    SpatialPannerProcessor& proc_;
    std::unique_ptr<juce::WebBrowserComponent> browser_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::File lastDir_;
    unsigned ticks_ = 0;
};

// ================================================================ map (layer)

class LayerMap : public juce::Component, private juce::Timer {
public:
    explicit LayerMap(SpatialPannerProcessor& p) : proc_(p) { startTimerHz(15); }

    void paint(juce::Graphics& g) override {
        g.fillAll(kBg);
        auto area = getLocalBounds().reduced(16).toFloat();
        if (layers_.empty() && path_.empty() && !room_) {
            g.setColour(kMuted);
            g.drawFittedText("Waiting for the scene", getLocalBounds(), juce::Justification::centred, 2);
            return;
        }
        // Bounds of everything, top view (x right, -z up the screen).
        juce::Rectangle<float> b;
        auto grow = [&](float x, float z) {
            const juce::Rectangle<float> r(x - 0.5f, z - 0.5f, 1.0f, 1.0f);
            b = b.isEmpty() ? r : b.getUnion(r);
        };
        if (room_) { grow(roomMin_.x, roomMin_.z); grow(roomMax_.x, roomMax_.z); }
        for (const auto& l : layers_) grow(l.pos.x, l.pos.z);
        for (const auto& q : path_) grow(q.x, q.z);
        grow(listener_[0], listener_[2]);
        b = b.expanded(1.0f);
        const float scale = std::min(area.getWidth() / b.getWidth(), area.getHeight() / b.getHeight());
        const float ox = area.getCentreX() - b.getCentreX() * scale, oy = area.getCentreY() - b.getCentreY() * scale;
        auto map = [&](float x, float z) { return juce::Point<float>(ox + x * scale, oy + z * scale); };

        if (room_) {
            const auto a = map(roomMin_.x, roomMin_.z), c = map(roomMax_.x, roomMax_.z);
            g.setColour(kPanel);
            g.fillRect(juce::Rectangle<float>(a, c));
            g.setColour(kMuted.withAlpha(0.6f));
            g.drawRect(juce::Rectangle<float>(a, c), 1.5f);
        }
        if (path_.size() > 1) {
            juce::Path p;
            p.startNewSubPath(map(path_[0].x, path_[0].z));
            for (size_t i = 1; i < path_.size(); ++i) p.lineTo(map(path_[i].x, path_[i].z));
            g.setColour(kAccent.withAlpha(0.7f));
            g.strokePath(p, juce::PathStrokeType(2.0f));
        }
        g.setFont(juce::FontOptions(12.0f));
        for (const auto& l : layers_) {
            const auto c = map(l.pos.x, l.pos.z);
            const float r = l.mine ? 8.0f : 5.0f;
            g.setColour(l.colour.withAlpha(l.mine ? 1.0f : 0.55f));
            g.fillEllipse(c.x - r, c.y - r, 2 * r, 2 * r);
            if (l.mine) { g.setColour(juce::Colours::white); g.drawEllipse(c.x - r, c.y - r, 2 * r, 2 * r, 2.0f); }
            g.setColour(l.mine ? kText : kMuted);
            g.drawText(l.name, juce::Rectangle<float>(c.x + r + 3, c.y - 8, 160, 16), juce::Justification::centredLeft);
        }
        // Listener: a head with its facing direction (yaw positive = left).
        const auto c = map(listener_[0], listener_[2]);
        const float yaw = listener_[3] * juce::MathConstants<float>::pi / 180.0f;
        const juce::Point<float> fwd(-std::sin(yaw), -std::cos(yaw));
        g.setColour(juce::Colours::white);
        g.fillEllipse(c.x - 6, c.y - 6, 12, 12);
        g.drawLine(c.x, c.y, c.x + fwd.x * 20, c.y + fwd.y * 20, 2.5f);
        g.setColour(kMuted);
        g.drawText("listener", juce::Rectangle<float>(c.x - 40, c.y + 8, 80, 14), juce::Justification::centred);

        g.setColour(kMuted);
        g.drawFittedText(mineFound_ ? "This track's layer is highlighted. Move layers and draw paths in the scene track's window."
                                    : "This track gets its own layer as soon as the scene track sees it.",
                         getLocalBounds().removeFromBottom(22).reduced(8, 0), juce::Justification::centredLeft, 1);
    }

private:
    struct Dot { sp::Vec3 pos; juce::String name; juce::Colour colour; bool mine = false; };

    void timerCallback() override {
        const json d = proc_.sceneDoc();
        const std::string text = d.dump();
        if (text != docText_) {
            docText_ = text;
            rebuild(d);
        }
        listener_ = proc_.listenerPose();
        repaint();
    }

    void rebuild(const json& d) {
        layers_.clear();
        path_.clear();
        room_ = false;
        mineFound_ = false;
        try {
            const sp::Scene s = sp::sceneFromJson(d.dump());
            const auto& raw = d.at("layers");
            for (size_t i = 0; i < s.layers.size(); ++i) {
                Dot dot;
                dot.pos = s.layers[i].position;
                dot.name = juce::String::fromUTF8(s.layers[i].name.c_str());
                dot.colour = juce::Colour::fromString("ff" + juce::String(raw[i].value("color", std::string("#4f9cf9"))).substring(1));
                dot.mine = raw[i].value("host_id", std::string()) == proc_.layerId();
                mineFound_ = mineFound_ || dot.mine;
                layers_.push_back(dot);
            }
            if (s.room.type == sp::RoomType::Box) {
                room_ = true;
                roomMin_ = s.room.minCorner();
                roomMax_ = s.room.maxCorner();
            }
            const int ap = s.listener.activePath;
            if (ap >= 0 && ap < static_cast<int>(s.listener.paths.size())) {
                const sp::SampledPath sp(s.listener.paths[static_cast<size_t>(ap)]);
                const float len = sp.length();
                const int n = std::max(2, std::min(2000, static_cast<int>(len / 0.1f)));
                for (int i = 0; i <= n; ++i) path_.push_back(sp.positionAt(len * static_cast<float>(i) / static_cast<float>(n)));
            }
        } catch (const std::exception&) {
        }
    }

    SpatialPannerProcessor& proc_;
    std::string docText_;
    std::vector<Dot> layers_;
    std::vector<sp::Vec3> path_;
    bool room_ = false, mineFound_ = false;
    sp::Vec3 roomMin_, roomMax_;
    std::array<float, 6> listener_{};
};

// ==================================================================== editor

PluginEditor::PluginEditor(SpatialPannerProcessor& p) : AudioProcessorEditor(p), proc_(p) {
    role_.addItem("Layer: this track is a sound in the scene", 1);
    role_.addItem("Scene: holds the scene, and this track is a layer too", 2);
    role_.addItem("Scene only: holds the scene, passes this track's audio through", 3);
    role_.onChange = [this] {
        const int id = role_.getSelectedId();
        if (id == 1) proc_.setRole(SpatialPannerProcessor::Role::Layer);
        else if (id == 2) proc_.setRole(SpatialPannerProcessor::Role::Scene, true);
        else if (id == 3) proc_.setRole(SpatialPannerProcessor::Role::Scene, false);
    };
    output_.addItem("Binaural (headphones)", 1);
    output_.addItem("Stereo speakers", 2);
    output_.onChange = [this] { proc_.setStereoAsSpeakers(output_.getSelectedId() == 2); };
    clearHistory_.onClick = [this] {
        proc_.clearAutomationHistory();
    };
    clearHistory_.setTooltip("Forget the automation recorded so far: the listener's (on the scene track) and this track's "
                             "Path Speed. Speed automation moves the listener, and a layer along its path, by what has been "
                             "played; after editing it early in the song, play through once or clear this.");
    for (auto* l : {&status_, &outputLabel_}) {
        l->setColour(juce::Label::textColourId, kMuted);
        l->setFont(juce::FontOptions(13.0f));
        addAndMakeVisible(*l);
    }
    addAndMakeVisible(role_);
    addChildComponent(output_);
    addChildComponent(clearHistory_);
    proc_.onRoleChanged = [this] { rebuildContent(); };
    setResizable(true, true);
    rebuildContent();
    startTimerHz(4);
}

PluginEditor::~PluginEditor() {
    proc_.onRoleChanged = nullptr;
    web_.reset();
}

void PluginEditor::rebuildContent() {
    const auto r = proc_.role();
    shownRole_ = r;
    shownSceneIsLayer_ = proc_.sceneIsLayer();
    const bool scene = r == SpatialPannerProcessor::Role::Scene;
    if (scene && !web_) {
        map_.reset();
        web_ = std::make_unique<SceneWebView>(proc_);
        addAndMakeVisible(*web_);
        setResizeLimits(900, 560, 2560, 1600);
        setSize(1320, 840);
    } else if (!scene && !map_) {
        web_.reset();
        map_ = std::make_unique<LayerMap>(proc_);
        addAndMakeVisible(*map_);
        setResizeLimits(420, 360, 1600, 1200);
        setSize(640, 520);
    }
    refreshHeader();
    resized();
}

void PluginEditor::refreshHeader() {
    const auto r = proc_.role();
    role_.setSelectedId(r == SpatialPannerProcessor::Role::Scene ? (proc_.sceneIsLayer() ? 2 : 3) : (r == SpatialPannerProcessor::Role::Layer ? 1 : 0),
                        juce::dontSendNotification);
    // The build (commit and date) so a report can say which one it is about.
    status_.setText(proc_.statusText() + juce::String(juce::CharPointer_UTF8("  \xc2\xb7  build ")) + SP_BUILD_ID, juce::dontSendNotification);
    const auto out = proc_.getBus(false, 0) ? proc_.getBus(false, 0)->getCurrentLayout() : juce::AudioChannelSet::stereo();
    const bool stereo = out == juce::AudioChannelSet::stereo();
    const bool pass = r == SpatialPannerProcessor::Role::Scene && !proc_.sceneIsLayer();
    output_.setVisible(stereo && !pass);
    output_.setSelectedId(proc_.stereoAsSpeakers() ? 2 : 1, juce::dontSendNotification);
    outputLabel_.setVisible(!stereo || pass);
    outputLabel_.setText(proc_.outputText(), juce::dontSendNotification);
    clearHistory_.setVisible(true);
}

void PluginEditor::timerCallback() {
    if (proc_.role() != shownRole_ || proc_.sceneIsLayer() != shownSceneIsLayer_) rebuildContent();
    else refreshHeader();
}

void PluginEditor::paint(juce::Graphics& g) { g.fillAll(kPanel); }

void PluginEditor::resized() {
    auto area = getLocalBounds();
    auto header = area.removeFromTop(40).reduced(8, 6);
    role_.setBounds(header.removeFromLeft(std::min(390, header.getWidth() / 2)));
    header.removeFromLeft(8);
    if (output_.isVisible()) output_.setBounds(header.removeFromLeft(180));
    if (outputLabel_.isVisible()) outputLabel_.setBounds(header.removeFromLeft(200));
    if (clearHistory_.isVisible()) clearHistory_.setBounds(header.removeFromRight(190));
    header.removeFromLeft(8);
    status_.setBounds(header);
    if (web_) web_->setBounds(area);
    if (map_) map_->setBounds(area);
}

juce::AudioProcessorEditor* SpatialPannerProcessor::createEditor() { return new PluginEditor(*this); }

}  // namespace spplug

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new spplug::SpatialPannerProcessor(); }
