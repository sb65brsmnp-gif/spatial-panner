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
        const juce::File f(a);
        const auto rel = f.getRelativePathFrom(dir);
        // Keep absolute paths for files on another volume or far away.
        if (!rel.startsWith("../../..") && !juce::File::isAbsolutePath(rel)) l["audio"] = rel.replaceCharacter('\\', '/').toStdString();
    }
}

}  // namespace

Bridge::Bridge(Session& s, juce::AudioDeviceManager& d) : session_(s), devices_(d) {
    lastDir_ = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
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
                    {"status", session_.statusText().toStdString()}}.dump();
    }
    if (name == "audioInfo") {
        json out = json::array();
        for (const auto& p : a.value("paths", std::vector<std::string>{})) out.push_back(audioInfoJson(session_.library().info(juce::String(p))));
        return out.dump();
    }
    if (name == "chooseAudioFiles" && async) { chooseAudioFiles(async); return {}; }
    if (name == "openScene" && async) { openScene(async); return {}; }
    if (name == "saveScene" && async) { saveScene(arg, async); return {}; }
    if (name == "bounce" && async) { bounce(arg, async); return {}; }
    if (name == "showAudioSettings") { showAudioSettings(); return ""; }
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
    const char* names[] = {"analyze", "setScene", "transport", "setOutput", "info", "audioInfo", "chooseAudioFiles",
                           "openScene", "saveScene", "bounce", "showAudioSettings", "startupScene"};
    for (const char* n : names) {
        const std::string name = n;
        o = o.withNativeFunction(juce::Identifier(n), [this, name](const juce::Array<juce::var>& args, Completion done) {
            const std::string arg = args.isEmpty() ? std::string() : args[0].toString().toStdString();
            auto finish = [done](std::string r) { done(juce::var(juce::String(r))); };
            const bool isAsync = name == "chooseAudioFiles" || name == "openScene" || name == "saveScene" || name == "bounce";
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

void Bridge::chooseAudioFiles(std::function<void(std::string)> done) {
    chooser_ = std::make_unique<juce::FileChooser>("Add audio files as layers", lastDir_, kAudioWildcard);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [this, done](const juce::FileChooser& fc) {
                              json out = json::array();
                              for (const auto& f : fc.getResults()) {
                                  lastDir_ = f.getParentDirectory();
                                  out.push_back(audioInfoJson(session_.library().info(f.getFullPathName())));
                              }
                              done(out.dump());
                          });
}

void Bridge::openScene(std::function<void(std::string)> done) {
    chooser_ = std::make_unique<juce::FileChooser>("Open a scene", lastDir_, "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, done](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f == juce::File()) { done("null"); return; }
                              lastDir_ = f.getParentDirectory();
                              done(readScene(f));
                          });
}

// {path, scene (canonical, absolute audio paths), raw (the file as written)}.
std::string Bridge::readScene(const juce::File& f) {
    try {
        const std::string text = f.loadFileAsString().toStdString();
        json raw = json::parse(text);
        json scene = json::parse(sp::sceneToJson(sp::sceneFromJson(text)));
        resolveAudioPaths(scene, f.getParentDirectory());
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
            if (!f.replaceWithText(juce::String(scene.dump(2)) + "\n")) throw std::runtime_error("Cannot write " + f.getFullPathName().toStdString());
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
    const std::string name = a["scene"].value("name", "scene");
    chooser_ = std::make_unique<juce::FileChooser>("Save scene", lastDir_.getChildFile(juce::File::createLegalFileName(name) + ".json"), "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [write, done](const juce::FileChooser& fc) mutable {
                              auto f = fc.getResult();
                              if (f == juce::File()) { done("null"); return; }
                              if (!f.hasFileExtension("json")) f = f.withFileExtension("json");
                              write(f);
                          });
}

void Bridge::bounce(const std::string& arg, std::function<void(std::string)> done) {
    const json a = json::parse(arg);
    OutputSetup o;
    o.mode = modeFrom(a.value("mode", "binaural"));
    o.layout = juce::String(a.value("layout", "7.1.4"));
    const double start = a.value("start", 0.0), end = a.value("end", 30.0), rate = a.value("sampleRate", 48000.0);
    const juce::String suffix = o.mode == sp::OutputMode::Binaural ? "binaural" : o.mode == sp::OutputMode::Ambisonics ? "ambix" : o.layout;
    chooser_ = std::make_unique<juce::FileChooser>("Bounce to WAV", lastDir_.getChildFile("bounce_" + suffix + ".wav"), "*.wav");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, o, start, end, rate, done](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f == juce::File()) { done("null"); return; }
                              if (!f.hasFileExtension("wav")) f = f.withFileExtension("wav");
                              lastDir_ = f.getParentDirectory();
                              session_.bounce(f, o, start, end, rate, [](float) {},
                                              [this, f](juce::String error) {
                                                  if (error.isEmpty()) sendMessage("Bounced " + f.getFileName(), "info");
                                                  else sendMessage("Bounce failed: " + error, "error");
                                              });
                              done(json{{"path", f.getFullPathName().toStdString()}}.dump());
                          });
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
