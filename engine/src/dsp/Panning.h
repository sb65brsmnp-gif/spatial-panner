// Loudspeaker and binaural decoding built on SAF: VBAP gain tables, AllRAD
// Ambisonics-to-loudspeaker matrices, and magnitude-least-squares
// Ambisonics-to-binaural filters designed from the loaded HRTF set.
#pragma once

#include <vector>

#include "dsp/Hrtf.h"
#include "sp/Math.h"
#include "sp/SpeakerLayout.h"

namespace sp::dsp {

// Gains for the spatial (non-LFE) speakers of a layout; LFE entries are
// always 0. Output arrays have layout.numChannels() entries.
class VbapPanner {
public:
    void init(const SpeakerLayout& layout, int resolutionDeg, float spreadDeg);
    // Direction in head-local engine coordinates.
    void gains(const Vec3& localDir, float* out) const;
    int numChannels() const { return numChannels_; }

private:
    int numChannels_ = 0, numSpatial_ = 0, res_ = 1, nAzi_ = 0, nElev_ = 0;
    bool planar_ = false;
    std::vector<int> spatialIndex_;  // spatial speaker -> layout channel
    std::vector<float> table_;       // N_gtable x numSpatial
    std::vector<float> stereoAz_;    // 2-speaker fallback
};

// Ambisonics (N3D/ACN) -> loudspeaker decoder matrix, numChannels x nSH.
class AmbiSpeakerDecoder {
public:
    void init(const SpeakerLayout& layout, int order);
    int numChannels() const { return numChannels_; }
    int numSh() const { return nSh_; }
    const float* row(int channel) const { return matrix_.data() + static_cast<size_t>(channel) * nSh_; }

private:
    int numChannels_ = 0, nSh_ = 0;
    std::vector<float> matrix_;
};

// Ambisonics -> binaural decoding filters (MagLS), one per (ear, SH channel).
struct AmbiBinauralFilters {
    int order = 0, nSh = 0, length = 0;
    std::vector<float> filters;  // [ear][sh][length]
    const float* filter(int ear, int sh) const {
        return filters.data() + (static_cast<size_t>(ear) * nSh + sh) * length;
    }
};
AmbiBinauralFilters designAmbiBinauralFilters(const HrtfSet& hrtf, int order, int filterLength);

}  // namespace sp::dsp
