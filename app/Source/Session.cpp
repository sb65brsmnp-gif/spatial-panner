#include "Session.h"

#include <cmath>
#include <thread>

#include "sp/SceneJson.h"

namespace spapp {

namespace {
constexpr double kTailSeconds = 3.0;    // reverb tail kept after pause
constexpr double kRampSeconds = 0.005;  // input fade on play/pause
constexpr double kFadeSeconds = 0.04;   // crossfade between programs
}  // namespace

// Per-layer playback data, swapped as a whole by patches.
struct LayerData {
    std::vector<std::shared_ptr<const LayerAudio>> audio;
    std::vector<std::vector<std::shared_ptr<const LayerAudio>>> files;  // an Ambisonic layer's one-file-per-channel set, else empty
    std::vector<juce::int64> start;   // samples
    std::vector<char> loop;
    std::vector<float> meterGain;     // level and mute, so the meters show what the layer contributes
    std::vector<int> channels;        // renderer inputs per layer (1, 2, or an Ambisonic layer's 4 / 9 / 16), in order
    std::vector<char> ambisonic;

    // The samples feeding input channel `c` of layer `i`, or null for silence.
    const std::vector<float>* source(size_t i, int c) const {
        if (!files[i].empty()) {
            const LayerAudio* f = static_cast<size_t>(c) < files[i].size() ? files[i][static_cast<size_t>(c)].get() : nullptr;
            return f && f->numChannels() > 0 ? &f->channel(0) : nullptr;
        }
        const LayerAudio* a = audio[i].get();
        if (!a || a->numChannels() == 0) return nullptr;
        return ambisonic[i] ? a->exactChannel(c) : &a->channel(c);
    }
};

struct Program {
    std::shared_ptr<sp::Renderer> renderer;  // shared only with the builder until published
    double sampleRate = 48000;
    int maxBlock = 512;
    double duration = 0;
    LayerData layers;
    std::vector<std::vector<float>> in;
    std::vector<const float*> inPtrs;
    std::vector<std::vector<float>> out;
    std::vector<float*> outPtrs;

    void allocate() {
        const size_t nl = static_cast<size_t>(renderer->numInputs()), no = static_cast<size_t>(renderer->numOutputs());
        in.assign(nl, std::vector<float>(static_cast<size_t>(maxBlock), 0.0f));
        out.assign(no, std::vector<float>(static_cast<size_t>(maxBlock), 0.0f));
        inPtrs.resize(nl);
        outPtrs.resize(no);
        for (size_t i = 0; i < nl; ++i) inPtrs[i] = in[i].data();
        for (size_t i = 0; i < no; ++i) outPtrs[i] = out[i].data();
    }
};

struct Patch {
    Program* target = nullptr;
    std::unique_ptr<sp::SceneUpdate> update;
    double duration = 0;
    LayerData layers;
};

// Delay-line headroom so layers and paths can be moved around while editing
// without forcing a new renderer.
static float editHeadroomDistance(const sp::Scene& s) {
    float r = 5.0f;
    auto grow = [&](const sp::Vec3& p) { r = std::max(r, p.length()); };
    for (const auto& l : s.layers) {
        grow(l.position);
        // A layer's path carries it from its place by the path's shape.
        if (!l.motion.hasPath() || l.motion.path.segments.front().points.empty()) continue;
        const sp::Vec3 p0 = l.motion.path.segments.front().points.front();
        for (const auto& seg : l.motion.path.segments)
            for (const auto& q : seg.points) grow(l.position + (q - p0));
    }
    for (const auto& p : s.listener.paths)
        for (const auto& seg : p.segments)
            for (const auto& q : seg.points) grow(q);
    grow(s.listener.staticPosition);
    if (s.room.type == sp::RoomType::Box) { grow(s.room.minCorner()); grow(s.room.maxCorner()); }
    const int order = s.room.type == sp::RoomType::Box ? std::max(1, s.room.reflectionOrder) : 1;
    return std::min(2.0f * r * static_cast<float>(order + 1) * 1.2f + 50.0f, 1000.0f);
}

Session::Session(juce::AudioDeviceManager& d) : devices_(d) {
    library_.onLoaded = [this] { tryUpdate(); };
    devices_.addAudioCallback(this);
    startTimerHz(20);
}

Session::~Session() {
    stopTimer();
    devices_.removeAudioCallback(this);
    *alive_ = false;
    if (bounceThread_) bounceThread_->stopThread(10000);
    builder_.removeAllJobs(true, 10000);
    {
        const juce::SpinLock::ScopedLockType l(audioLock_);
        handleCommands();
        if (current_) garbage_.push({current_, nullptr});
        if (fading_) garbage_.push({fading_, nullptr});
        current_ = fading_ = nullptr;
    }
    collectGarbage();
}

// ------------------------------------------------------------ message thread

juce::File Session::hrtfFile() {
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const juce::File candidates[] = {
        exe.getParentDirectory().getParentDirectory().getChildFile("Resources/sadie_d1.sofa"),  // macOS bundle
        exe.getParentDirectory().getChildFile("sadie_d1.sofa"),
#ifdef SP_DEFAULT_HRTF
        juce::File(SP_DEFAULT_HRTF),
#endif
    };
    for (const auto& f : candidates)
        if (f.existsAsFile()) return f;
    return {};
}

sp::RenderConfig Session::makeConfig(const OutputSetup& out, double rate) {
    sp::RenderConfig cfg;
    cfg.sampleRate = rate;
    cfg.mode = out.mode;
    cfg.ambisonicsOrder = 3;
    cfg.layout = sp::SpeakerLayout::preset(out.layout.toStdString());
    if (cfg.layout.numChannels() == 0) cfg.layout = sp::SpeakerLayout::preset("7.1.4");
    cfg.hrtfPath = hrtfFile().getFullPathName().toStdString();
    return cfg;
}

void Session::setScene(const sp::Scene& scene, double duration) {
    scene_ = scene;
    duration_ = duration;
    ++revision_;
    haveScene_ = true;
    tryUpdate();
}

void Session::setOutput(const OutputSetup& out) {
    if (out == output_) return;
    output_ = out;
    if (!building_) startBuild();  // a build in flight checks the output when it finishes
}

void Session::fillLayerData(LayerData& d, const sp::Scene& s, double rate) {
    const size_t n = s.layers.size();
    d.audio.resize(n);
    d.start.resize(n);
    d.loop.resize(n);
    d.meterGain.resize(n);
    d.channels.resize(n);
    d.files.assign(n, {});
    d.ambisonic.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& l = s.layers[i];
        d.ambisonic[i] = sp::isAmbisonic(l) ? 1 : 0;
        if (d.ambisonic[i] && !l.audioFiles.empty()) {
            for (const auto& f : l.audioFiles) d.files[i].push_back(library_.get(juce::String(f)));
        } else {
            d.audio[i] = library_.get(juce::String(l.audioFile));
        }
        d.start[i] = static_cast<juce::int64>(std::llround(l.startTime * rate));
        d.loop[i] = l.loop ? 1 : 0;
        d.meterGain[i] = l.mute ? 0.0f : sp::dbToGain(l.levelDb);
        d.channels[i] = sp::layerInputs(l);
    }
}

void Session::tryUpdate() {
    if (!haveScene_ || building_) return;
    const double rate = deviceRate_.load();
    if (published_ && std::abs(published_->sampleRate - rate) < 0.5 &&
        published_->renderer->numInputs() == sp::inputChannels(scene_)) {
        if (auto u = published_->renderer->prepareUpdate(scene_)) {
            auto* p = new Patch;
            p->target = published_;
            p->update = std::move(u);
            p->duration = duration_;
            fillLayerData(p->layers, scene_, rate);
            if (!toAudio_.push({nullptr, p})) delete p;
            return;
        }
    }
    startBuild();
}

void Session::startBuild() {
    if (!haveScene_) return;
    building_ = true;
    ++buildsStarted_;
    const int rev = revision_;
    const sp::Scene scene = scene_;
    const OutputSetup out = output_;
    const double rate = deviceRate_.load(), dur = duration_;
    auto alive = alive_;
    builder_.addJob([this, rev, scene, out, rate, dur, alive] {
        std::shared_ptr<sp::Renderer> r;
        juce::String err;
        try {
            auto cfg = makeConfig(out, rate);
            if (cfg.mode == sp::OutputMode::Binaural && cfg.hrtfPath.empty()) throw std::runtime_error("HRTF file (sadie_d1.sofa) not found next to the app");
            cfg.maxDistance = editHeadroomDistance(scene);
            // Live playback: ray-trace reflections on a worker thread, never in
            // the audio callback, and leave cores for the audio and the UI.
            cfg.steam.asyncSimulation = true;
            cfg.steam.threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
            r = std::make_shared<sp::Renderer>(scene, cfg, dur > 0 ? dur : 600.0);
        } catch (const std::exception& e) {
            err = e.what();
        }
        juce::MessageManager::callAsync([this, rev, out, rate, dur, r, err, alive] {
            if (!*alive) return;
            building_ = false;
            if (err.isNotEmpty()) {
                if (onMessage) onMessage("Audio engine: " + err, true);
                if (rev != revision_) tryUpdate();
                return;
            }
            // Output or rate changed while building: start again.
            if (!(out == output_) || std::abs(rate - deviceRate_.load()) > 0.5) { startBuild(); return; }
            auto prog = std::make_unique<Program>();
            prog->renderer = r;
            prog->sampleRate = rate;
            prog->maxBlock = std::max(256, deviceBlock_.load());
            prog->duration = dur;
            prog->allocate();
            const sp::Scene* sceneForData = nullptr;
            if (rev != revision_) {
                // Edits arrived while building. Bring the new renderer up to
                // date before anyone plays it (it is not shared yet, so this
                // is safe here), or build again if they need a new one.
                auto u = prog->renderer->prepareUpdate(scene_);
                if (!u) { startBuild(); return; }
                prog->renderer->applyUpdate(*u);
                prog->duration = duration_;
            }
            sceneForData = &scene_;
            fillLayerData(prog->layers, *sceneForData, rate);
            Program* p = prog.release();
            if (!toAudio_.push({p, nullptr})) { delete p; return; }
            published_ = p;
        });
    });
}

// ------------------------------------------------------------------ bounce

namespace {

class BounceThread : public juce::Thread {
public:
    std::function<void()> body;
    BounceThread() : juce::Thread("bounce") {}
    void run() override { body(); }
};

}  // namespace

void Session::bounce(const juce::File& file, OutputSetup out, double start, double end, double rate,
                     std::function<void(float)> progress, std::function<void(juce::String)> done) {
    if (bouncing()) { done("A bounce is already running"); return; }
    if (bounceThread_) bounceThread_->stopThread(1000);
    auto t = std::make_unique<BounceThread>();
    auto* raw = t.get();
    const sp::Scene scene = scene_;
    const double duration = duration_;
    auto alive = alive_;
    const juce::File partial = file.getSiblingFile("." + file.getFileNameWithoutExtension() + ".partial.wav");
    t->body = [raw, scene, duration, out, start, end, rate, file, partial, progress, done, alive] {
        juce::String error;
        auto report = [alive](std::function<void()> f) {
            juce::MessageManager::callAsync([alive, f] { if (*alive) f(); });
        };
        try {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            LayerData audio;
            const size_t nl = scene.layers.size();
            audio.audio.resize(nl);
            audio.files.assign(nl, {});
            audio.ambisonic.resize(nl);
            for (size_t i = 0; i < nl; ++i) {
                const auto& l = scene.layers[i];
                juce::String err;
                audio.ambisonic[i] = sp::isAmbisonic(l) ? 1 : 0;
                if (audio.ambisonic[i] && !l.audioFiles.empty()) {
                    for (const auto& f : l.audioFiles) {
                        audio.files[i].push_back(AudioLibrary::decode(fm, juce::String(f), rate, err));
                        if (err.isNotEmpty()) break;
                    }
                } else if (!l.audioFile.empty()) {
                    audio.audio[i] = AudioLibrary::decode(fm, juce::String(l.audioFile), rate, err);
                }
                if (err.isNotEmpty()) throw std::runtime_error((juce::String(l.name) + ": " + err).toStdString());
            }
            const double t1 = std::max(end, start + 0.1);
            sp::Renderer r(scene, makeConfig(out, rate), std::max(duration, t1));
            const int nOut = r.numOutputs(), lat = r.latencySamples(), block = 1024;
            // Start a few seconds early so delays and the room are already full at `start`.
            const auto pre = static_cast<juce::int64>(std::min(start, 3.0) * rate);
            const auto first = static_cast<juce::int64>(start * rate) - pre;
            const auto total = static_cast<juce::int64>((t1 - start) * rate);
            // Render into a hidden file next to the target and move it into
            // place at the end, so the chosen name only ever holds a complete
            // bounce (a failed or cancelled one leaves nothing behind).
            partial.deleteFile();
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream>(partial);
            if (static_cast<juce::FileOutputStream*>(os.get())->failedToOpen()) throw std::runtime_error("Cannot write in " + file.getParentDirectory().getFullPathName().toStdString());
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(nOut).withBitsPerSample(24));
            if (!writer) throw std::runtime_error("Cannot create a WAV writer for " + std::to_string(nOut) + " channels");
            std::vector<std::vector<float>> in(static_cast<size_t>(r.numInputs()), std::vector<float>(block)), o(static_cast<size_t>(nOut), std::vector<float>(block));
            std::vector<const float*> ip(in.size());
            std::vector<float*> op(o.size());
            for (size_t i = 0; i < in.size(); ++i) ip[i] = in[i].data();
            for (size_t i = 0; i < o.size(); ++i) op[i] = o[i].data();
            juce::int64 pos = first, written = 0, skip = pre + lat;
            float lastReported = -1;
            while (written < total) {
                if (raw->threadShouldExit()) throw std::runtime_error("Bounce cancelled");
                size_t input = 0;
                for (size_t i = 0; i < scene.layers.size(); ++i) {
                    const auto& l = scene.layers[i];
                    const auto st = static_cast<juce::int64>(std::llround(l.startTime * rate));
                    const int nch = sp::layerInputs(l);
                    for (int c = 0; c < nch; ++c, ++input) {
                        const std::vector<float>* smp = audio.source(i, c);
                        for (int k = 0; k < block; ++k) {
                            juce::int64 idx = pos + k - st;
                            float v = 0;
                            if (smp && !smp->empty() && idx >= 0) {
                                const auto size = static_cast<juce::int64>(smp->size());
                                if (l.loop) idx %= size;
                                if (idx < size) v = (*smp)[static_cast<size_t>(idx)];
                            }
                            in[input][static_cast<size_t>(k)] = v;
                        }
                    }
                }
                r.process(ip.data(), op.data(), block, static_cast<double>(pos) / rate);
                pos += block;
                int from = 0;
                if (skip > 0) { from = static_cast<int>(std::min<juce::int64>(skip, block)); skip -= from; }
                const int n = static_cast<int>(std::min<juce::int64>(block - from, total - written));
                if (n > 0) {
                    std::vector<const float*> wp(o.size());
                    for (size_t c = 0; c < o.size(); ++c) wp[c] = o[c].data() + from;
                    writer->writeFromFloatArrays(wp.data(), nOut, n);
                    written += n;
                }
                const float pr = static_cast<float>(written) / static_cast<float>(std::max<juce::int64>(1, total));
                if (pr - lastReported > 0.01f) { lastReported = pr; report([progress, pr] { progress(pr); }); }
            }
            writer.reset();
            if (!partial.moveFileTo(file)) throw std::runtime_error("Cannot write " + file.getFullPathName().toStdString());
        } catch (const std::exception& e) {
            error = e.what();
            partial.deleteFile();
        }
        report([done, error] { done(error); });
    };
    t->startThread();
    bounceThread_ = std::move(t);
}

void Session::transport(Transport cmd, double value) {
    commands_.push({cmd, value});
    if (cmd == Transport::Play) playing_ = true;
    if (cmd == Transport::Pause || cmd == Transport::Stop) playing_ = false;
    if (cmd == Transport::Stop) time_ = 0;
    if (cmd == Transport::Seek) time_ = value;
}

void Session::timerCallback() {
    if (!audioRunning_.load()) {
        // No device is calling back: play the audio thread's part here so
        // edits, transport and garbage keep flowing.
        const juce::SpinLock::ScopedLockType l(audioLock_);
        handleCommands();
    }
    collectGarbage();
}

void Session::collectGarbage() {
    Garbage g;
    while (garbage_.pop(g)) {
        if (g.program == published_) published_ = nullptr;
        delete g.program;
        delete g.patch;
    }
}

Session::Tick Session::tick() {
    Tick t;
    t.time = time_.load();
    t.playing = playing_.load();
    for (size_t i = 0; i < t.pose.size(); ++i) t.pose[i] = pose_[i].load();
    const size_t n = std::min<size_t>(scene_.layers.size(), kMaxMeters);
    t.metersDb.resize(n);
    for (size_t i = 0; i < n; ++i) t.metersDb[i] = sp::gainToDb(meters_[i].exchange(0.0f));
    t.cpu = cpu_.load();
    return t;
}

juce::String Session::statusText() const {
    if (!devices_.getCurrentAudioDevice()) return "No audio device";
    if (building_) return "Preparing the renderer";
    if (library_.anyLoading()) return "Loading audio";
    return playing_.load() ? "Playing" : "Stopped";
}

int Session::outputChannels() const {
    auto* d = devices_.getCurrentAudioDevice();
    return d ? d->getActiveOutputChannels().countNumberOfSetBits() : 0;
}

juce::String Session::deviceName() const {
    auto* d = devices_.getCurrentAudioDevice();
    return d ? d->getName() : juce::String("No audio device");
}

// ------------------------------------------------------------- audio thread

void Session::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    const double rate = device->getCurrentSampleRate();
    deviceBlock_ = device->getCurrentBufferSizeSamples();
    const bool changed = std::abs(rate - deviceRate_.load()) > 0.5;
    deviceRate_ = rate;
    audioRunning_ = true;
    if (changed) {
        auto alive = alive_;
        juce::MessageManager::callAsync([this, rate, alive] {
            if (!*alive) return;
            library_.setTargetRate(rate);
            if (!building_) startBuild();
        });
    }
}

void Session::audioDeviceStopped() { audioRunning_ = false; }

void Session::retire(Program* p) {
    if (p && !garbage_.push({p, nullptr})) { /* queue full: leak rather than free on the audio thread */ }
}

void Session::resetRenderers() {
    if (current_) current_->renderer->reset();
    retire(fading_);
    fading_ = nullptr;
}

void Session::handleCommands() {
    Message m;
    while (toAudio_.pop(m)) {
        if (m.program) {
            const bool audible = audioPlaying_ || inGain_ > 0 || tailLeft_ > 0;
            if (!current_) {
                current_ = m.program;
            } else if (!audible) {
                retire(current_);
                current_ = m.program;
            } else {
                retire(fading_);
                fading_ = current_;
                current_ = m.program;
                fadePos_ = 0;
            }
        } else if (m.patch) {
            Patch& p = *m.patch;
            if (p.target == current_ && current_) {
                current_->renderer->applyUpdate(*p.update);
                std::swap(current_->layers, p.layers);
                current_->duration = p.duration;
            }
            garbage_.push({nullptr, m.patch});
        }
    }
    Command c;
    const double sr = current_ ? current_->sampleRate : deviceRate_.load();
    while (commands_.pop(c)) {
        switch (c.kind) {
            case Transport::Play:
                if (current_ && current_->duration > 0 && pos_ >= static_cast<juce::int64>(current_->duration * sr)) {
                    pos_ = 0;
                    resetRenderers();
                }
                audioPlaying_ = true;
                break;
            case Transport::Pause:
                if (audioPlaying_) tailLeft_ = static_cast<juce::int64>(kTailSeconds * sr);
                audioPlaying_ = false;
                break;
            case Transport::Stop:
                audioPlaying_ = false;
                inGain_ = 0;
                tailLeft_ = 0;
                pos_ = 0;
                resetRenderers();
                break;
            case Transport::Seek: {
                const auto np = static_cast<juce::int64>(std::max(0.0, c.value) * sr);
                if (np != pos_) {
                    pos_ = np;
                    resetRenderers();
                }
                break;
            }
            case Transport::Loop:
                loop_ = c.value > 0.5;
                break;
        }
    }
    time_ = static_cast<double>(pos_) / sr;
}

void Session::fillInputs(Program& p, int n, float gainStart, float gainStep, bool metering) {
    const auto& L = p.layers;
    const size_t nl = L.channels.size();
    size_t input = 0;
    for (size_t i = 0; i < nl && input < p.in.size(); ++i) {
        const int nch = L.channels[i];
        float peak = 0;
        for (int c = 0; c < nch && input < p.in.size(); ++c, ++input) {
            float* dst = p.in[input].data();
            const std::vector<float>* src = L.source(i, c);
            if (!src || src->empty() || (gainStart <= 0 && gainStep <= 0)) {
                std::fill(dst, dst + n, 0.0f);
                continue;
            }
            const auto& smp = *src;
            const auto size = static_cast<juce::int64>(smp.size());
            const juce::int64 base = pos_ - L.start[i];
            float g = gainStart;
            for (int k = 0; k < n; ++k) {
                juce::int64 idx = base + k;
                float v = 0;
                if (idx >= 0) {
                    if (L.loop[i]) idx %= size;
                    if (idx < size) v = smp[static_cast<size_t>(idx)];
                }
                g += gainStep;
                v *= g;
                dst[k] = v;
                peak = std::max(peak, std::abs(v));
            }
        }
        if (metering && i < static_cast<size_t>(kMaxMeters)) {
            const float m = peak * L.meterGain[i];
            float prev = meters_[i].load(std::memory_order_relaxed);
            while (m > prev && !meters_[i].compare_exchange_weak(prev, m)) {}
        }
    }
    for (; input < p.in.size(); ++input) std::fill(p.in[input].begin(), p.in[input].begin() + n, 0.0f);
}

void Session::renderChunk(float* const* out, int numOut, int offset, int n) {
    Program* p = current_;
    const double sr = p->sampleRate;
    const bool active = audioPlaying_ || inGain_ > 0 || tailLeft_ > 0 || fading_;
    if (!active) return;

    const float target = audioPlaying_ ? 1.0f : 0.0f;
    const float maxStep = static_cast<float>(1.0 / (kRampSeconds * sr));
    const float g0 = inGain_;
    const float g1 = target > g0 ? std::min(target, g0 + maxStep * n) : std::max(target, g0 - maxStep * n);
    const float step = (g1 - g0) / static_cast<float>(n);
    const double t = static_cast<double>(pos_) / sr;

    fillInputs(*p, n, g0, step, true);
    p->renderer->process(p->inPtrs.data(), p->outPtrs.data(), n, t);
    const int nc = std::min(numOut, static_cast<int>(p->out.size()));

    if (fading_) {
        Program* f = fading_;
        fillInputs(*f, n, g0, step, false);
        f->renderer->process(f->inPtrs.data(), f->outPtrs.data(), n, t);
        const int fadeLen = static_cast<int>(kFadeSeconds * sr);
        const int nf = std::min(numOut, static_cast<int>(f->out.size()));
        for (int k = 0; k < n; ++k) {
            const float w = std::min(1.0f, static_cast<float>(fadePos_ + k) / static_cast<float>(fadeLen));
            const float gi = std::sin(w * juce::MathConstants<float>::halfPi), go = std::cos(w * juce::MathConstants<float>::halfPi);
            for (int c = 0; c < nc; ++c) p->out[static_cast<size_t>(c)][static_cast<size_t>(k)] *= gi;
            for (int c = 0; c < nf; ++c) out[c][offset + k] += f->out[static_cast<size_t>(c)][static_cast<size_t>(k)] * go;
        }
        fadePos_ += n;
        if (fadePos_ >= fadeLen) { retire(fading_); fading_ = nullptr; }
    }
    for (int c = 0; c < nc; ++c) {
        const float* src = p->out[static_cast<size_t>(c)].data();
        float* dst = out[c] + offset;
        for (int k = 0; k < n; ++k) dst[k] += src[k];
    }

    // Pose for the editor.
    const sp::Pose pose = p->renderer->lastPose();
    float yaw, pitch, roll;
    pose.orientation.toYawPitchRoll(yaw, pitch, roll);
    const float vals[8] = {pose.position.x, pose.position.y, pose.position.z, sp::radToDeg(yaw), sp::radToDeg(pitch),
                           sp::radToDeg(roll), 0.0f, pose.velocity.length()};
    for (int i = 0; i < 8; ++i) pose_[static_cast<size_t>(i)].store(vals[i], std::memory_order_relaxed);

    // Advance the transport.
    inGain_ = g1;
    if (audioPlaying_ || g0 > 0) {
        pos_ += n;
        if (p->duration > 0 && pos_ >= static_cast<juce::int64>(p->duration * sr)) {
            if (loop_) {
                pos_ = 0;
                resetRenderers();
            } else {
                pos_ = static_cast<juce::int64>(p->duration * sr);
                audioPlaying_ = false;
                inGain_ = 0;
                tailLeft_ = static_cast<juce::int64>(kTailSeconds * sr);
                playing_ = false;
            }
        }
    } else if (tailLeft_ > 0) {
        tailLeft_ -= n;
    }
    time_ = static_cast<double>(pos_) / sr;
}

void Session::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) {
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (int c = 0; c < numOut; ++c)
        if (out[c]) juce::FloatVectorOperations::clear(out[c], numSamples);
    const juce::SpinLock::ScopedTryLockType lock(audioLock_);
    if (!lock.isLocked()) return;
    handleCommands();
    if (!current_ || std::abs(current_->sampleRate - deviceRate_.load()) > 0.5) return;
    for (int done = 0; done < numSamples;) {
        const int n = std::min(current_->maxBlock, numSamples - done);
        renderChunk(out, numOut, done, n);
        done += n;
    }
    const double secs = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
    const float load = static_cast<float>(secs / (numSamples / deviceRate_.load()));
    cpu_ = cpu_.load() * 0.9f + load * 0.1f;
}

}  // namespace spapp
