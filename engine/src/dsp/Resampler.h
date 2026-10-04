// Offline sample-rate conversion for impulse responses: a Kaiser-windowed
// sinc interpolator (64 taps), good to well under -80 dB of aliasing, which
// is more than an IR needs. Not for the audio thread.
#pragma once

#include <vector>

namespace sp::dsp {

std::vector<float> resample(const std::vector<float>& in, double fromRate, double toRate);

}  // namespace sp::dsp
