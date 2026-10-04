#include "AudioLibrary.h"

#include <cmath>

#include "saf.h"  // speex resampler, as used by sp-render

namespace spapp {

AudioLibrary::AudioLibrary() { formats_.registerBasicFormats(); }

AudioLibrary::~AudioLibrary() {
    *alive_ = false;
    pool_.removeAllJobs(true, 10000);
}

AudioFileInfo AudioLibrary::info(const juce::String& path) {
    AudioFileInfo i;
    i.path = path;
    const juce::File f(path);
    i.name = f.getFileNameWithoutExtension();
    if (!f.existsAsFile()) { i.error = "File not found"; return i; }
    std::unique_ptr<juce::AudioFormatReader> r(formats_.createReaderFor(f));
    if (!r) { i.error = "Unsupported audio format"; return i; }
    i.channels = static_cast<int>(r->numChannels);
    i.sampleRate = r->sampleRate;
    i.duration = r->sampleRate > 0 ? static_cast<double>(r->lengthInSamples) / r->sampleRate : 0;
    return i;
}

std::shared_ptr<const LayerAudio> AudioLibrary::decode(juce::AudioFormatManager& fm, const juce::String& path, double rate,
                                                       juce::String& error) {
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(juce::File(path)));
    if (!r) { error = juce::File(path).existsAsFile() ? "Unsupported audio format" : "File not found"; return nullptr; }
    const auto n = static_cast<int>(std::min<juce::int64>(r->lengthInSamples, std::numeric_limits<int>::max() / 2));
    const int fileCh = static_cast<int>(r->numChannels);
    const int ch = std::min(fileCh, 16);  // mono, left and right, or an Ambisonic recording (up to third order)
    juce::AudioBuffer<float> buf(fileCh, n);
    r->read(&buf, 0, n, 0, true, true);
    auto out = std::make_shared<LayerAudio>();
    out->sampleRate = rate;
    out->channels.resize(static_cast<size_t>(ch));
    const int from = static_cast<int>(std::lround(r->sampleRate)), to = static_cast<int>(std::lround(rate));
    for (int c = 0; c < ch; ++c) {
        const float* src = buf.getReadPointer(c);
        auto& dst = out->channels[static_cast<size_t>(c)];
        if (from == to || from <= 0) {
            dst.assign(src, src + n);
            continue;
        }
        int err = 0;
        SpeexResamplerState* st = speex_resampler_init(1, static_cast<spx_uint32_t>(from), static_cast<spx_uint32_t>(to), 8, &err);
        if (!st) { error = "Resampler failed"; return nullptr; }
        dst.resize(static_cast<size_t>(std::ceil(n * static_cast<double>(to) / from)) + 64);
        spx_uint32_t il = static_cast<spx_uint32_t>(n), ol = static_cast<spx_uint32_t>(dst.size());
        speex_resampler_process_float(st, 0, src, &il, dst.data(), &ol);
        speex_resampler_destroy(st);
        dst.resize(ol);
    }
    return out;
}

std::shared_ptr<const LayerAudio> AudioLibrary::get(const juce::String& path) {
    if (path.isEmpty()) return nullptr;
    auto& e = entries_[path];
    if (e.audio || e.loading || e.error.isNotEmpty()) return e.audio;
    e.loading = true;
    const double rate = rate_;
    const int gen = generation_;
    auto alive = alive_;
    pool_.addJob([this, path, rate, gen, alive] {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        juce::String error;
        auto audio = decode(fm, path, rate, error);
        juce::MessageManager::callAsync([this, path, gen, alive, audio, error] {
            if (!*alive || gen != generation_) return;
            auto& en = entries_[path];
            en.loading = false;
            en.audio = audio;
            en.error = error;
            if (onLoaded) onLoaded();
        });
    });
    return nullptr;
}

void AudioLibrary::setTargetRate(double rate) {
    if (std::abs(rate - rate_) < 0.5) return;
    rate_ = rate;
    ++generation_;
    entries_.clear();
}

bool AudioLibrary::anyLoading() const {
    for (const auto& [k, e] : entries_)
        if (e.loading) return true;
    return false;
}

}  // namespace spapp
