// The plugin driven the way a host drives it: buses, prepareToPlay, a
// playhead, blocks of audio, saved state; several instances in one process
// sharing one session file, as on a Logic session.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <functional>
#include <random>

#include "PluginProcessor.h"
#include "SceneDoc.h"
#include "sp/Renderer.h"
#include "sp/SceneJson.h"

using namespace spplug;
using nlohmann::json;

// The tests run without the editor (no WebView).
juce::AudioProcessorEditor* spplug::SpatialPannerProcessor::createEditor() { return nullptr; }

namespace {

constexpr double kRate = 48000;
constexpr int kBlock = 512;

struct PlayHead : juce::AudioPlayHead {
    juce::int64 pos = 0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo p;
        p.setTimeInSamples(pos);
        p.setTimeInSeconds(static_cast<double>(pos) / kRate);
        p.setIsPlaying(playing);
        return p;
    }
};

// A fresh session file per test, so tests don't see each other's instances.
struct FreshSession {
    juce::TemporaryFile file{".bin"};
    FreshSession() {
        SharedSession::detachAllForTesting();
        SpatialPannerProcessor::setSessionPathForTesting(file.getFile().getFullPathName().toStdString());
    }
    ~FreshSession() {
        SharedSession::detachAllForTesting();
        SpatialPannerProcessor::setSessionPathForTesting({});
    }
};

std::unique_ptr<SpatialPannerProcessor> makeInstance(PlayHead& ph, juce::AudioChannelSet out = juce::AudioChannelSet::stereo(),
                                                     juce::AudioChannelSet in = juce::AudioChannelSet::mono()) {
    auto p = std::make_unique<SpatialPannerProcessor>();
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(in);
    layout.outputBuses.add(out);
    REQUIRE(p->setBusesLayout(layout));
    p->setPlayHead(&ph);
    p->prepareToPlay(kRate, kBlock);
    return p;
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

void tickAll(const std::vector<SpatialPannerProcessor*>& ps, int times = 2) {
    for (int i = 0; i < times; ++i)
        for (auto* p : ps) p->tickForTesting();
}

void waitEngines(const std::vector<SpatialPannerProcessor*>& ps) {
    for (auto* p : ps) REQUIRE(p->engineForTesting().waitUntilCurrent(20000));
}

json loadDoc(const char* name) {
    const auto f = juce::File(SP_SCENES_DIR).getChildFile(name);
    REQUIRE(f.existsAsFile());
    return json::parse(sp::sceneToJson(sp::sceneFromJson(f.loadFileAsString().toStdString())));
}

std::vector<std::vector<float>> noise(int layers, int frames, unsigned seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0.0f, 0.1f);
    std::vector<std::vector<float>> x(static_cast<size_t>(layers), std::vector<float>(static_cast<size_t>(frames)));
    for (auto& ch : x)
        for (auto& v : ch) v = d(rng);
    return x;
}

double rms(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += static_cast<double>(x) * x;
    return std::sqrt(s / std::max<size_t>(1, v.size()));
}

}  // namespace

TEST_CASE("Output layouts: binaural on stereo, Logic's surround and Atmos beds by channel type") {
    FreshSession fs;
    SpatialPannerProcessor p;
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::mono());
    for (const auto& out : {juce::AudioChannelSet::stereo(), juce::AudioChannelSet::create5point1(),
                            juce::AudioChannelSet::create7point1point4(), juce::AudioChannelSet::create9point1point6()}) {
        l.outputBuses.clear();
        l.outputBuses.add(out);
        CHECK(p.checkBusesLayoutSupported(l));
    }
    l.inputBuses.clear();
    l.inputBuses.add(juce::AudioChannelSet::create5point1());
    CHECK_FALSE(p.checkBusesLayoutSupported(l));

    const auto atmos = speakerLayoutFor(juce::AudioChannelSet::create7point1point4());
    REQUIRE(atmos.numChannels() == 12);
    int lfe = 0, heights = 0;
    for (const auto& s : atmos.speakers) {
        lfe += s.lfe ? 1 : 0;
        heights += s.elevationDeg > 40 ? 1 : 0;
    }
    CHECK(lfe == 1);
    CHECK(heights == 4);
}

TEST_CASE("A new instance holds the scene when there is none; later ones are layers that get a layer each") {
    FreshSession fs;
    PlayHead ph;
    auto a = makeInstance(ph);
    auto b = makeInstance(ph);
    a->setRole(SpatialPannerProcessor::Role::Scene, true);
    b->setRole(SpatialPannerProcessor::Role::Layer);
    tickAll({a.get(), b.get()}, 3);
    CHECK(a->role() == SpatialPannerProcessor::Role::Scene);
    CHECK(b->role() == SpatialPannerProcessor::Role::Layer);

    const json d = a->sceneDoc();
    REQUIRE(d["layers"].size() == 2);
    CHECK(doc::layerIndex(d, a->layerId()) >= 0);
    CHECK(doc::layerIndex(d, b->layerId()) >= 0);
    // The layer instance follows the scene's document.
    CHECK(b->sceneDoc()["layers"].size() == 2);

    SECTION("an undecided instance becomes a layer when a scene exists") {
        auto c = std::make_unique<SpatialPannerProcessor>();
        pump(700);
        c->tickForTesting();
        CHECK(c->role() == SpatialPannerProcessor::Role::Layer);
    }
}

TEST_CASE("A duplicated track gets its own layer, starting where the original is") {
    FreshSession fs;
    PlayHead ph;
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);
    auto layer = makeInstance(ph);
    layer->setRole(SpatialPannerProcessor::Role::Layer);
    tickAll({scene.get(), layer.get()}, 3);

    // Move the original somewhere recognisable.
    json d = scene->sceneDoc();
    const int i = doc::layerIndex(d, layer->layerId());
    REQUIRE(i >= 0);
    d["layers"][static_cast<size_t>(i)]["position"] = {5.5, 1.2, -3.25};
    std::string error;
    REQUIRE(scene->setSceneDoc(d, error));

    juce::MemoryBlock state;
    layer->getStateInformation(state);
    auto copy = makeInstance(ph);
    copy->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    tickAll({scene.get(), layer.get(), copy.get()}, 3);

    CHECK(copy->layerId() != layer->layerId());
    d = scene->sceneDoc();
    REQUIRE(d["layers"].size() == 2);
    const int j = doc::layerIndex(d, copy->layerId());
    REQUIRE(j >= 0);
    CHECK(d["layers"][static_cast<size_t>(j)]["position"] == json({5.5, 1.2, -3.25}));
}

namespace {

struct SumResult {
    double refRms = 0, errRms = 0, worstEnvelopeDb = 0;
};

// Renders `d` with one layer instance per layer (plus a scene-only instance)
// and with one engine renderer holding every layer; returns how far apart
// the summed instance outputs and the engine's output are.
SumResult renderBothWays(json d, int seconds) {
    FreshSession fs;
    PlayHead ph;
    const int nLayers = static_cast<int>(d["layers"].size());
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);   // scene only: passes audio through
    std::vector<std::unique_ptr<SpatialPannerProcessor>> layers;
    std::vector<SpatialPannerProcessor*> all{scene.get()};
    for (int i = 0; i < nLayers; ++i) {
        layers.push_back(makeInstance(ph));
        layers.back()->setRole(SpatialPannerProcessor::Role::Layer);
        all.push_back(layers.back().get());
    }
    tickAll(all, 2);
    // Bind the scene's layers to the tracks, in order.
    for (int i = 0; i < nLayers; ++i) d["layers"][static_cast<size_t>(i)]["host_id"] = layers[static_cast<size_t>(i)]->layerId();
    std::string error;
    REQUIRE(scene->setSceneDoc(d, error));
    tickAll(all, 2);
    REQUIRE(scene->sceneDoc()["layers"].size() == static_cast<size_t>(nLayers));
    waitEngines(all);

    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    // One block somewhere else first, so the transport jump to 0 starts the
    // comparison from cleared state, like the reference.
    ph.pos = static_cast<juce::int64>(20 * kRate);
    for (auto* p : all) { buf.clear(); p->processBlock(buf, midi); }

    const sp::Scene full = sp::sceneFromJson(d.dump());
    sp::RenderConfig cfg;
    cfg.sampleRate = kRate;
    cfg.hrtfPath = SP_DEFAULT_HRTF;
    sp::Renderer ref(full, cfg, 30);

    const int total = static_cast<int>(kRate) * seconds;
    const auto in = noise(nLayers, total);
    std::vector<float> sumL(static_cast<size_t>(total)), sumR(sumL), refL(sumL), refR(sumL);
    std::vector<const float*> rin(static_cast<size_t>(nLayers));
    std::vector<float> ro0(kBlock), ro1(kBlock);
    float* rout[2] = {ro0.data(), ro1.data()};
    for (int pos = 0; pos < total; pos += kBlock) {
        ph.pos = pos;
        buf.clear();
        scene->processBlock(buf, midi);
        for (int i = 0; i < nLayers; ++i) {
            buf.clear();
            buf.copyFrom(0, 0, in[static_cast<size_t>(i)].data() + pos, kBlock);
            layers[static_cast<size_t>(i)]->processBlock(buf, midi);
            for (int k = 0; k < kBlock; ++k) {
                sumL[static_cast<size_t>(pos + k)] += buf.getSample(0, k);
                sumR[static_cast<size_t>(pos + k)] += buf.getSample(1, k);
            }
            rin[static_cast<size_t>(i)] = in[static_cast<size_t>(i)].data() + pos;
        }
        ref.process(rin.data(), rout, kBlock, pos / kRate);
        std::copy_n(ro0.data(), kBlock, refL.data() + pos);
        std::copy_n(ro1.data(), kBlock, refR.data() + pos);
    }

    SumResult r;
    std::vector<float> diff(static_cast<size_t>(total));
    for (size_t k = 0; k < diff.size(); ++k) diff[k] = std::max(std::abs(sumL[k] - refL[k]), std::abs(sumR[k] - refR[k]));
    r.refRms = rms(refL);
    r.errRms = rms(diff);
    // Loudness envelope, 20 ms windows, per ear.
    const int w = static_cast<int>(0.02 * kRate);
    for (const auto& pair : {std::make_pair(&sumL, &refL), std::make_pair(&sumR, &refR)}) {
        for (int k0 = 0; k0 + w <= total; k0 += w) {
            double a = 0, b = 0;
            for (int k = k0; k < k0 + w; ++k) {
                a += std::pow((*pair.first)[static_cast<size_t>(k)], 2);
                b += std::pow((*pair.second)[static_cast<size_t>(k)], 2);
            }
            if (b < 1e-8 * w) continue;
            r.worstEnvelopeDb = std::max(r.worstEnvelopeDb, std::abs(10 * std::log10(a / b)));
        }
    }
    return r;
}

}  // namespace

TEST_CASE("A stereo track's layer is a left/right pair; the Mono parameter folds it to the centre") {
    FreshSession fs;
    PlayHead ph;
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);
    auto layer = makeInstance(ph, juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo());
    layer->setRole(SpatialPannerProcessor::Role::Layer);
    std::vector<SpatialPannerProcessor*> all{scene.get(), layer.get()};
    tickAll(all, 3);
    pump(50);
    tickAll(all, 2);

    // The track's two channels reach the scene document as a stereo layer.
    json d = scene->sceneDoc();
    REQUIRE(d["layers"].size() == 1);
    CHECK(d["layers"][0].value("channels", 1) == 2);
    d["room"]["type"] = "none";
    d["listener"]["paths"] = json::array();
    d["listener"]["static_position"] = {0, 1.6, 0};
    d["layers"][0]["position"] = {0, 1.6, -2};
    d["layers"][0]["stereo"] = {{"width", 4.0}, {"rotation", 0}, {"elevation", 0}, {"mono", false}};
    std::string error;
    REQUIRE(scene->setSceneDoc(d, error));
    tickAll(all, 2);
    waitEngines(all);

    // Noise on the left channel only: the left ear hears much more than the right.
    const int total = static_cast<int>(kRate) * 2;
    const auto in = noise(1, total);
    auto renderEars = [&](bool mono) {
        if (auto* p = layer->parametersForTesting().getParameter("mono")) p->setValueNotifyingHost(mono ? 1.0f : 0.0f);
        juce::AudioBuffer<float> buf(2, kBlock);
        juce::MidiBuffer midi;
        ph.pos = static_cast<juce::int64>(20 * kRate);
        buf.clear();
        layer->processBlock(buf, midi);
        double l = 0, r = 0;
        for (int pos = 0; pos < total; pos += kBlock) {
            ph.pos = pos;
            buf.clear();
            buf.copyFrom(0, 0, in[0].data() + pos, kBlock);
            layer->processBlock(buf, midi);
            if (pos < static_cast<int>(kRate) / 2) continue;  // let the fold settle
            for (int k = 0; k < kBlock; ++k) { l += std::pow(buf.getSample(0, k), 2); r += std::pow(buf.getSample(1, k), 2); }
        }
        return std::make_pair(l, r);
    };
    const auto spread = renderEars(false);
    REQUIRE(spread.first > 1e-6);
    CHECK(10 * std::log10(spread.first / spread.second) > 6);
    // Folded to mono the pair plays from the centre: both ears alike.
    const auto folded = renderEars(true);
    CHECK(std::abs(10 * std::log10(folded.first / folded.second)) < 1.5);
}

TEST_CASE("Layer instances on separate tracks sum to the engine rendering all layers at once") {
    json d = loadDoc("room_walk.json");
    SECTION("standing listener: the same samples") {
        d["listener"]["paths"] = json::array();
        const auto r = renderBothWays(d, 4);
        INFO("reference RMS " << r.refRms << ", difference RMS " << r.errRms);
        REQUIRE(r.refRms > 1e-3);
        CHECK(r.errRms < r.refRms * 1e-4);   // -80 dB: float rounding
    }
    SECTION("walking listener: the same sound") {
        // The instances get the listener as a path fraction rather than a
        // distance; the micrometre rounding that adds can move an HRTF update
        // by a sub-block, so the waveforms are close, not identical.
        const auto r = renderBothWays(d, 12);
        INFO("reference RMS " << r.refRms << ", difference RMS " << r.errRms << ", worst 20 ms level difference "
                              << r.worstEnvelopeDb << " dB");
        REQUIRE(r.refRms > 1e-3);
        CHECK(r.errRms < r.refRms * 0.1);    // -20 dB
        CHECK(r.worstEnvelopeDb < 0.5);
    }
    SECTION("layers travelling their own paths: the same sound") {
        // By speed (the plugin integrates the track's Path Speed, here x1),
        // by keys going back and forth and turning, and held at a point
        // (the Path Position parameter picks up the scene's value).
        d["listener"]["paths"] = json::array();
        REQUIRE(d["layers"].size() >= 3);
        auto line = [](json from, double dx, double dz) {
            json to = {from[0].get<double>() + dx, from[1], from[2].get<double>() + dz};
            return json{{"name", ""}, {"closed", false}, {"segments", json::array({json{{"type", "line"}, {"points", {from, to}}}})}};
        };
        auto& L = d["layers"];
        L[0]["motion"] = {{"path", line(L[0]["position"], 6, 0)}, {"timing", "speed"},
                          {"speed", json::array({json{{"time", 0}, {"speed", 1.5}, {"easing", "linear"}}})}, {"start_time", 1.0}, {"end", "loop"}};
        L[1]["motion"] = {{"path", line(L[1]["position"], 0, -5)}, {"timing", "keys"}, {"end", "ping_pong"}, {"turn", true},
                          {"keys", json::array({json{{"time", 0.5}, {"fraction", 0}, {"easing", "smooth"}}, json{{"time", 3}, {"fraction", 1}, {"easing", "smooth"}}})}};
        L[2]["motion"] = {{"path", line(L[2]["position"], -4, 2)}, {"timing", "position"}, {"fraction", 0.4}};
        L[2]["level_keys"] = json::array({json{{"time", 0}, {"level_db", -80}, {"easing", "linear"}}, json{{"time", 2}, {"level_db", 0}, {"easing", "linear"}}});
        const auto r = renderBothWays(d, 8);
        INFO("reference RMS " << r.refRms << ", difference RMS " << r.errRms << ", worst 20 ms level difference "
                              << r.worstEnvelopeDb << " dB");
        REQUIRE(r.refRms > 1e-3);
        CHECK(r.errRms < r.refRms * 0.1);
        CHECK(r.worstEnvelopeDb < 0.5);
    }
}

namespace {

// A straight 40 m path ahead of the listener, walked at 1 m/s.
json straightDoc() {
    json d = doc::defaultScene();
    d["room"]["type"] = "none";
    d["listener"]["paths"] = json::array({json{{"name", "line"}, {"closed", false},
                                              {"segments", json::array({json{{"type", "line"}, {"points", {{0, 1.7, 0}, {0, 1.7, -40}}}}})}}});
    d["listener"]["speed"] = json::array({json{{"time", 0}, {"speed", 1.0}, {"easing", "linear"}}});
    return d;
}

// Speed automation used below: x1 for 2 s, ramps to x3 by 4 s, holds.
float speedAt(double t) { return t < 2 ? 1.0f : t < 4 ? 1.0f + static_cast<float>(t - 2) : 3.0f; }

// Distance at time t with the automation constant over each host block, as
// a host delivers it.
double expectedDistance(double t) {
    double s = 0;
    for (int pos = 0; pos < static_cast<int>(t * kRate); pos += kBlock)
        s += speedAt(pos / kRate) * std::min<double>(kBlock, t * kRate - pos) / kRate;
    return s;
}

// The pose a block reports is the one of its last 32-sample sub-block.
double poseTime(int blockStart) { return (blockStart + kBlock - 32) / kRate; }

}  // namespace

TEST_CASE("Speed automation on the scene track moves the listener the same way in every layer instance") {
    FreshSession fs;
    PlayHead ph;
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);
    auto a = makeInstance(ph);
    a->setRole(SpatialPannerProcessor::Role::Layer);
    tickAll({scene.get(), a.get()}, 2);
    std::string error;
    REQUIRE(scene->setSceneDoc(straightDoc(), error));
    tickAll({scene.get(), a.get()}, 2);
    waitEngines({scene.get(), a.get()});

    auto* speed = scene->parameters().getParameter("speed");
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    const int total = static_cast<int>(6 * kRate);

    // Each pass plays from the start. `layerFirst`: the layer is processed
    // before the scene track in every block (as when Logic renders it ahead).
    auto play = [&](bool layerFirst) {
        std::vector<float> z;
        for (int pos = 0; pos < total; pos += kBlock) {
            ph.pos = pos;
            speed->setValueNotifyingHost(speed->convertTo0to1(speedAt(pos / kRate)));
            buf.clear();
            if (!layerFirst) { scene->processBlock(buf, midi); buf.clear(); }
            a->processBlock(buf, midi);
            if (layerFirst) { buf.clear(); scene->processBlock(buf, midi); }
            z.push_back(a->listenerPose()[2]);
        }
        return z;
    };
    auto worstDiff = [](const std::vector<float>& x, const std::vector<float>& y) {
        float w = 0;
        for (size_t i = 0; i < x.size(); ++i) w = std::max(w, std::abs(x[i] - y[i]));
        return w;
    };

    const auto first = play(false);
    // The listener's position is the integral of the automated speed. The
    // recorded automation has 10 ms resolution, which here (a ramp of 1 m/s
    // per second) puts the listener up to 1 cm behind the exact integral.
    const int last = ((total - 1) / kBlock) * kBlock;
    INFO("z " << first.back() << " expected " << -expectedDistance(poseTime(last)));
    CHECK(std::abs(first.back() + expectedDistance(poseTime(last))) < 0.015);

    // Once played, the recorded automation is used: the layer lands on the
    // same path whichever track is processed first. (The first pass can be
    // off by the automation's change within one 10 ms bin, here 0.1 mm.)
    const auto second = play(true);
    const auto third = play(false);
    CHECK(worstDiff(first, second) < 1e-3f);
    CHECK(worstDiff(second, third) < 1e-5f);

    SECTION("starting in the middle lands where playing through got to") {
        const int start = (static_cast<int>(5 * kRate) / kBlock) * kBlock;
        ph.pos = start;
        buf.clear();
        a->processBlock(buf, midi);
        CHECK(std::abs(a->listenerPose()[2] + expectedDistance(poseTime(start))) < 0.015);
    }
}

TEST_CASE("Path Speed automation moves a layer along its own path by what was played, and is saved with the track") {
    FreshSession fs;
    PlayHead ph;
    juce::MemoryBlock state;
    json d = straightDoc();
    d["listener"]["paths"] = json::array();
    d["layers"] = json::array({doc::defaultLayer(0, "", {0, 1.6f, -2})});
    d["layers"][0]["motion"] = {{"path", {{"name", ""}, {"closed", false},
                                          {"segments", json::array({json{{"type", "line"}, {"points", {{0, 1.6, -2}, {60, 1.6, -2}}}}})}}},
                                {"timing", "speed"}, {"speed", json::array({json{{"time", 0}, {"speed", 1.0}, {"easing", "linear"}}})}};
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    const int total = static_cast<int>(6 * kRate);
    const int last = ((total - 1) / kBlock) * kBlock;
    {
        auto inst = makeInstance(ph);
        inst->setRole(SpatialPannerProcessor::Role::Scene, true);   // holds the scene and plays the layer
        inst->tickForTesting();
        d["layers"][0]["host_id"] = inst->layerId();
        std::string error;
        REQUIRE(inst->setSceneDoc(d, error));
        tickAll({inst.get()}, 2);
        waitEngines({inst.get()});
        auto* speed = inst->parameters().getParameter("lpspeed");
        for (int pos = 0; pos < total; pos += kBlock) {
            ph.pos = pos;
            speed->setValueNotifyingHost(speed->convertTo0to1(speedAt(pos / kRate)));
            buf.clear();
            inst->processBlock(buf, midi);
        }
        // The distance is the integral of the automated speed (50 ms bins:
        // within a few centimetres of the exact integral on this ramp).
        const double travelled = inst->engineForTesting().pathTravel();
        INFO("travelled " << travelled << " expected " << expectedDistance(poseTime(last)));
        CHECK(std::abs(travelled - expectedDistance(poseTime(last))) < 0.05);
        // Located to 5 s with the transport stopped and the parameter at x1:
        // the recording places it.
        speed->setValueNotifyingHost(speed->convertTo0to1(1.0f));
        ph.playing = false;
        ph.pos = static_cast<juce::int64>(5 * kRate / kBlock) * kBlock;
        buf.clear();
        inst->processBlock(buf, midi);
        CHECK(std::abs(inst->engineForTesting().pathTravel() - expectedDistance(poseTime(static_cast<int>(ph.pos)))) < 0.05);
        inst->getStateInformation(state);

        SECTION("Clear recorded automation forgets it") {
            inst->clearAutomationHistory();
            buf.clear();
            inst->processBlock(buf, midi);
            CHECK(std::abs(inst->engineForTesting().pathTravel() - poseTime(static_cast<int>(ph.pos))) < 0.05);
        }
    }
    SECTION("a new Logic process restores it") {
        SharedSession::detachAllForTesting();
        auto again = makeInstance(ph);
        again->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        tickAll({again.get()}, 2);
        waitEngines({again.get()});
        auto* speed = again->parameters().getParameter("lpspeed");
        speed->setValueNotifyingHost(speed->convertTo0to1(1.0f));
        ph.playing = false;
        ph.pos = static_cast<juce::int64>(5 * kRate / kBlock) * kBlock;
        buf.clear();
        again->processBlock(buf, midi);
        CHECK(std::abs(again->engineForTesting().pathTravel() - expectedDistance(poseTime(static_cast<int>(ph.pos)))) < 0.05);
    }
}

TEST_CASE("Head turn automation reaches layer instances at the timeline time it was played") {
    FreshSession fs;
    PlayHead ph;
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);
    auto a = makeInstance(ph);
    a->setRole(SpatialPannerProcessor::Role::Layer);
    tickAll({scene.get(), a.get()}, 2);
    std::string error;
    json d = straightDoc();
    d["listener"]["paths"] = json::array();   // stands still, facing -z
    REQUIRE(scene->setSceneDoc(d, error));
    tickAll({scene.get(), a.get()}, 2);
    waitEngines({scene.get(), a.get()});

    auto* yaw = scene->parameters().getParameter("yaw");
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    auto yawAt = [](double t) { return t < 1 ? 0.0f : 60.0f; };
    for (int pass = 0; pass < 2; ++pass) {
        for (int pos = 0; pos < static_cast<int>(2 * kRate); pos += kBlock) {
            ph.pos = pos;
            yaw->setValueNotifyingHost(yaw->convertTo0to1(yawAt(pos / kRate)));
            buf.clear();
            if (pass == 0) { scene->processBlock(buf, midi); buf.clear(); a->processBlock(buf, midi); }
            else { a->processBlock(buf, midi); buf.clear(); scene->processBlock(buf, midi); }
            if (pass == 1 && pos > kRate * 1.1) {
                INFO("t=" << pos / kRate << " yaw " << a->listenerPose()[3]);
                REQUIRE(std::abs(a->listenerPose()[3] - 60.0f) < 0.5f);
            }
            if (pass == 1 && pos < kRate * 0.9) REQUIRE(std::abs(a->listenerPose()[3]) < 0.5f);
        }
    }
}

TEST_CASE("The scene and its recorded automation survive saving and reloading the session") {
    FreshSession fs;
    PlayHead ph;
    juce::MemoryBlock state;
    std::string history;
    json saved;
    {
        auto scene = makeInstance(ph);
        scene->setRole(SpatialPannerProcessor::Role::Scene, true);
        scene->tickForTesting();
        std::string error;
        REQUIRE(scene->setSceneDoc(straightDoc(), error));
        auto* speed = scene->parameters().getParameter("speed");
        speed->setValueNotifyingHost(speed->convertTo0to1(2.0f));
        juce::AudioBuffer<float> buf(2, kBlock);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < static_cast<int>(kRate); pos += kBlock) {
            ph.pos = pos;
            scene->processBlock(buf, midi);
        }
        history = scene->session().encodeHistory();
        saved = scene->sceneDoc();
        scene->getStateInformation(state);
    }
    SharedSession::detachAllForTesting();   // a new Logic process
    auto again = makeInstance(ph);
    again->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(again->role() == SpatialPannerProcessor::Role::Scene);
    CHECK(again->sceneDoc() == saved);
    CHECK(again->session().encodeHistory() == history);
    CHECK(!history.empty());
}

TEST_CASE("Without a scene track, a layer keeps playing the scene saved with it") {
    FreshSession fs;
    PlayHead ph;
    juce::MemoryBlock state;
    std::string id;
    {
        auto scene = makeInstance(ph);
        scene->setRole(SpatialPannerProcessor::Role::Scene, false);
        auto a = makeInstance(ph);
        a->setRole(SpatialPannerProcessor::Role::Layer);
        tickAll({scene.get(), a.get()}, 3);
        std::string error;
        json d = scene->sceneDoc();
        d["room"]["type"] = "outdoor";
        REQUIRE(scene->setSceneDoc(d, error));
        tickAll({scene.get(), a.get()}, 2);
        CHECK(a->sceneDoc()["room"]["type"] == "outdoor");
        a->getStateInformation(state);
        id = a->layerId();
    }
    auto b = makeInstance(ph);
    b->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    tickAll({b.get()}, 2);
    CHECK(b->role() == SpatialPannerProcessor::Role::Layer);
    CHECK(b->layerId() == id);
    CHECK(b->sceneDoc()["room"]["type"] == "outdoor");
    waitEngines({b.get()});
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    float peak = 0;
    for (int pos = 0; pos < 8 * kBlock; pos += kBlock) {
        ph.pos = pos;
        buf.clear();
        for (int k = 0; k < kBlock; ++k) buf.setSample(0, k, std::sin(0.05f * static_cast<float>(pos + k)) * 0.5f);
        b->processBlock(buf, midi);
        peak = std::max(peak, buf.getMagnitude(0, kBlock));
    }
    CHECK(peak > 1e-3f);
}

TEST_CASE("A layer on a 7.1.4 track renders to the speakers and leaves the LFE alone") {
    FreshSession fs;
    PlayHead ph;
    auto a = makeInstance(ph, juce::AudioChannelSet::create7point1point4());
    a->setRole(SpatialPannerProcessor::Role::Layer);
    a->tickForTesting();
    waitEngines({a.get()});
    juce::AudioBuffer<float> buf(12, kBlock);
    juce::MidiBuffer midi;
    std::vector<double> energy(12);
    const auto set = juce::AudioChannelSet::create7point1point4();
    for (int pos = 0; pos < 40 * kBlock; pos += kBlock) {
        ph.pos = pos;
        buf.clear();
        for (int k = 0; k < kBlock; ++k) buf.setSample(0, k, std::sin(0.07f * static_cast<float>(pos + k)) * 0.5f);
        a->processBlock(buf, midi);
        for (int c = 0; c < 12; ++c) energy[static_cast<size_t>(c)] += buf.getRMSLevel(c, 0, kBlock);
    }
    const int lfe = set.getChannelIndexForType(juce::AudioChannelSet::LFE);
    double total = 0;
    for (double e : energy) total += e;
    CHECK(total > 0.01);
    CHECK(energy[static_cast<size_t>(lfe)] < total * 1e-6);
}

TEST_CASE("In a room with objects, ray tracing runs on a worker in real time and in line during a bounce") {
    if (!sp::Renderer::steamAudioAvailable()) SKIP("built without Steam Audio");
    FreshSession fs;
    PlayHead ph;
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);
    auto a = makeInstance(ph);
    a->setRole(SpatialPannerProcessor::Role::Layer);
    tickAll({scene.get(), a.get()}, 3);
    std::string error;
    json d = loadDoc("occluder.json");
    d["layers"] = json::array();
    REQUIRE(scene->setSceneDoc(d, error));
    tickAll({scene.get(), a.get()}, 3);
    REQUIRE(a->sceneDoc()["room"]["objects"].size() == 3);
    waitEngines({a.get()});

    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    auto play = [&](int blocks) {
        float peak = 0;
        for (int i = 0; i < blocks; ++i) {
            ph.pos = static_cast<juce::int64>(i) * kBlock;
            buf.clear();
            for (int k = 0; k < kBlock; ++k) buf.setSample(0, k, std::sin(0.05f * static_cast<float>(ph.pos + k)) * 0.5f);
            a->processBlock(buf, midi);
            peak = std::max(peak, buf.getMagnitude(0, kBlock));
        }
        return peak;
    };
    CHECK(play(4) > 1e-3f);
    CHECK(a->engineForTesting().usesSteamAudio());
    CHECK_FALSE(a->engineForTesting().simulatesInline());

    // Logic switches to an offline bounce: the next block waits for a
    // renderer that simulates in line.
    a->setNonRealtime(true);
    CHECK(play(4) > 1e-3f);
    CHECK(a->engineForTesting().simulatesInline());

    a->setNonRealtime(false);
    waitEngines({a.get()});
    play(1);
    CHECK_FALSE(a->engineForTesting().simulatesInline());
}

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juce;
    return Catch::Session().run(argc, argv);
}


// Helpers for the Ambisonic tests: a first-order ambiX plane wave from the
// recording's left (W and Y carry the signal, in phase), and left/right ear
// energies from a layer instance over `total` frames.
namespace {

std::pair<double, double> earEnergies(SpatialPannerProcessor& layer, PlayHead& ph, int inCh, int total,
                                      const std::function<void(juce::AudioBuffer<float>&, int)>& fill) {
    juce::AudioBuffer<float> buf(std::max(2, inCh), kBlock);
    juce::MidiBuffer midi;
    double l = 0, r = 0;
    for (int pos = 0; pos < total; pos += kBlock) {
        ph.pos = pos;
        buf.clear();
        fill(buf, pos);
        layer.processBlock(buf, midi);
        if (pos < static_cast<int>(kRate) / 2) continue;
        for (int k = 0; k < kBlock; ++k) { l += std::pow(buf.getSample(0, k), 2); r += std::pow(buf.getSample(1, k), 2); }
    }
    return {l, r};
}

}  // namespace

TEST_CASE("A quad track is a first-order Ambisonic sphere: the field turns with the Sphere Rotation parameter") {
    FreshSession fs;
    PlayHead ph;
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);
    auto layer = makeInstance(ph, juce::AudioChannelSet::stereo(), juce::AudioChannelSet::quadraphonic());
    layer->setRole(SpatialPannerProcessor::Role::Layer);
    std::vector<SpatialPannerProcessor*> all{scene.get(), layer.get()};
    tickAll(all, 3);
    pump(50);
    tickAll(all, 2);

    // The track's four channels reach the scene document as an Ambisonic layer with its sphere.
    json d = scene->sceneDoc();
    REQUIRE(d["layers"].size() == 1);
    CHECK(d["layers"][0].value("channels", 1) == 4);
    REQUIRE(d["layers"][0].contains("ambisonic"));
    CHECK(d["layers"][0]["ambisonic"].value("radius", 0.0) == 3.0);
    d["room"]["type"] = "none";
    d["listener"]["paths"] = json::array();
    d["listener"]["static_position"] = {0, 1.6, 0};
    d["layers"][0]["position"] = {0, 1.6, 0};   // the listener sits at the sphere's centre
    std::string error;
    REQUIRE(scene->setSceneDoc(d, error));
    tickAll(all, 2);
    waitEngines(all);

    // A sound on the recording's left (ambiX: W and Y = ACN 1 in phase).
    const int total = static_cast<int>(kRate) * 2;
    const auto in = noise(1, total);
    const auto fill = [&](juce::AudioBuffer<float>& buf, int pos) {
        buf.copyFrom(0, 0, in[0].data() + pos, kBlock);
        buf.copyFrom(1, 0, in[0].data() + pos, kBlock);
    };
    auto* rot = layer->parametersForTesting().getParameter("amrot");
    REQUIRE(rot != nullptr);
    const auto left = earEnergies(*layer, ph, 4, total, fill);
    REQUIRE(left.first > 1e-6);
    CHECK(10 * std::log10(left.first / left.second) > 6);
    // Turning the sphere half way round brings it to the right ear.
    rot->setValueNotifyingHost(rot->convertTo0to1(180.0f));
    const auto turned = earEnergies(*layer, ph, 4, total, fill);
    CHECK(10 * std::log10(turned.second / turned.first) > 6);
}

TEST_CASE("A higher-order recording plays from its file on any track, at the host's position") {
    FreshSession fs;
    PlayHead ph;
    // A 9-channel ambiX file: noise from the recording's left on W and Y (ACN 1), 1.5 s long.
    juce::TemporaryFile tmp(".wav");
    const int frames = static_cast<int>(kRate * 1.5);
    {
        const auto n = noise(1, frames, 3);
        juce::AudioBuffer<float> buf(9, frames);
        buf.clear();
        buf.copyFrom(0, 0, n[0].data(), frames);
        buf.copyFrom(1, 0, n[0].data(), frames);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream>(tmp.getFile());
        auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(9).withBitsPerSample(24));
        REQUIRE(w != nullptr);
        REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, frames));
    }
    auto scene = makeInstance(ph);
    scene->setRole(SpatialPannerProcessor::Role::Scene, false);
    auto layer = makeInstance(ph);   // a mono track: its own audio is not used
    layer->setRole(SpatialPannerProcessor::Role::Layer);
    std::vector<SpatialPannerProcessor*> all{scene.get(), layer.get()};
    tickAll(all, 3);
    pump(50);
    tickAll(all, 2);
    json d = scene->sceneDoc();
    REQUIRE(d["layers"].size() == 1);
    d["room"]["type"] = "none";
    d["listener"]["paths"] = json::array();
    d["listener"]["static_position"] = {0, 1.6, 0};
    d["layers"][0]["position"] = {0, 1.6, 0};
    d["layers"][0]["channels"] = 9;
    d["layers"][0]["audio"] = tmp.getFile().getFullPathName().toStdString();
    d["layers"][0]["loop"] = false;
    std::string error;
    REQUIRE(scene->setSceneDoc(d, error));
    tickAll(all, 2);
    waitEngines(all);
    // The track's channel count no longer overrides the file-fed layer.
    CHECK(scene->sceneDoc()["layers"][0].value("channels", 1) == 9);
    // Wait for the file to decode.
    for (int i = 0; i < 200 && layer->fileForTesting().loading(); ++i) pump(50);
    REQUIRE_FALSE(layer->fileForTesting().loading());
    CHECK(layer->fileForTesting().error().isEmpty());

    // Silence on the track, the recording from the file: heard on the left
    // while the file lasts, then nothing (the file does not loop).
    const int total = static_cast<int>(kRate) * 3;
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    double l = 0, r = 0, late = 0;
    for (int pos = 0; pos < total; pos += kBlock) {
        ph.pos = pos;
        buf.clear();
        layer->processBlock(buf, midi);
        for (int k = 0; k < kBlock; ++k) {
            const double a = std::pow(buf.getSample(0, k), 2), b = std::pow(buf.getSample(1, k), 2);
            if (pos + k < static_cast<int>(kRate) / 2) continue;
            if (pos + k < frames) { l += a; r += b; } else if (pos + k > frames + static_cast<int>(kRate) / 2) late += a + b;
        }
    }
    REQUIRE(l > 1e-6);
    CHECK(10 * std::log10(l / r) > 6);
    CHECK(late < 1e-6 * l);
}
