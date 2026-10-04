#include "Bridge.h"

#include <nlohmann/json.hpp>

#include "sp/SceneAnalysis.h"
#include "sp/SceneJson.h"

namespace spapp {

using nlohmann::json;

namespace {

const char* kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf";

json audioInfoJson(const AudioFileInfo& i) {
    json j{{"path", i.path.toStdString()}, {"name", i.name.toStdString()}, {"duration", i.duration},
           {"channels", i.channels}, {"sampleRate", i.sampleRate}};
    if (i.error.isNotEmpty()) j["error"] = i.error.toStdString();
    return j;
}

std::string err(const std::string& e) { return json{{"ok", false}, {"error", e}}.dump(); }

sp::OutputMode modeFrom(const std::string& s) {
    if (s == "speakers") return sp::OutputMode::Speakers;
    if (s == "ambix") return sp::OutputMode::Ambisonics;
    return sp::OutputMode::Binaural;
}

std::string modeName(sp::OutputMode m) {
    return m == sp::OutputMode::Speakers ? "speakers" : m == sp::OutputMode::Ambisonics ? "ambix" : "binaural";
}

// Audio paths are absolute in the editor and relative to the scene file on
// disk (so scenes can move with their audio, and sp-render reads them too).
void resolveAudioPath(json& a, const juce::File& dir) {
    if (!a.is_string()) return;
    const std::string s = a.get<std::string>();
    if (s.empty() || juce::File::isAbsolutePath(s)) return;
    a = dir.getChildFile(s).getFullPathName().toStdString();
}

void relativiseAudioPath(json& a, const juce::File& dir) {
    if (!a.is_string()) return;
    const std::string s = a.get<std::string>();
    if (s.empty() || !juce::File::isAbsolutePath(s)) return;
    const auto rel = juce::File(s).getRelativePathFrom(dir);
    // Keep absolute paths for files on another volume or far away.
    if (!rel.startsWith("../../..") && !juce::File::isAbsolutePath(rel)) a = rel.replaceCharacter('\\', '/').toStdString();
}

void resolveAudioPaths(json& scene, const juce::File& dir) {
    if (!scene.contains("layers")) return;
    for (auto& l : scene["layers"]) {
        if (l.contains("audio")) resolveAudioPath(l["audio"], dir);
        if (l.contains("audio_files") && l["audio_files"].is_array())
            for (auto& f : l["audio_files"]) resolveAudioPath(f, dir);
    }
}

void relativiseAudioPaths(json& scene, const juce::File& dir) {
    if (!scene.contains("layers")) return;
    for (auto& l : scene["layers"]) {
        if (l.contains("audio")) relativiseAudioPath(l["audio"], dir);
        if (l.contains("audio_files") && l["audio_files"].is_array())
            for (auto& f : l["audio_files"]) relativiseAudioPath(f, dir);
    }
}

// A mesh room's OBJ file follows the same rule as audio. The editor keeps the
// mesh as the file wrote it ({"file", "materials"}), not the engine's inline
// triangles, so saving does not bake the OBJ into the scene.
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

// The room's impulse response follows the audio rule too. The engine makes
// the path absolute when it reads the file (sceneFromJson with the scene's
// directory), so only saving needs a hand.
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

}  // namespace

Bridge::Bridge(Session& s, juce::AudioDeviceManager& d) : session_(s), devices_(d) {
    lastDir_ = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    recent_.setMaxNumberOfItems(10);
}

void Bridge::setSettings(juce::PropertiesFile* p) {
    settings_ = p;
    if (p) recent_.restoreFromString(p->getValue("recentScenes"));
}

juce::StringArray Bridge::recentScenes() const {
    juce::StringArray out;
    for (int i = 0; i < recent_.getNumFiles(); ++i)
        if (recent_.getFile(i).existsAsFile()) out.add(recent_.getFile(i).getFullPathName());
    return out;
}

void Bridge::clearRecentScenes() {
    recent_.clear();
    if (settings_) { settings_->setValue("recentScenes", recent_.toString()); settings_->saveIfNeeded(); }
    if (onRecentChanged) onRecentChanged();
}

// Puts a scene first in Open Recent (and, on macOS, the Dock menu's list).
void Bridge::noteRecent(const juce::File& f) {
    recent_.addFile(f);
    juce::RecentlyOpenedFilesList::registerRecentFileNatively(f);
    if (settings_) { settings_->setValue("recentScenes", recent_.toString()); settings_->saveIfNeeded(); }
    if (onRecentChanged) onRecentChanged();
}

void Bridge::dropFiles(const juce::StringArray& paths, int x, int y) {
    json p = json::array();
    for (const auto& s : paths) p.push_back(s.toStdString());
    emit("dropFiles", json{{"paths", p}, {"x", x}, {"y", y}}.dump());
}

void Bridge::dropHover(bool over) { emit("dropHover", json{{"over", over}}.dump()); }

void Bridge::menuCommand(const juce::String& action, const juce::String& path) {
    emit("menu", json{{"action", action.toStdString()}, {"path", path.toStdString()}}.dump());
}

std::string Bridge::call(const std::string& name, const std::string& arg, std::function<void(std::string)> async) {
    const json a = arg.empty() ? json::object() : json::parse(arg, nullptr, false);
    if (a.is_discarded()) return err("Bad arguments for " + name);
    if (name == "analyze") {
        try {
            const sp::Scene scene = sp::sceneFromJson(a.at("scene").dump());
            return sp::analysisToJson(sp::analyzeScene(scene, a.value("duration", 0.0)));
        } catch (const std::exception& e) {
            return json{{"error", e.what()}}.dump();
        }
    }
    if (name == "setScene") {
        try {
            session_.setScene(sp::sceneFromJson(a.at("scene").dump()), a.value("duration", 0.0));
            return json{{"ok", true}}.dump();
        } catch (const std::exception& e) {
            return err(e.what());
        }
    }
    if (name == "transport") {
        const std::string act = a.value("action", "");
        if (a.contains("loop") && a["loop"].is_boolean()) session_.transport(Session::Transport::Loop, a["loop"].get<bool>() ? 1 : 0);
        if (act == "play") session_.transport(Session::Transport::Play);
        else if (act == "pause") session_.transport(Session::Transport::Pause);
        else if (act == "stop") session_.transport(Session::Transport::Stop);
        else if (act == "seek") session_.transport(Session::Transport::Seek, a.value("time", 0.0));
        return "";
    }
    if (name == "setOutput") {
        OutputSetup o;
        o.mode = modeFrom(a.value("mode", "binaural"));
        o.layout = juce::String(a.value("layout", "7.1.4"));
        if (sp::SpeakerLayout::preset(o.layout.toStdString()).numChannels() == 0) return err("Unknown speaker layout " + o.layout.toStdString());
        session_.setOutput(o);
        return json{{"ok", true}}.dump();
    }
    if (name == "info") {
        const auto& o = session_.output();
        return json{{"device", session_.deviceName().toStdString()}, {"sampleRate", session_.sampleRate()},
                    {"outputChannels", session_.outputChannels()}, {"cpu", session_.cpuLoad()},
                    {"output", {{"mode", modeName(o.mode)}, {"layout", o.layout.toStdString()}}},
                    {"status", session_.statusText().toStdString()},
                    {"steamAudio", sp::Renderer::steamAudioAvailable()}}.dump();
    }
    if (name == "audioInfo") {
        json out = json::array();
        for (const auto& p : a.value("paths", std::vector<std::string>{})) out.push_back(audioInfoJson(session_.library().info(juce::String(p))));
        return out.dump();
    }
    if (name == "chooseAudioFiles" && async) { chooseAudioFiles(a, async); return {}; }
    if (name == "chooseFile" && async) { chooseFile(a, async); return {}; }
    if (name == "openScene" && async) { openScene(async); return {}; }
    if (name == "saveScene" && async) { saveScene(arg, async); return {}; }
    if (name == "bounce" && async) { bounce(arg, async); return {}; }
    if (name == "confirm" && async) { confirm(a, async); return {}; }
    if (name == "revealFile") {
        const juce::File f(juce::String::fromUTF8(a.value("path", "").c_str()));
        if (f.exists()) f.revealToUser();
        return json{{"ok", f.exists()}}.dump();
    }
    if (name == "showAudioSettings") { showAudioSettings(); return ""; }
    if (name == "recentScenes") {
        json out = json::array();
        for (const auto& p : recentScenes()) out.push_back(p.toStdString());
        return out.dump();
    }
    if (name == "clearRecentScenes") { clearRecentScenes(); return ""; }
    // A scene by path (Open Recent): {path} -> as openScene.
    if (name == "openScenePath") {
        const juce::File f(juce::String::fromUTF8(a.value("path", "").c_str()));
        if (!f.existsAsFile()) {
            recent_.removeFile(f);
            if (settings_) settings_->setValue("recentScenes", recent_.toString());
            if (onRecentChanged) onRecentChanged();
            return json{{"nativeError", f.getFileName().toStdString() + " is no longer there (moved or deleted)"}}.dump();
        }
        lastDir_ = f.getParentDirectory();
        return readScene(f);
    }
    if (name == "startupScene") {
        pageReady_ = true;
        if (pendingOpen_ == juce::File()) return "null";
        const auto f = pendingOpen_;
        pendingOpen_ = juce::File();
        return readScene(f);
    }
    return err("Unknown function " + name);
}

juce::WebBrowserComponent::Options Bridge::addTo(juce::WebBrowserComponent::Options o) {
    const char* names[] = {"analyze", "setScene", "transport", "setOutput", "info", "audioInfo", "chooseAudioFiles", "chooseFile",
                           "openScene", "saveScene", "bounce", "showAudioSettings", "startupScene", "confirm", "revealFile",
                           "recentScenes", "clearRecentScenes", "openScenePath"};
    for (const char* n : names) {
        const std::string name = n;
        o = o.withNativeFunction(juce::Identifier(n), [this, name](const juce::Array<juce::var>& args, Completion done) {
            const std::string arg = args.isEmpty() ? std::string() : args[0].toString().toStdString();
            auto finish = [done](std::string r) { done(juce::var(juce::String(r))); };
            const bool isAsync = name == "chooseAudioFiles" || name == "chooseFile" || name == "openScene" || name == "saveScene" ||
                                 name == "bounce" || name == "confirm";
            try {
                if (isAsync) call(name, arg, finish);
                else finish(call(name, arg));
            } catch (const std::exception& e) {
                finish(json{{"nativeError", e.what()}}.dump());
            }
        });
    }
    return o;
}

void Bridge::emit(const juce::Identifier& id, const std::string& j) {
    if (browser_) browser_->emitEventIfBrowserIsVisible(id, juce::JSON::parse(juce::String(j)));
}

void Bridge::sendTick() {
    const auto t = session_.tick();
    json j{{"time", t.time}, {"playing", t.playing}, {"pose", t.pose}, {"meters", t.metersDb}, {"cpu", t.cpu}};
    emit("tick", j.dump());
}

void Bridge::sendMessage(const juce::String& text, const juce::String& level) {
    emit("message", json{{"text", text.toStdString()}, {"level", level.toStdString()}}.dump());
}

// The editor's web view cannot show its own dialogs on macOS (WKWebView
// answers window.confirm with Cancel unless the host implements it, and JUCE
// does not), so the editor asks through here.
void Bridge::confirm(const json& a, std::function<void(std::string)> done) {
    auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Spatial Panner",
                                                             juce::String::fromUTF8(a.value("message", "").c_str()),
                                                             juce::String::fromUTF8(a.value("ok", "OK").c_str()), "Cancel", browser_);
    // NativeMessageBox reports the button's index: 0 is the first (OK).
    juce::NativeMessageBox::showAsync(opts, [done](int button) { done(json{{"ok", button == 0}}.dump()); });
}

// One file dialog at a time. A second request while one is open (a double
// click, or Open pressed with the dialog behind the window) is ignored rather
// than replacing the open dialog, which would drop the first request.
bool Bridge::beginChooser(std::unique_ptr<juce::FileChooser> c, int flags, std::function<void(const juce::FileChooser&)> fn,
                          const std::function<void(std::string)>& done) {
    if (chooserOpen_) { done("null"); return false; }
    chooserOpen_ = true;
    chooser_ = std::move(c);
    chooser_->launchAsync(flags, [this, fn](const juce::FileChooser& fc) {
        chooserOpen_ = false;
        fn(fc);
    });
    return true;
}

// {"title"?} -> [audio info, ...] in the order the files were chosen, or null.
void Bridge::chooseAudioFiles(const json& a, std::function<void(std::string)> done) {
    const juce::String title = juce::String::fromUTF8(a.value("title", "Add audio files as layers").c_str());
    beginChooser(std::make_unique<juce::FileChooser>(title, lastDir_, kAudioWildcard),
                 juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                     juce::FileBrowserComponent::canSelectMultipleItems,
                 [this, done](const juce::FileChooser& fc) {
                     json out = json::array();
                     for (const auto& f : fc.getResults()) {
                         lastDir_ = f.getParentDirectory();
                         out.push_back(audioInfoJson(session_.library().info(f.getFullPathName())));
                     }
                     done(out.dump());
                 }, done);
}

// One file of any kind: {"title", "wildcard"} -> {"path", "name"} or null.
void Bridge::chooseFile(const json& a, std::function<void(std::string)> done) {
    const juce::String title = juce::String::fromUTF8(a.value("title", "Choose a file").c_str());
    const juce::String wildcard = juce::String::fromUTF8(a.value("wildcard", "*").c_str());
    beginChooser(std::make_unique<juce::FileChooser>(title, lastDir_, wildcard),
                 juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                 [this, done](const juce::FileChooser& fc) {
                     const auto f = fc.getResult();
                     if (f == juce::File()) { done("null"); return; }
                     lastDir_ = f.getParentDirectory();
                     done(json{{"path", f.getFullPathName().toStdString()}, {"name", f.getFileNameWithoutExtension().toStdString()}}.dump());
                 }, done);
}

void Bridge::openScene(std::function<void(std::string)> done) {
    beginChooser(std::make_unique<juce::FileChooser>("Open a scene", lastDir_, "*.spscene;*.json"),
                 juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                 [this, done](const juce::FileChooser& fc) {
                     const auto f = fc.getResult();
                     if (f == juce::File()) { done("null"); return; }
                     lastDir_ = f.getParentDirectory();
                     done(readScene(f));
                 }, done);
}

// {path, scene (canonical, absolute audio paths), raw (the file as written)}.
std::string Bridge::readScene(const juce::File& f) {
    try {
        const std::string text = f.loadFileAsString().toStdString();
        json raw = json::parse(text);
        const juce::File dir = f.getParentDirectory();
        json scene = json::parse(sp::sceneToJson(sp::sceneFromJson(text, dir.getFullPathName().toStdString())));
        resolveAudioPaths(scene, dir);
        resolveMeshPath(scene, raw, dir);
        noteRecent(f);
        return json{{"path", f.getFullPathName().toStdString()}, {"scene", scene}, {"raw", raw}}.dump();
    } catch (const std::exception& e) {
        return json{{"nativeError", std::string("Could not read ") + f.getFileName().toStdString() + ": " + e.what()}}.dump();
    }
}

void Bridge::openFile(const juce::File& f) {
    if (!pageReady_) { pendingOpen_ = f; return; }
    emit("openFile", readScene(f));
}

void Bridge::saveScene(const std::string& arg, std::function<void(std::string)> done) {
    json a = json::parse(arg);
    auto write = [this, a, done](const juce::File& f) mutable {
        try {
            json scene = a.at("scene");
            sp::sceneFromJson(scene.dump());  // validate before writing
            relativiseAudioPaths(scene, f.getParentDirectory());
            relativiseMeshPath(scene, f.getParentDirectory());
            relativiseIrPath(scene, f.getParentDirectory());
            if (!f.replaceWithText(juce::String(scene.dump(2)) + "\n")) throw std::runtime_error("Cannot write " + f.getFullPathName().toStdString());
            lastDir_ = f.getParentDirectory();
            noteRecent(f);
            done(json{{"path", f.getFullPathName().toStdString()}}.dump());
        } catch (const std::exception& e) {
            done(json{{"nativeError", e.what()}}.dump());
        }
    };
    if (a.contains("path") && a["path"].is_string() && !a["path"].get<std::string>().empty()) {
        write(juce::File(a["path"].get<std::string>()));
        return;
    }
    const std::string name = a["scene"].value("name", "scene");
    // New scenes are .spscene files (JSON inside), which Finder opens with the app.
    beginChooser(std::make_unique<juce::FileChooser>("Save scene", lastDir_.getChildFile(juce::File::createLegalFileName(name) + ".spscene"), "*.spscene"),
                 juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                 [write, done](const juce::FileChooser& fc) mutable {
                     auto f = fc.getResult();
                     if (f == juce::File()) { done("null"); return; }
                     if (!isSceneFile(f)) f = f.withFileExtension("spscene");
                     write(f);
                 }, done);
}

// The file only appears under its name once the render is complete (see
// Session::bounce); progress and the result go to the editor as "bounce"
// events, which is what tells the user where the file is.
void Bridge::bounce(const std::string& arg, std::function<void(std::string)> done) {
    const json a = json::parse(arg);
    OutputSetup o;
    o.mode = modeFrom(a.value("mode", "binaural"));
    o.layout = juce::String(a.value("layout", "7.1.4"));
    const double start = a.value("start", 0.0), end = a.value("end", 30.0), rate = a.value("sampleRate", 48000.0);
    if (session_.bouncing()) { done(json{{"nativeError", "A bounce is already running"}}.dump()); return; }
    const juce::String suffix = o.mode == sp::OutputMode::Binaural ? "binaural" : o.mode == sp::OutputMode::Ambisonics ? "ambix" : o.layout;
    beginChooser(std::make_unique<juce::FileChooser>("Bounce to WAV", lastDir_.getChildFile("bounce_" + suffix + ".wav"), "*.wav"),
                 juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                 [this, o, start, end, rate, done](const juce::FileChooser& fc) {
                     auto f = fc.getResult();
                     if (f == juce::File()) { done("null"); return; }
                     if (!f.hasFileExtension("wav")) f = f.withFileExtension("wav");
                     lastDir_ = f.getParentDirectory();
                     const std::string path = f.getFullPathName().toStdString();
                     emit("bounce", json{{"path", path}, {"progress", 0.0}}.dump());
                     session_.bounce(
                         f, o, start, end, rate,
                         [this, path](float p) { emit("bounce", json{{"path", path}, {"progress", p}}.dump()); },
                         [this, f, path](juce::String error) {
                             if (error.isEmpty()) {
                                 emit("bounce", json{{"path", path}, {"progress", 1.0}, {"done", true}}.dump());
                                 sendMessage("Bounced to " + f.getFullPathName(), "info");
                             } else {
                                 emit("bounce", json{{"path", path}, {"done", true}, {"error", error.toStdString()}}.dump());
                                 sendMessage("Bounce failed: " + error, "error");
                             }
                         });
                     done(json{{"path", path}}.dump());
                 }, done);
}

void Bridge::showAudioSettings() {
    auto* selector = new juce::AudioDeviceSelectorComponent(devices_, 0, 0, 2, 16, false, false, false, false);
    selector->setSize(520, 420);
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(selector);
    opts.dialogTitle = "Audio device";
    opts.dialogBackgroundColour = juce::Colour(0xff1d2027);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.launchAsync();
}

}  // namespace spapp
