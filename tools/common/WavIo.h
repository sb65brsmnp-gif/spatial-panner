// WAV reading/writing for the command-line tools (dr_wav).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

namespace sp::tools {

struct AudioFile {
    int channels = 0;
    int sampleRate = 0;
    std::vector<std::vector<float>> data;  // [channel][frame]
    size_t frames() const { return data.empty() ? 0 : data[0].size(); }
};

inline AudioFile readWav(const std::string& path) {
    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr))
        throw std::runtime_error("Cannot open WAV file '" + path + "'");
    AudioFile f;
    f.channels = static_cast<int>(wav.channels);
    f.sampleRate = static_cast<int>(wav.sampleRate);
    const size_t frames = static_cast<size_t>(wav.totalPCMFrameCount);
    std::vector<float> inter(frames * wav.channels);
    const drwav_uint64 got = drwav_read_pcm_frames_f32(&wav, frames, inter.data());
    drwav_uninit(&wav);
    f.data.assign(f.channels, std::vector<float>(got));
    for (size_t i = 0; i < got; ++i)
        for (int c = 0; c < f.channels; ++c) f.data[c][i] = inter[i * wav.channels + c];
    return f;
}

// 24-bit PCM, or 32-bit float when `floatFormat` is set.
inline void writeWav(const std::string& path, const std::vector<std::vector<float>>& data, int sampleRate,
                     bool floatFormat = false) {
    if (data.empty()) throw std::runtime_error("No channels to write");
    const int channels = static_cast<int>(data.size());
    const size_t frames = data[0].size();
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format = floatFormat ? DR_WAVE_FORMAT_IEEE_FLOAT : DR_WAVE_FORMAT_PCM;
    fmt.channels = static_cast<drwav_uint32>(channels);
    fmt.sampleRate = static_cast<drwav_uint32>(sampleRate);
    fmt.bitsPerSample = floatFormat ? 32 : 24;
    drwav wav;
    if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr))
        throw std::runtime_error("Cannot write WAV file '" + path + "'");
    if (floatFormat) {
        std::vector<float> inter(frames * channels);
        for (size_t i = 0; i < frames; ++i)
            for (int c = 0; c < channels; ++c) inter[i * channels + c] = data[c][i];
        drwav_write_pcm_frames(&wav, frames, inter.data());
    } else {
        std::vector<uint8_t> inter(frames * channels * 3);
        for (size_t i = 0; i < frames; ++i) {
            for (int c = 0; c < channels; ++c) {
                float v = data[c][i];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                const int32_t s = static_cast<int32_t>(v * 8388607.0f);
                uint8_t* p = inter.data() + (i * channels + c) * 3;
                p[0] = static_cast<uint8_t>(s & 0xff);
                p[1] = static_cast<uint8_t>((s >> 8) & 0xff);
                p[2] = static_cast<uint8_t>((s >> 16) & 0xff);
            }
        }
        drwav_write_pcm_frames(&wav, frames, inter.data());
    }
    drwav_uninit(&wav);
}

}  // namespace sp::tools
