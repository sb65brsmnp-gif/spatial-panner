#include "PluginProcessor.h"

#include <cmath>

#include "SceneDoc.h"
#include "sp/SceneJson.h"

namespace spplug {

using nlohmann::json;
using CT = juce::AudioChannelSet::ChannelType;

namespace {

std::string& sessionPathOverride() {
    static std::string p;
    return p;
}

uint64_t makeToken() {
    uint64_t t = static_cast<uint64_t>(juce::Random::getSystemRandom().nextInt64());
    t ^= static_cast<uint64_t>(juce::Time::getHighResolutionTicks()) << 1;
    return t == 0 ? 1 : t;
}

std::string newId() { return juce::Uuid().toDashedString().toStdString(); }

// The output layouts offered to the host: stereo (binaural or stereo
// speakers) and the surround and immersive layouts Logic uses, up to 16
// channels (9.1.6).
const std::vector<juce::AudioChannelSet>& outputSets() {
    static const std::vector<juce::AudioChannelSet> sets = {
        juce::AudioChannelSet::stereo(),          juce::AudioChannelSet::createLCR(),
        juce::AudioChannelSet::quadraphonic(),    juce::AudioChannelSet::createLCRS(),
        juce::AudioChannelSet::create5point0(),   juce::AudioChannelSet::create5point1(),
        juce::AudioChannelSet::create6point0(),   juce::AudioChannelSet::create6point1(),
        juce::AudioChannelSet::create7point0(),   juce::AudioChannelSet::create7point1(),
        juce::AudioChannelSet::create7point0SDDS(), juce::AudioChannelSet::create7point1SDDS(),
        juce::AudioChannelSet::create5point0point2(), juce::AudioChannelSet::create5point1point2(),
        juce::AudioChannelSet::create5point0point4(), juce::AudioChannelSet::create5point1point4(),
        juce::AudioChannelSet::create7point0point2(), juce::AudioChannelSet::create7point1point2(),
        juce::AudioChannelSet::create7point0point4(), juce::AudioChannelSet::create7point1point4(),
        juce::AudioChannelSet::create7point0point6(), juce::AudioChannelSet::create7point1point6(),
        juce::AudioChannelSet::create9point0point4(), juce::AudioChannelSet::create9point1point4(),
        juce::AudioChannelSet::create9point0point6(), juce::AudioChannelSet::create9point1point6(),
    };
    return sets;
}

}  // namespace

// ITU-R BS.2051 / BS.775 positions; heights at 45 degrees (what Logic's
// Atmos bed assumes, and the engine's presets use).
sp::SpeakerLayout speakerLayoutFor(const juce::AudioChannelSet& set) {
    sp::SpeakerLayout out;
    if (set == juce::AudioChannelSet::quadraphonic()) return sp::SpeakerLayout::preset("quad");
    out.name = set.getDescription().toStdString();
    for (int i = 0; i < set.size(); ++i) {
        const auto t = set.getTypeOfChannel(i);
        sp::Speaker s;
        s.name = juce::AudioChannelSet::getAbbreviatedChannelTypeName(t).toStdString();
        float az = 0, el = 0;
        bool known = true;
        switch (t) {
            case CT::left: az = 30; break;
            case CT::right: az = -30; break;
            case CT::centre: az = 0; break;
            case CT::LFE: case CT::LFE2: s.lfe = true; break;
            case CT::leftSurround: az = 110; break;
            case CT::rightSurround: az = -110; break;
            case CT::leftCentre: az = 15; break;
            case CT::rightCentre: az = -15; break;
            case CT::centreSurround: az = 180; break;
            case CT::leftSurroundSide: az = 90; break;
            case CT::rightSurroundSide: az = -90; break;
            case CT::leftSurroundRear: az = 135; break;
            case CT::rightSurroundRear: az = -135; break;
            case CT::wideLeft: az = 60; break;
            case CT::wideRight: az = -60; break;
            case CT::topMiddle: el = 90; break;
            case CT::topFrontLeft: az = 45; el = 45; break;
            case CT::topFrontCentre: az = 0; el = 45; break;
            case CT::topFrontRight: az = -45; el = 45; break;
            case CT::topRearLeft: az = 135; el = 45; break;
            case CT::topRearCentre: az = 180; el = 45; break;
            case CT::topRearRight: az = -135; el = 45; break;
            case CT::topSideLeft: az = 90; el = 45; break;
            case CT::topSideRight: az = -90; el = 45; break;
            default: known = false; break;
        }
        if (!known) return {};
        s.azimuthDeg = az;
        s.elevationDeg = el;
        out.speakers.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------- parameters

juce::AudioProcessorValueTreeState::ParameterLayout SpatialPannerProcessor::createLayout() {
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto deg = AudioParameterFloatAttributes().withLabel(juce::String::fromUTF8("\xc2\xb0"));
    auto listener = std::make_unique<AudioProcessorParameterGroup>("listener", "Listener (scene track)", " | ");
    listener->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"speed", 1}, "Listener Speed",
                                                             NormalisableRange<float>(0.0f, 4.0f, 0.001f, 0.5f), 1.0f,
                                                             AudioParameterFloatAttributes().withLabel("x")));
    listener->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"position", 1}, "Listener Path Position",
                                                             NormalisableRange<float>(0.0f, 100.0f, 0.01f), 0.0f,
                                                             AudioParameterFloatAttributes().withLabel("%")));
    listener->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"yaw", 1}, "Listener Head Turn",
                                                             NormalisableRange<float>(-180.0f, 180.0f, 0.1f), 0.0f, deg));
    listener->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"pitch", 1}, "Listener Head Tilt",
                                                             NormalisableRange<float>(-90.0f, 90.0f, 0.1f), 0.0f, deg));
    listener->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"roll", 1}, "Listener Head Roll",
                                                             NormalisableRange<float>(-90.0f, 90.0f, 0.1f), 0.0f, deg));
    StringArray paths{"Scene setting"};
    for (int i = 1; i <= 8; ++i) paths.add("Path " + String(i));
    listener->addChild(std::make_unique<AudioParameterChoice>(ParameterID{"path", 1}, "Listener Path", paths, 0));

    auto layer = std::make_unique<AudioProcessorParameterGroup>("layer", "Layer (this track)", " | ");
    layer->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"level", 1}, "Layer Level",
                                                          NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 2.0f), 0.0f,
                                                          AudioParameterFloatAttributes().withLabel("dB")));
    layer->addChild(std::make_unique<AudioParameterBool>(ParameterID{"mute", 1}, "Layer Mute", false));
    layer->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"doppler", 1}, "Layer Doppler",
                                                          NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f,
                                                          AudioParameterFloatAttributes().withLabel("%")));
    layer->addChild(std::make_unique<AudioParameterFloat>(ParameterID{"width", 1}, "Layer Width",
                                                          NormalisableRange<float>(0.0f, 180.0f, 0.1f), 0.0f, deg));
    const char* axes[] = {"x", "y", "z"};
    for (const char* a : axes)
        layer->addChild(std::make_unique<AudioParameterFloat>(ParameterID{std::string("off") + a, 1},
                                                              String("Layer Offset ") + String(a).toUpperCase(),
                                                              NormalisableRange<float>(-20.0f, 20.0f, 0.01f), 0.0f,
                                                              AudioParameterFloatAttributes().withLabel("m")));
    layout.add(std::move(listener), std::move(layer));
    return layout;
}

// -------------------------------------------------------------- construction

void SpatialPannerProcessor::setSessionPathForTesting(const std::string& path) { sessionPathOverride() = path; }

SpatialPannerProcessor::SpatialPannerProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      params_(*this, nullptr, "params", createLayout()),
      session_(SharedSession::attach(sessionPathOverride())),
      token_(makeToken()) {
    pSpeed_ = params_.getRawParameterValue("speed");
    pPosition_ = params_.getRawParameterValue("position");
    pYaw_ = params_.getRawParameterValue("yaw");
    pPitch_ = params_.getRawParameterValue("pitch");
    pRoll_ = params_.getRawParameterValue("roll");
    pPath_ = params_.getRawParameterValue("path");
    pLevel_ = params_.getRawParameterValue("level");
    pMute_ = params_.getRawParameterValue("mute");
    pDoppler_ = params_.getRawParameterValue("doppler");
    pWidth_ = params_.getRawParameterValue("width");
    pX_ = params_.getRawParameterValue("offx");
    pY_ = params_.getRawParameterValue("offy");
    pZ_ = params_.getRawParameterValue("offz");

    layerId_ = newId();
    doc_ = doc::defaultScene();
    constructedMs_ = juce::Time::getMillisecondCounter();
    startTimerHz(10);
}

SpatialPannerProcessor::~SpatialPannerProcessor() {
    *alive_ = false;
    stopTimer();
    if (slot_ >= 0) session_->releaseSlot(slot_, token_);
    session_->releaseScene(token_);
}

bool SpatialPannerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) return false;
    for (const auto& s : outputSets())
        if (out == s) return true;
    return false;
}

EngineConfig SpatialPannerProcessor::engineConfig() const {
    EngineConfig c;
    c.sampleRate = sampleRate_;
    c.maxBlock = std::max(64, static_cast<int>(mono_.size()));
    const auto out = getBus(false, 0) ? getBus(false, 0)->getCurrentLayout() : juce::AudioChannelSet::stereo();
    if (out == juce::AudioChannelSet::stereo()) {
        c.mode = stereoAsSpeakers_ ? sp::OutputMode::Speakers : sp::OutputMode::Binaural;
        c.layout = sp::SpeakerLayout::preset("stereo");
    } else {
        c.mode = sp::OutputMode::Speakers;
        c.layout = speakerLayoutFor(out);
        if (c.layout.numChannels() == 0) c.layout = sp::SpeakerLayout::preset("stereo");
    }
    c.render = !(role_ == Role::Scene && !sceneIsLayer_);
    c.hrtfPath = findHrtf();
    return c;
}

void SpatialPannerProcessor::reconfigureEngine() {
    passThrough_ = role_ == Role::Scene && !sceneIsLayer_;
    engine_.configure(engineConfig());
    setLatencySamples(passThrough_ ? 0 : engine_.latencySamples());
}

void SpatialPannerProcessor::prepareToPlay(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate;
    mono_.assign(static_cast<size_t>(std::max(maxBlock, 64)), 0.0f);
    expected_ = -1;
    reconfigureEngine();
    {
        const juce::ScopedLock l(docLock_);
        docText_.clear();
        applyDocToEngine(doc_);
    }
}

// ---------------------------------------------------------------- the scene

json SpatialPannerProcessor::sceneDoc() const {
    const juce::ScopedLock l(docLock_);
    return doc_;
}

void SpatialPannerProcessor::applyDocToEngine(const json& d) {
    std::string text = d.dump();
    if (text == docText_) return;
    try {
        bool found = false;
        const sp::Scene s = doc::layerScene(d, layerId_, &found);
        const auto& L = s.layers.front();
        docLevelGain_ = L.mute ? 0.0f : sp::dbToGain(L.levelDb);
        docDoppler_ = L.dopplerAmount;
        docSpread_ = L.spreadDeg;
        engine_.setScene(s);
        docText_ = std::move(text);
    } catch (const std::exception& e) {
        status_ = juce::String("Scene error: ") + e.what();
    }
}

void SpatialPannerProcessor::publish() {
    const juce::ScopedLock l(docLock_);
    session_->publishScene(token_, doc_.dump());
    seenRevision_ = session_->sceneRevision();
    applyDocToEngine(doc_);
    needsPublish_ = false;
}

bool SpatialPannerProcessor::setSceneDoc(const json& d, std::string& error) {
    if (role_ != Role::Scene) { error = "This instance does not hold the scene"; return false; }
    try {
        sp::sceneFromJson(d.dump());
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    bool changedByPlugin = false;
    json copy;
    {
        const juce::ScopedLock l(docLock_);
        // The editor's "position along path" slider and the automatable
        // parameter are one control.
        const auto fractionOf = [](const json& j) { return j.contains("listener") ? j["listener"].value("path_fraction", 0.0) : 0.0; };
        const double f = fractionOf(d);
        if (std::abs(f - fractionOf(doc_)) > 1e-6)
            if (auto* p = params_.getParameter("position")) p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(f * 100.0)));
        doc_ = d;
        changedByPlugin = doc::reconcile(doc_, session_->liveLayers(5000, false));
        copy = doc_;
    }
    publish();
    if (changedByPlugin && onSceneChanged) onSceneChanged(copy);
    return true;
}

void SpatialPannerProcessor::clearAutomationHistory() {
    if (role_ == Role::Scene) session_->clearHistory();
}

std::vector<SpatialPannerProcessor::HostTrack> SpatialPannerProcessor::hostTracks() {
    std::vector<HostTrack> out;
    for (const auto& l : session_->liveLayers(5000, true))
        out.push_back({l.id, l.name, l.meterPeak > 0 ? sp::gainToDb(l.meterPeak) : -120.0f});
    return out;
}

// --------------------------------------------------------------------- roles

void SpatialPannerProcessor::becomeScene(bool force) {
    if (!session_->claimScene(token_, force)) {
        if (role_ != Role::Layer) becomeLayer();
        status_ = "Another track holds the scene";
        return;
    }
    const bool changed = role_ != Role::Scene;
    role_ = Role::Scene;
    // Automation recorded by another scene instance belongs to its lanes.
    session_->clearHistory();
    if (!historyText_.empty()) session_->decodeHistory(historyText_);
    historyText_.clear();
    recordHistory_ = true;
    needsPublish_ = true;
    status_.clear();
    reconfigureEngine();
    publish();
    if (changed && onRoleChanged) onRoleChanged();
}

void SpatialPannerProcessor::becomeLayer() {
    const bool changed = role_ != Role::Layer;
    if (role_ == Role::Scene) {
        // Keep this instance's automation if it becomes the scene again later.
        historyText_ = session_->encodeHistory();
    }
    session_->releaseScene(token_);
    role_ = Role::Layer;
    recordHistory_ = false;
    seenRevision_ = 0;
    reconfigureEngine();
    if (changed && onRoleChanged) onRoleChanged();
}

void SpatialPannerProcessor::setRole(Role r, bool isLayer) {
    sceneIsLayer_ = isLayer;
    if (r == Role::Scene) {
        if (role_ == Role::Scene) {
            reconfigureEngine();
            if (onRoleChanged) onRoleChanged();
        } else {
            becomeScene(true);
        }
    } else {
        becomeLayer();
    }
}

void SpatialPannerProcessor::setStereoAsSpeakers(bool b) {
    stereoAsSpeakers_ = b;
    reconfigureEngine();
}

void SpatialPannerProcessor::decideRole() {
    if (role_ != Role::Undecided) return;
    // Give the host a moment to restore saved state before choosing: a new
    // instance holds the scene if nobody does yet.
    if (!stateRestored_ && juce::Time::getMillisecondCounter() - constructedMs_ < 500) return;
    if (session_->claimScene(token_, false)) becomeScene(false);
    else becomeLayer();
}

void SpatialPannerProcessor::manageSlot() {
    const bool want = role_ != Role::Scene || sceneIsLayer_;
    if (!want) {
        if (slot_ >= 0) session_->releaseSlot(slot_, token_);
        slot_ = -1;
        slotForAudio_ = -1;
        return;
    }
    if (slot_ < 0 || !session_->slotOwnedBy(slot_, token_)) {
        int s = session_->claimSlot(token_, layerId_, cloneOf_);
        if (s == -1) {
            // Another live instance has this id: this one is a duplicated
            // track. Take a new id and start where the original is.
            cloneOf_ = layerId_;
            layerId_ = newId();
            docText_.clear();
            s = session_->claimSlot(token_, layerId_, cloneOf_);
        }
        slot_ = s >= 0 ? s : -1;
        if (slot_ >= 0) session_->setSlotName(slot_, token_, trackName_);
    }
    session_->heartbeatSlot(slot_, token_);
    slotForAudio_ = slot_;
}

void SpatialPannerProcessor::updateTrackProperties(const TrackProperties& p) {
    const std::string name = p.name ? p.name->toStdString() : std::string();
    juce::MessageManager::callAsync([this, alive = alive_, name] {
        if (!*alive) return;
        trackName_ = name;
        if (slot_ >= 0) session_->setSlotName(slot_, token_, name);
    });
}

void SpatialPannerProcessor::adoptPublishedScene() {
    if (session_->sceneOwner() == 0) {
        // No scene instance: keep rendering the copy saved with this track.
        const juce::ScopedLock l(docLock_);
        applyDocToEngine(doc_);
        return;
    }
    const uint32_t gen = session_->ownerGeneration();
    if (gen != seenGeneration_) {
        seenGeneration_ = gen;
        seenRevision_ = 0;
    }
    std::string text;
    if (!session_->readScene(seenRevision_, text)) return;
    try {
        json d = json::parse(text);
        const juce::ScopedLock l(docLock_);
        doc_ = std::move(d);
        applyDocToEngine(doc_);
    } catch (const std::exception&) {
    }
}

void SpatialPannerProcessor::timerCallback() {
    decideRole();
    if (role_ == Role::Scene) {
        if (session_->sceneOwner() != token_) {
            becomeLayer();
            status_ = "Another track took over the scene";
        } else {
            session_->heartbeatScene(token_);
        }
    }
    manageSlot();
    if (role_ == Role::Scene) {
        bool changed = false;
        json copy;
        {
            const juce::ScopedLock l(docLock_);
            changed = doc::reconcile(doc_, session_->liveLayers(5000, false));
            if (changed) copy = doc_;
        }
        if (changed) needsPublish_ = true;
        if (needsPublish_) publish();
        if (changed && onSceneChanged) onSceneChanged(copy);
    } else if (role_ == Role::Layer) {
        adoptPublishedScene();
    }
    engine_.update();
    const auto err = engine_.lastError();
    if (!err.empty()) status_ = "Audio engine: " + err;
}

juce::String SpatialPannerProcessor::statusText() const {
    if (status_.isNotEmpty()) return status_;
    switch (role_) {
        case Role::Undecided: return "Starting";
        case Role::Scene: {
            int n = static_cast<int>(session_->liveLayers(5000, false).size());
            return "Holds the scene · " + juce::String(n) + (n == 1 ? " layer track" : " layer tracks") +
                   (session_->isShared() ? "" : " (this process only)");
        }
        case Role::Layer:
            if (session_->sceneOwner() == 0) return "No scene track in this session: playing the scene saved with this track";
            return session_->sceneAlive() ? "Following the scene track" : "The scene track is not responding";
    }
    return {};
}

juce::String SpatialPannerProcessor::outputText() const {
    if (passThrough_) return "Passes this track's audio through";
    const auto c = engineConfig();
    if (c.mode == sp::OutputMode::Binaural) return "Binaural (headphones)";
    return "Speakers: " + juce::String(c.layout.name);
}

// --------------------------------------------------------------------- state

void SpatialPannerProcessor::getStateInformation(juce::MemoryBlock& dest) {
    juce::XmlElement root("SpatialPanner");
    root.setAttribute("version", 1);
    root.setAttribute("role", role_ == Role::Scene ? "scene" : "layer");
    root.setAttribute("sceneIsLayer", sceneIsLayer_);
    root.setAttribute("stereoSpeakers", stereoAsSpeakers_);
    root.setAttribute("layerId", juce::String(layerId_));
    if (auto params = params_.copyState().createXml()) root.addChildElement(params.release());
    {
        const juce::ScopedLock l(docLock_);
        root.createNewChildElement("Scene")->addTextElement(juce::String::fromUTF8(doc_.dump().c_str()));
    }
    if (role_ == Role::Scene && session_->sceneOwner() == token_)
        root.createNewChildElement("History")->addTextElement(juce::String(session_->encodeHistory()));
    copyXmlToBinary(root, dest);
}

void SpatialPannerProcessor::setStateInformation(const void* data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName("SpatialPanner")) return;
    if (auto* p = xml->getChildByName(params_.state.getType())) params_.replaceState(juce::ValueTree::fromXml(*p));
    sceneIsLayer_ = xml->getBoolAttribute("sceneIsLayer", true);
    stereoAsSpeakers_ = xml->getBoolAttribute("stereoSpeakers", false);
    const std::string id = xml->getStringAttribute("layerId").toStdString();
    if (!id.empty() && id != layerId_) {
        if (slot_ >= 0) session_->releaseSlot(slot_, token_);
        slot_ = -1;
        layerId_ = id;
    }
    if (auto* s = xml->getChildByName("Scene")) {
        try {
            json d = json::parse(s->getAllSubText().toStdString());
            const juce::ScopedLock l(docLock_);
            doc_ = std::move(d);
            docText_.clear();
        } catch (const std::exception&) {
        }
    }
    historyText_ = xml->getChildByName("History") ? xml->getChildByName("History")->getAllSubText().toStdString() : std::string();
    stateRestored_ = true;
    if (xml->getStringAttribute("role") == "scene") becomeScene(false);
    else becomeLayer();
    {
        const juce::ScopedLock l(docLock_);
        applyDocToEngine(doc_);
    }
    manageSlot();
}

// --------------------------------------------------------------------- audio

void SpatialPannerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const auto ticks0 = juce::Time::getHighResolutionTicks();
    const int n = buffer.getNumSamples();
    const int nIn = getTotalNumInputChannels(), nOut = getTotalNumOutputChannels();
    if (n <= 0) return;

    // Where the host's timeline is. Without a playhead (auval, some hosts)
    // run a free clock.
    juce::int64 pos = freeRun_;
    bool playing = true, havePos = false;
    if (auto* ph = getPlayHead()) {
        if (auto p = ph->getPosition()) {
            playing = p->getIsPlaying();
            if (auto s = p->getTimeInSamples()) { pos = *s; havePos = true; }
            else if (auto sec = p->getTimeInSeconds()) { pos = static_cast<juce::int64>(std::llround(*sec * sampleRate_)); havePos = true; }
        }
    }
    if (!havePos) { playing = true; freeRun_ = pos + n; }
    const bool jumped = expected_ >= 0 && std::llabs(pos - expected_) > 1;
    expected_ = pos + (playing ? n : 0);
    const double t = static_cast<double>(pos) / sampleRate_;
    lastTime_ = t;
    lastPlaying_ = playing;

    // The scene instance records its listener automation for everyone.
    if (recordHistory_.load(std::memory_order_relaxed)) {
        const float v[kNumListenerParams] = {
            pSpeed_->load(), pPosition_->load() / 100.0f, pYaw_->load(), pPitch_->load(), pRoll_->load(),
            static_cast<float>(std::lround(pPath_->load())) - 1.0f};
        for (int p = 0; p < kNumListenerParams; ++p) {
            session_->setLive(p, v[p]);
            if (playing) session_->record(p, t, t + n / sampleRate_, v[p]);
        }
    }
    const bool useHistory = session_->sceneOwner() != 0;

    if (isNonRealtime() && !engine_.hasProgram()) {
        // Offline bounce right after loading: wait for the renderer instead of
        // bouncing silence.
        engine_.waitUntilCurrent(30000);
        engine_.drainInboxBlocking();
    }

    if (passThrough_.load(std::memory_order_relaxed)) {
        for (int c = nIn; c < nOut; ++c) {
            if (nIn == 1 && c == 1) buffer.copyFrom(1, 0, buffer, 0, 0, n);
            else buffer.clear(c, 0, n);
        }
        LayerEngine::Block b;
        b.numFrames = n;
        b.time = t;
        b.playing = playing;
        b.jumped = jumped;
        b.session = session_.get();
        b.useHistory = useHistory;
        engine_.process(b);   // listener pose only
        return;
    }

    sp::LayerControls lc;
    const float levelDb = pLevel_->load();
    lc.levelOffsetDb = levelDb;
    lc.mute = pMute_->load() > 0.5f || levelDb <= -59.95f;
    lc.positionOffset = {pX_->load(), pY_->load(), pZ_->load()};
    lc.dopplerAmount = docDoppler_.load() * pDoppler_->load() / 100.0f;
    lc.spreadDeg = std::min(180.0f, docSpread_.load() + pWidth_->load());
    const float meterGain = lc.mute ? 0.0f : docLevelGain_.load() * sp::dbToGain(levelDb);
    const int slot = slotForAudio_.load(std::memory_order_relaxed);

    const int chunk = static_cast<int>(mono_.size());
    float* outs[LayerEngine::kMaxOutputs];
    const int no = std::min(nOut, LayerEngine::kMaxOutputs);
    for (int off = 0; off < n; off += chunk) {
        const int k = std::min(chunk, n - off);
        float peak = 0;
        if (nIn > 0) {
            const float g = 1.0f / static_cast<float>(nIn);
            for (int i = 0; i < k; ++i) {
                float s = 0;
                for (int c = 0; c < nIn; ++c) s += buffer.getReadPointer(c)[off + i];
                s *= g;
                mono_[static_cast<size_t>(i)] = s;
                peak = std::max(peak, std::abs(s));
            }
        } else {
            std::fill(mono_.begin(), mono_.begin() + k, 0.0f);
        }
        if (slot >= 0) session_->addMeter(slot, peak * meterGain);
        for (int c = 0; c < no; ++c) outs[c] = buffer.getWritePointer(c) + off;
        LayerEngine::Block b;
        b.input = mono_.data();
        b.outputs = outs;
        b.numOutputs = no;
        b.numFrames = k;
        b.time = t + off / sampleRate_;
        b.playing = playing;
        b.jumped = jumped && off == 0;
        b.session = session_.get();
        b.useHistory = useHistory;
        b.layer = lc;
        engine_.process(b);
    }
    for (int c = no; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, n);

    const double secs = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - ticks0);
    cpu_ = cpu_.load() * 0.9f + static_cast<float>(secs / (n / sampleRate_)) * 0.1f;
}

}  // namespace spplug
