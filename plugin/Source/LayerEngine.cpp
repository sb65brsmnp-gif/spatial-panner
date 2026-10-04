#include "LayerEngine.h"

#include <cmath>
#include <vector>

namespace spplug {

namespace {

constexpr double kFadeSeconds = 0.04;
constexpr int kSubBlock = 32;   // RenderConfig::subBlockSize default

bool sameLayout(const sp::SpeakerLayout& a, const sp::SpeakerLayout& b) {
    if (a.speakers.size() != b.speakers.size()) return false;
    for (size_t i = 0; i < a.speakers.size(); ++i) {
        const auto& x = a.speakers[i];
        const auto& y = b.speakers[i];
        if (x.azimuthDeg != y.azimuthDeg || x.elevationDeg != y.elevationDeg || x.lfe != y.lfe || x.distance != y.distance) return false;
    }
    return true;
}

// Delay-line headroom so layers and paths can be moved while editing (and by
// the position-offset parameters) without a new renderer. Same rule as the app.
float headroomDistance(const sp::Scene& s) {
    float r = 5.0f;
    auto grow = [&](const sp::Vec3& p) { r = std::max(r, p.length()); };
    for (const auto& l : s.layers) grow(l.position);
    for (const auto& p : s.listener.paths)
        for (const auto& seg : p.segments)
            for (const auto& q : seg.points) grow(q);
    grow(s.listener.staticPosition);
    if (s.room.type == sp::RoomType::Box) { grow(s.room.minCorner()); grow(s.room.maxCorner()); }
    r += 35.0f;  // position offset parameters reach +-20 m per axis
    const int order = s.room.type == sp::RoomType::Box ? std::max(1, s.room.reflectionOrder) : 1;
    return std::min(2.0f * r * static_cast<float>(order + 1) * 1.2f + 50.0f, 1500.0f);
}

sp::Scene renderScene(const sp::Scene& s) {
    sp::Scene r = s;
    r.listener.positionMode = sp::PositionMode::AlongPath;
    return r;
}

// Single-producer/single-consumer ring of pointers (audio -> control side).
template <typename T, size_t N>
class Spsc {
public:
    bool push(T v) {
        const size_t w = w_.load(std::memory_order_relaxed), next = (w + 1) % N;
        if (next == r_.load(std::memory_order_acquire)) return false;
        items_[w] = v;
        w_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const size_t r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire)) return false;
        v = items_[r];
        r_.store((r + 1) % N, std::memory_order_release);
        return true;
    }
private:
    std::array<T, N> items_{};
    std::atomic<size_t> w_{0}, r_{0};
};

juce::ThreadPool& buildPool() {
    // Shared by every instance in the process: a project with many tracks
    // builds several renderers at once (an HRTF load is ~0.4 s each).
    static juce::ThreadPool pool(juce::ThreadPoolOptions{}
                                     .withThreadName("Spatial Panner builder")
                                     .withNumberOfThreads(std::max(2, juce::SystemStats::getNumCpus() / 2)));
    return pool;
}

}  // namespace

bool EngineConfig::operator==(const EngineConfig& o) const {
    return sampleRate == o.sampleRate && maxBlock == o.maxBlock && mode == o.mode && render == o.render &&
           hrtfPath == o.hrtfPath && offline == o.offline &&
           (mode != sp::OutputMode::Speakers || sameLayout(layout, o.layout));
}

namespace {
// Whether a program built for `built` can serve `wanted`: the offline flag
// only matters to a renderer that uses Steam Audio.
bool serves(const EngineConfig& built, bool builtUsesSteam, const EngineConfig& wanted) {
    EngineConfig w = wanted;
    if (!builtUsesSteam) w.offline = built.offline;
    return built == w;
}
}  // namespace

struct LayerEngine::Program {
    EngineConfig cfg;
    std::unique_ptr<sp::Renderer> renderer;
    ListenerTimeline timeline;
    int numOut = 0;
    int phase = 0;   // frames into the renderer's current sub-block
    std::vector<std::vector<float>> out;
    std::vector<float*> outPtrs;
    sp::ListenerControls lastControls;
    double lastTime = 0;
    bool usesSteam = false;
    float tail = 3.0f;   // seconds of output after the input stops
};

struct LayerEngine::Patch {
    Program* target = nullptr;
    std::unique_ptr<sp::SceneUpdate> update;
    ListenerTimeline timeline;
};

struct LayerEngine::Inbox {
    struct Msg { Program* program = nullptr; Patch* patch = nullptr; };
    std::mutex m;
    std::array<Msg, 64> ring{};
    size_t head = 0, count = 0;
    // Results of the newest build, for the control side.
    Program* published = nullptr;
    EngineConfig publishedConfig;
    uint64_t publishedRevision = 0;
    bool buildDone = false;
    std::string buildError;
    juce::WaitableEvent posted;
    Spsc<Msg, 256> garbage;   // audio thread -> control side

    bool push(Msg msg) {
        if (count == ring.size()) return false;
        ring[(head + count) % ring.size()] = msg;
        ++count;
        return true;
    }
};

LayerEngine::LayerEngine() : inbox_(std::make_shared<Inbox>()) {
    for (auto& p : pose_) p.store(0);
}

LayerEngine::~LayerEngine() {
    std::lock_guard<std::mutex> l(lock_);
    // Builds still running hold their own reference to the inbox and post into
    // it; whatever they post is freed with it.
    {
        std::lock_guard<std::mutex> il(inbox_->m);
        for (size_t i = 0; i < inbox_->count; ++i) {
            const auto& m = inbox_->ring[(inbox_->head + i) % inbox_->ring.size()];
            delete m.program;
            delete m.patch;
        }
        inbox_->count = 0;
        inbox_->published = nullptr;
    }
    collectGarbage();
    delete current_;
    delete fading_;
}

// ------------------------------------------------------------- control side

void LayerEngine::configure(const EngineConfig& cfg) {
    {
        std::lock_guard<std::mutex> l(lock_);
        if (haveConfig_ && cfg == config_) return;
        config_ = cfg;
        haveConfig_ = true;
        ++revision_;
    }
    update();
}

void LayerEngine::setScene(const sp::Scene& scene) {
    {
        std::lock_guard<std::mutex> l(lock_);
        scene_ = scene;
        haveScene_ = true;
        ++revision_;
    }
    update();
}

std::string LayerEngine::lastError() const {
    std::lock_guard<std::mutex> l(lock_);
    return error_;
}

void LayerEngine::collectGarbage() {
    Inbox::Msg m;
    while (inbox_->garbage.pop(m)) {
        delete m.program;
        delete m.patch;
    }
}

void LayerEngine::update() {
    std::lock_guard<std::mutex> l(lock_);
    collectGarbage();
    if (building_) {
        std::lock_guard<std::mutex> il(inbox_->m);
        if (!inbox_->buildDone) return;
        building_ = false;
        inbox_->buildDone = false;
        error_ = inbox_->buildError;
    }
    if (!haveConfig_ || !haveScene_) return;

    Program* pub = nullptr;
    EngineConfig pubCfg;
    uint64_t pubRev = 0;
    {
        std::lock_guard<std::mutex> il(inbox_->m);
        pub = inbox_->published;
        pubCfg = inbox_->publishedConfig;
        pubRev = inbox_->publishedRevision;
    }
    if (pub && pubRev == revision_) return;   // up to date
    if (pub && serves(pubCfg, pub->usesSteam, config_)) {
        // Same output setup: try a live update of the published renderer.
        std::unique_ptr<sp::SceneUpdate> u;
        if (pub->renderer) u = pub->renderer->prepareUpdate(renderScene(scene_));
        if (u || !pub->renderer) {
            auto* patch = new Patch;
            patch->target = pub;
            patch->update = std::move(u);
            patch->timeline = ListenerTimeline(scene_);
            std::lock_guard<std::mutex> il(inbox_->m);
            if (inbox_->push({nullptr, patch})) {
                inbox_->publishedRevision = revision_;
                inbox_->posted.signal();
            } else {
                delete patch;
            }
            return;
        }
    }
    startBuildLocked();
}

void LayerEngine::startBuildLocked() {
    building_ = true;
    const EngineConfig cfg = config_;
    const sp::Scene scene = scene_;
    const uint64_t rev = revision_;
    auto inbox = inbox_;
    buildPool().addJob([cfg, scene, rev, inbox] {
        std::unique_ptr<Program> p;
        std::string err;
        try {
            p = std::make_unique<Program>();
            p->cfg = cfg;
            p->timeline = ListenerTimeline(scene);
            if (cfg.render) {
                sp::RenderConfig rc;
                rc.sampleRate = cfg.sampleRate;
                rc.mode = cfg.mode;
                if (cfg.mode == sp::OutputMode::Speakers) rc.layout = cfg.layout;
                rc.hrtfPath = cfg.hrtfPath;
                if (rc.mode == sp::OutputMode::Binaural && rc.hrtfPath.empty())
                    throw std::runtime_error("the HRTF file (sadie_d1.sofa) is missing from the plugin bundle");
                const sp::Scene rs = renderScene(scene);
                rc.maxDistance = headroomDistance(rs);
                // Ray tracing (mesh rooms, objects) runs on Steam Audio's own
                // worker in real time, never on the audio thread. One tracing
                // thread per instance: a session has one instance per track.
                rc.steam.asyncSimulation = !cfg.offline;
                rc.steam.threads = 1;
                p->renderer = std::make_unique<sp::Renderer>(rs, rc, 1.0);
                const auto st = p->renderer->stats();
                p->usesSteam = st.backend == sp::ReflectionsBackend::SteamAudio;
                const float decay = p->usesSteam ? st.irSeconds : st.reverbRt60Mid * 1.5f;
                p->tail = std::min(30.0f, decay + st.maxDistance / 343.0f + 0.1f);
                p->numOut = std::min(p->renderer->numOutputs(), kMaxOutputs);
                p->out.assign(static_cast<size_t>(p->numOut), std::vector<float>(static_cast<size_t>(std::max(cfg.maxBlock, 64)), 0.0f));
                p->outPtrs.resize(static_cast<size_t>(p->numOut));
                for (int c = 0; c < p->numOut; ++c) p->outPtrs[static_cast<size_t>(c)] = p->out[static_cast<size_t>(c)].data();
            }
        } catch (const std::exception& e) {
            err = e.what();
            p.reset();
        }
        std::lock_guard<std::mutex> il(inbox->m);
        inbox->buildError = err;
        inbox->buildDone = true;
        if (p) {
            Program* raw = p.release();
            if (inbox->push({raw, nullptr})) {
                inbox->published = raw;
                inbox->publishedConfig = cfg;
                inbox->publishedRevision = rev;
            } else {
                delete raw;
            }
        }
        inbox->posted.signal();
    });
}

bool LayerEngine::waitUntilCurrent(int timeoutMs) {
    const auto end = juce::Time::getMillisecondCounter() + static_cast<uint32_t>(timeoutMs);
    for (;;) {
        update();
        {
            std::lock_guard<std::mutex> l(lock_);
            std::lock_guard<std::mutex> il(inbox_->m);
            if (!building_ && haveConfig_ && haveScene_ &&
                ((inbox_->published && inbox_->publishedRevision == revision_) || !error_.empty()))
                return error_.empty();
        }
        if (juce::Time::getMillisecondCounter() > end) return false;
        inbox_->posted.wait(10);
    }
}

// --------------------------------------------------------------- audio side

void LayerEngine::retire(Program* p) {
    if (p && !inbox_->garbage.push({p, nullptr})) { /* queue full: leak rather than free here */ }
}

void LayerEngine::handleInbox(bool blocking) {
    std::unique_lock<std::mutex> il(inbox_->m, std::defer_lock);
    if (blocking) il.lock();
    else if (!il.try_lock()) return;
    while (inbox_->count > 0) {
        const Inbox::Msg m = inbox_->ring[inbox_->head];
        inbox_->head = (inbox_->head + 1) % inbox_->ring.size();
        --inbox_->count;
        if (m.program) {
            if (!current_) {
                current_ = m.program;
            } else {
                retire(fading_);
                fading_ = current_;
                current_ = m.program;
                fadePos_ = 0;
            }
            hasProgram_ = true;
            if (current_->renderer) tail_ = current_->tail;
            usesSteam_ = current_->usesSteam;
            offlineProgram_ = current_->cfg.offline;
        } else if (m.patch) {
            Patch& p = *m.patch;
            if (p.target && p.target == current_) {
                if (p.update && current_->renderer) current_->renderer->applyUpdate(*p.update);
                std::swap(current_->timeline, p.timeline);
            }
            if (!inbox_->garbage.push({nullptr, m.patch})) { /* leak */ }
        }
    }
}

void LayerEngine::drainInboxBlocking() { handleInbox(true); }

void LayerEngine::renderProgram(Program& p, const Block& b, int numOut) {
    const double sr = p.cfg.sampleRate;
    const SharedSession& s = *b.session;
    p.timeline.refresh(s, b.useHistory);
    if (!p.renderer) {
        p.lastControls = p.timeline.evaluate(s, b.time, b.playing, b.useHistory);
        p.lastTime = b.time;
        return;
    }
    p.renderer->setLayerControls(0, b.layer);
    const float* in[kMaxInputs] = {};
    float* out[kMaxOutputs];
    int done = 0;
    while (done < b.numFrames) {
        const int k = std::min(kSubBlock - p.phase, b.numFrames - done);
        const double t = b.time + done / sr;
        if (p.phase == 0) {
            // The renderer evaluates the pose once per sub-block, at the time
            // of its first frame: hand it the controls for exactly that time.
            p.lastControls = p.timeline.evaluate(s, t, b.playing, b.useHistory);
            p.lastTime = t;
            p.renderer->setListenerControls(p.lastControls);
        }
        for (int c = 0; c < kMaxInputs; ++c) in[c] = c < b.numInputs && b.inputs[c] ? b.inputs[c] + done : nullptr;
        for (int c = 0; c < p.numOut; ++c) out[c] = p.out[static_cast<size_t>(c)].data() + done;
        p.renderer->process(in, out, k, t);
        p.phase = (p.phase + k) % kSubBlock;
        done += k;
    }
    juce::ignoreUnused(numOut);
}

void LayerEngine::process(const Block& b) {
    handleInbox(false);
    for (int c = 0; c < b.numOutputs; ++c)
        if (b.outputs[c]) std::fill(b.outputs[c], b.outputs[c] + b.numFrames, 0.0f);
    if (!current_ || !b.session) return;

    if (b.jumped) {
        if (current_->renderer) current_->renderer->reset();
        current_->phase = 0;
        retire(fading_);
        fading_ = nullptr;
    }

    const int maxBlock = std::max(64, current_->cfg.maxBlock);
    for (int off = 0; off < b.numFrames; off += maxBlock) {
        Block sub = b;
        sub.numFrames = std::min(maxBlock, b.numFrames - off);
        for (int c = 0; c < kMaxInputs; ++c) sub.inputs[c] = b.inputs[c] ? b.inputs[c] + off : nullptr;
        sub.time = b.time + off / current_->cfg.sampleRate;
        renderProgram(*current_, sub, b.numOutputs);
        const int nc = std::min(b.numOutputs, current_->numOut);
        if (fading_ && fading_->renderer && maxBlock <= std::max(64, fading_->cfg.maxBlock)) {
            renderProgram(*fading_, sub, b.numOutputs);
            const int fadeLen = std::max(1, static_cast<int>(kFadeSeconds * current_->cfg.sampleRate));
            const int nf = std::min(b.numOutputs, fading_->numOut);
            for (int k = 0; k < sub.numFrames; ++k) {
                const float w = std::min(1.0f, static_cast<float>(fadePos_ + k) / static_cast<float>(fadeLen));
                const float gi = std::sin(w * 1.5707963f), go = std::cos(w * 1.5707963f);
                for (int c = 0; c < nc; ++c) current_->out[static_cast<size_t>(c)][static_cast<size_t>(k)] *= gi;
                for (int c = 0; c < nf; ++c)
                    if (b.outputs[c]) b.outputs[c][off + k] += fading_->out[static_cast<size_t>(c)][static_cast<size_t>(k)] * go;
            }
            fadePos_ += sub.numFrames;
            if (fadePos_ >= fadeLen) { retire(fading_); fading_ = nullptr; }
        } else if (fading_) {
            retire(fading_);
            fading_ = nullptr;
        }
        if (current_->renderer) {
            for (int c = 0; c < nc; ++c) {
                if (!b.outputs[c]) continue;
                const float* src = current_->out[static_cast<size_t>(c)].data();
                float* dst = b.outputs[c] + off;
                for (int k = 0; k < sub.numFrames; ++k) dst[k] += src[k];
            }
        }
    }

    const sp::Pose pose = current_->timeline.pose(current_->lastTime, current_->lastControls);
    float yaw, pitch, roll;
    pose.orientation.toYawPitchRoll(yaw, pitch, roll);
    const float v[6] = {pose.position.x, pose.position.y, pose.position.z, sp::radToDeg(yaw), sp::radToDeg(pitch), sp::radToDeg(roll)};
    for (int i = 0; i < 6; ++i) pose_[static_cast<size_t>(i)].store(v[i], std::memory_order_relaxed);
}

std::array<float, 6> LayerEngine::pose() const {
    std::array<float, 6> p{};
    for (size_t i = 0; i < 6; ++i) p[i] = pose_[i].load(std::memory_order_relaxed);
    return p;
}

std::string findHrtf() {
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const juce::File candidates[] = {
        exe.getParentDirectory().getParentDirectory().getChildFile("Resources/sadie_d1.sofa"),  // macOS bundle
        exe.getParentDirectory().getChildFile("sadie_d1.sofa"),
#ifdef SP_DEFAULT_HRTF
        juce::File(SP_DEFAULT_HRTF),
#endif
    };
    for (const auto& f : candidates)
        if (f.existsAsFile()) return f.getFullPathName().toStdString();
    return {};
}

}  // namespace spplug
