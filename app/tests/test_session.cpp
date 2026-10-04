// The app's audio session driven the way the editor and an audio device
// drive it, without a window or real audio hardware: a fake device calls the
// audio callback, the message loop is pumped by hand.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <cmath>

#include "Session.h"

using namespace spapp;

namespace {

class FakeDevice : public juce::AudioIODevice {
public:
    FakeDevice(double rate, int block) : AudioIODevice("Fake", "Fake"), rate_(rate), block_(block) {}
    juce::StringArray getOutputChannelNames() override { return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {rate_}; }
    juce::Array<int> getAvailableBufferSizes() override { return {block_}; }
    int getDefaultBufferSize() override { return block_; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return block_; }
    double getCurrentSampleRate() override { return rate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 12, true); return b; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
private:
    double rate_;
    int block_;
};

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

void waitIdle(Session& s) {
    for (int i = 0; i < 400 && s.busy(); ++i) pump(25);
    pump(60);
}

juce::File writeNoiseWav(const juce::File& dir, const juce::String& name, double seconds, int channels = 1) {
    const auto f = dir.getChildFile(name + ".wav");
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream>(f);
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(44100).withNumChannels(channels).withBitsPerSample(16));
    juce::AudioBuffer<float> b(channels, static_cast<int>(seconds * 44100));
    juce::Random r(42);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(c, i, (r.nextFloat() * 2 - 1) * 0.3f);
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    return f;
}

struct Rig {
    juce::AudioDeviceManager dm;
    Session s{dm};
    FakeDevice dev{48000, 512};
    std::vector<std::vector<float>> buf = std::vector<std::vector<float>>(12, std::vector<float>(512));
    std::vector<float*> ptrs;
    juce::AudioIODeviceCallbackContext ctx{};

    Rig() {
        for (auto& b : buf) ptrs.push_back(b.data());
        s.audioDeviceAboutToStart(&dev);
    }
    ~Rig() { s.audioDeviceStopped(); }

    // Runs the audio callback for `seconds`; returns channel 0..n-1 concatenated per channel.
    std::vector<std::vector<float>> run(double seconds, int channels = 2) {
        std::vector<std::vector<float>> out(static_cast<size_t>(channels));
        const int blocks = static_cast<int>(seconds * 48000 / 512);
        for (int b = 0; b < blocks; ++b) {
            s.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs.data(), 12, 512, ctx);
            for (int c = 0; c < channels; ++c) out[static_cast<size_t>(c)].insert(out[static_cast<size_t>(c)].end(), buf[static_cast<size_t>(c)].begin(), buf[static_cast<size_t>(c)].end());
            if (b % 8 == 0) pump(1);
        }
        return out;
    }
};

double rms(const std::vector<float>& v, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > v.size()) to = v.size();
    double e = 0;
    for (size_t i = from; i < to; ++i) e += static_cast<double>(v[i]) * v[i];
    return std::sqrt(e / std::max<size_t>(1, to - from));
}

float maxStep(const std::vector<float>& v) {
    float m = 0;
    for (size_t i = 1; i < v.size(); ++i) m = std::max(m, std::abs(v[i] - v[i - 1]));
    return m;
}

bool finite(const std::vector<float>& v) {
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

sp::Scene testScene(const juce::File& dir) {
    sp::Scene s;
    s.duration = 20;
    s.room.size = {10, 3, 12};
    for (int i = 0; i < 3; ++i) {
        sp::Layer l;
        l.name = "layer" + std::to_string(i);
        l.audioFile = writeNoiseWav(dir, l.name, 3.0, i == 1 ? 2 : 1).getFullPathName().toStdString();
        l.position = {-3.0f + 3.0f * i, 1.6f, -3.0f};
        l.loop = true;
        l.levelDb = -6;
        s.layers.push_back(l);
    }
    sp::Path p;
    p.segments.push_back({sp::SegmentType::Line, {{0, 1.7f, 4}, {0, 1.7f, -4}}});
    s.listener.paths.push_back(p);
    s.listener.speed.keys = {{0.0, 1.0f, sp::Easing::Linear}};
    return s;
}

}  // namespace

TEST_CASE("Session plays a scene, takes live edits without rebuilding, and crossfades rebuilds") {
    juce::TemporaryFile tmpDir;
    const auto dir = tmpDir.getFile();
    dir.createDirectory();
    Rig rig;
    auto& s = rig.s;
    if (!Session::hrtfFile().existsAsFile()) SKIP("HRTF not available");

    sp::Scene scene = testScene(dir);
    s.setScene(scene, 20);
    waitIdle(s);
    REQUIRE(s.buildsStarted() == 1);

    // Stopped: silence.
    auto quiet = rig.run(0.2);
    CHECK(rms(quiet[0]) == 0.0);

    s.transport(Session::Transport::Play);
    auto a = rig.run(1.0);
    CHECK(finite(a[0]));
    CHECK(rms(a[0], 4800) > 1e-3);
    CHECK(rms(a[1], 4800) > 1e-3);
    auto t = s.tick();
    CHECK(t.playing);
    CHECK(std::abs(t.time - 1.0) < 0.05);
    CHECK(t.pose[2] < 3.1f);        // walked forward from z = 4 at 1 m/s
    CHECK(t.pose[2] > 2.9f);
    REQUIRE(t.metersDb.size() == 3);
    CHECK(t.metersDb[0] > -30.0f);

    // A live edit (move a layer, change a level, turn the head) patches the running renderer.
    scene.layers[0].position = {-2, 1.6f, -1};
    scene.layers[2].levelDb = -20;
    scene.listener.head.yawOffsetDeg = 45;
    s.setScene(scene, 20);
    pump(20);
    auto b = rig.run(0.5);
    CHECK(s.buildsStarted() == 1);
    CHECK(finite(b[0]));
    // No clicks: the largest sample-to-sample step stays in the range of
    // the steady noise before the edit.
    const float steady = maxStep(a[0]);
    INFO("steady " << steady << " after edit " << maxStep(b[0]));
    CHECK(maxStep(b[0]) < 1.5f * steady);
    t = s.tick();
    CHECK(std::abs(t.pose[3] - 45.0f) < 1.0f);  // head yaw now includes the offset

    // A room change needs a new renderer: built in the background, crossfaded in.
    scene.room.materials[sp::WallNegZ] = sp::materials::byName("curtain");
    s.setScene(scene, 20);
    auto during = rig.run(0.3);  // keeps playing the old program meanwhile
    waitIdle(s);
    auto c = rig.run(0.5);
    CHECK(s.buildsStarted() == 2);
    CHECK(finite(during[0]));
    CHECK(finite(c[0]));
    CHECK(rms(c[0]) > 1e-3);
    INFO("during rebuild " << maxStep(during[0]) << " after " << maxStep(c[0]));
    CHECK(maxStep(during[0]) < 1.5f * steady);
    CHECK(maxStep(c[0]) < 1.5f * steady);

    // Switching to speakers rebuilds with 12 outputs.
    s.setOutput({sp::OutputMode::Speakers, "7.1.4"});
    waitIdle(s);
    auto d = rig.run(0.5, 12);
    CHECK(s.buildsStarted() == 3);
    double total = 0;
    for (int ch = 0; ch < 12; ++ch) if (ch != 3) total += rms(d[static_cast<size_t>(ch)], 12000);
    CHECK(total > 1e-3);

    // Pause: the inputs fade out, the room tail rings on and then stops.
    s.transport(Session::Transport::Pause);
    auto e = rig.run(4.0);
    CHECK(rms(e[0], 0, 2400) > 0.0);
    CHECK(rms(e[0], 48000 * 3 + 24000) == 0.0);
    CHECK(!s.tick().playing);

    // Seek moves the playhead.
    s.transport(Session::Transport::Seek, 10.0);
    rig.run(0.05);
    CHECK(std::abs(s.tick().time - 10.0) < 0.06);
}

TEST_CASE("Session plays a room with objects through the ray-traced back-end, and live edits keep it") {
    if (!sp::Renderer::steamAudioAvailable()) SKIP("built without Steam Audio");
    juce::TemporaryFile tmpDir;
    const auto dir = tmpDir.getFile();
    dir.createDirectory();
    Rig rig;
    auto& s = rig.s;
    if (!Session::hrtfFile().existsAsFile()) SKIP("HRTF not available");

    sp::Scene scene = testScene(dir);
    sp::SceneObject wall;
    wall.name = "partition";
    wall.minCorner = {-1, 0, -2};
    wall.maxCorner = {1, 3, -1.8f};
    wall.material = sp::materials::byName("brick");
    scene.room.objects.push_back(wall);
    s.setScene(scene, 20);
    waitIdle(s);
    REQUIRE(s.buildsStarted() == 1);
    s.transport(Session::Transport::Play);
    auto a = rig.run(1.5);
    CHECK(finite(a[0]));
    CHECK(finite(a[1]));
    CHECK(rms(a[0], 9600) > 1e-3);

    // Moving a layer patches the running ray-traced renderer; moving the wall rebuilds it.
    scene.layers[0].position = {-2, 1.6f, -1};
    s.setScene(scene, 20);
    pump(20);
    auto b = rig.run(0.5);
    CHECK(s.buildsStarted() == 1);
    CHECK(finite(b[0]));
    scene.room.objects[0].maxCorner.y = 2.5f;
    s.setScene(scene, 20);
    waitIdle(s);
    auto c = rig.run(1.0);
    CHECK(s.buildsStarted() == 2);
    CHECK(finite(c[0]));
    CHECK(rms(c[0], 9600) > 1e-3);
}

TEST_CASE("Session bounces binaural and 7.1.4 WAV files") {
    juce::TemporaryFile tmpDir;
    const auto dir = tmpDir.getFile();
    dir.createDirectory();
    Rig rig;
    auto& s = rig.s;
    if (!Session::hrtfFile().existsAsFile()) SKIP("HRTF not available");
    s.setScene(testScene(dir), 20);
    waitIdle(s);

    for (auto [mode, channels] : {std::pair{sp::OutputMode::Binaural, 2}, std::pair{sp::OutputMode::Speakers, 12}}) {
        const auto out = dir.getChildFile("bounce" + juce::String(channels) + ".wav");
        juce::String result = "pending";
        s.bounce(out, {mode, "7.1.4"}, 1.0, 3.0, 48000, [](float) {}, [&](juce::String e) { result = e; });
        for (int i = 0; i < 800 && result == "pending"; ++i) pump(25);
        REQUIRE(result.isEmpty());
        // Rendered into a hidden file and moved into place at the end.
        CHECK_FALSE(dir.getChildFile(".bounce" + juce::String(channels) + ".partial.wav").exists());
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(out));
        REQUIRE(r);
        CHECK(static_cast<int>(r->numChannels) == channels);
        CHECK(std::abs(static_cast<double>(r->lengthInSamples) - 96000.0) < 2);
        juce::AudioBuffer<float> buf(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
        r->read(&buf, 0, buf.getNumSamples(), 0, true, true);
        CHECK(buf.getRMSLevel(0, 0, buf.getNumSamples()) > 1e-3f);
    }

    // A bounce that fails leaves no file under the chosen name.
    const auto bad = dir.getChildFile("missing-folder").getChildFile("bounce.wav");
    juce::String result = "pending";
    s.bounce(bad, {sp::OutputMode::Binaural, "7.1.4"}, 0.0, 1.0, 48000, [](float) {}, [&](juce::String e) { result = e; });
    for (int i = 0; i < 800 && result == "pending"; ++i) pump(25);
    CHECK(result.isNotEmpty());
    CHECK_FALSE(bad.exists());
}

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI init;
    return Catch::Session().run(argc, argv);
}
