#include "LayerFile.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "saf.h"  // speex resampler, as the app and sp-render use it

namespace spplug {

namespace {

std::vector<float> resample(const float* src, int n, int from, int to, juce::String& error) {
    std::vector<float> dst;
    if (from == to || from <= 0 || to <= 0) {
        dst.assign(src, src + n);
        return dst;
    }
    int err = 0;
    SpeexResamplerState* st = speex_resampler_init(1, static_cast<spx_uint32_t>(from), static_cast<spx_uint32_t>(to), 8, &err);
    if (!st) { error = "Resampler failed"; return dst; }
    dst.resize(static_cast<size_t>(std::ceil(n * static_cast<double>(to) / from)) + 64);
    spx_uint32_t il = static_cast<spx_uint32_t>(n), ol = static_cast<spx_uint32_t>(dst.size());
    speex_resampler_process_float(st, 0, src, &il, dst.data(), &ol);
    speex_resampler_destroy(st);
    dst.resize(ol);
    return dst;
}

}  // namespace

LayerFile::LayerFile() = default;

LayerFile::~LayerFile() {
    alive_->store(false);
    pool_.removeAllJobs(true, 10000);
}

juce::String LayerFile::error() const {
    const juce::ScopedLock l(errorLock_);
    return error_;
}

std::shared_ptr<const LayerFile::Audio> LayerFile::decode(const std::vector<std::string>& paths, double rate, int channels, juce::String& error) {
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto out = std::make_shared<Audio>();
    out->rate = rate;
    out->channels.resize(static_cast<size_t>(std::max(1, channels)));
    const int to = static_cast<int>(std::lround(rate));
    const bool separate = paths.size() > 1;
    for (size_t i = 0; i < paths.size(); ++i) {
        const juce::File f(juce::String::fromUTF8(paths[i].c_str()));
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
        if (!r) { error = f.getFileName() + ": " + (f.existsAsFile() ? "unsupported audio format" : "file not found"); return nullptr; }
        const auto n = static_cast<int>(std::min<juce::int64>(r->lengthInSamples, std::numeric_limits<int>::max() / 2));
        const int fileCh = static_cast<int>(r->numChannels);
        juce::AudioBuffer<float> buf(fileCh, n);
        r->read(&buf, 0, n, 0, true, true);
        const int from = static_cast<int>(std::lround(r->sampleRate));
        if (separate) {
            // One mono file per channel (a stereo file contributes its first channel).
            if (static_cast<int>(i) >= channels) break;
            out->channels[i] = resample(buf.getReadPointer(0), n, from, to, error);
        } else {
            for (int c = 0; c < std::min(fileCh, channels); ++c) out->channels[static_cast<size_t>(c)] = resample(buf.getReadPointer(c), n, from, to, error);
        }
        if (error.isNotEmpty()) return nullptr;
    }
    // Pad short or missing channels with silence so every channel has the same length.
    size_t frames = 0;
    for (const auto& c : out->channels) frames = std::max(frames, c.size());
    for (auto& c : out->channels) c.resize(frames, 0.0f);
    return out;
}

void LayerFile::load(const std::vector<std::string>& paths, double rate, int channels) {
    if (paths == paths_ && rate == rate_ && channels == channels_) return;
    paths_ = paths;
    rate_ = rate;
    channels_ = channels;
    const int gen = ++generation_;
    loading_.store(true);
    {
        const juce::ScopedLock l(errorLock_);
        error_.clear();
    }
    auto alive = alive_;
    pool_.addJob([this, paths, rate, channels, gen, alive] {
        juce::String err;
        auto audio = decode(paths, rate, channels, err);
        if (!alive->load()) return;
        juce::MessageManager::callAsync([this, gen, alive, audio, err] {
            if (!alive->load() || gen != generation_) return;
            {
                const juce::SpinLock::ScopedLockType l(lock_);
                retired_ = std::move(audio_);
                audio_ = audio;
            }
            retired_.reset();
            {
                const juce::ScopedLock l(errorLock_);
                error_ = err;
            }
            loading_.store(false);
        });
    });
}

void LayerFile::clear() {
    if (paths_.empty() && !audio_) return;
    paths_.clear();
    rate_ = 0;
    channels_ = 0;
    ++generation_;
    loading_.store(false);
    const juce::SpinLock::ScopedLockType l(lock_);
    retired_ = std::move(audio_);
    audio_.reset();
}

bool LayerFile::read(float* const* out, int numCh, int n, juce::int64 pos, juce::int64 start, bool loop) {
    std::shared_ptr<const Audio> a;
    {
        const juce::SpinLock::ScopedTryLockType l(lock_);
        if (l.isLocked()) a = audio_;
    }
    if (!a || a->frames() == 0) {
        for (int c = 0; c < numCh; ++c) std::fill(out[c], out[c] + n, 0.0f);
        return false;
    }
    const auto size = static_cast<juce::int64>(a->frames());
    for (int c = 0; c < numCh; ++c) {
        const auto* src = static_cast<size_t>(c) < a->channels.size() ? a->channels[static_cast<size_t>(c)].data() : nullptr;
        for (int k = 0; k < n; ++k) {
            juce::int64 idx = pos + k - start;
            float v = 0;
            if (src && idx >= 0) {
                if (loop) idx %= size;
                if (idx < size) v = src[idx];
            }
            out[c][k] = v;
        }
    }
    return true;
}

}  // namespace spplug
