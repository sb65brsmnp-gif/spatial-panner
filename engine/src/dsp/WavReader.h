// Minimal WAV (RIFF/WAVE) reader for impulse responses: PCM 8/16/24/32-bit
// and IEEE float 32/64-bit, plain or WAVE_FORMAT_EXTENSIBLE, any channel
// count. The engine has no JUCE and the tools' dr_wav stays in the tools.
#pragma once

#include <string>
#include <vector>

namespace sp::dsp {

struct WavInfo {
    int channels = 0;
    int sampleRate = 0;
    long frames = 0;
    int bitsPerSample = 0;
    bool isFloat = false;
    double seconds() const { return sampleRate > 0 ? static_cast<double>(frames) / sampleRate : 0.0; }
};

struct WavData {
    WavInfo info;
    std::vector<std::vector<float>> channels;  // [channel][frame], -1..1
};

// Header only (cheap). Throws std::runtime_error on I/O or format errors.
WavInfo readWavInfo(const std::string& path);

// Whole file as floats. Throws std::runtime_error on I/O or format errors.
WavData readWav(const std::string& path);

}  // namespace sp::dsp
