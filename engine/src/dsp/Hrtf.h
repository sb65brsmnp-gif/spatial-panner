// HRTF set loaded from a SOFA file (libmysofa), resampled to the engine rate.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sp/Math.h"

struct MYSOFA_EASY;

namespace sp::dsp {

class HrtfSet {
public:
    HrtfSet() = default;
    ~HrtfSet();
    HrtfSet(const HrtfSet&) = delete;
    HrtfSet& operator=(const HrtfSet&) = delete;

    // Throws std::runtime_error on failure.
    void load(const std::string& sofaPath, float sampleRate);

    bool loaded() const { return easy_ != nullptr; }
    int filterLength() const { return filterLength_; }
    float sampleRate() const { return sampleRate_; }
    int numMeasurements() const { return numMeasurements_; }

    // Interpolated HRIR pair for a direction in head-local engine coordinates
    // (Y up, -Z forward, +X right). `left`/`right` receive filterLength() samples.
    void getFilter(const Vec3& localDir, float* left, float* right) const;

    // Interpolated HRIR pair with a spread: averages HRIRs over a ring of
    // directions `spreadDeg`/2 away from the centre direction.
    void getFilterSpread(const Vec3& localDir, float spreadDeg, float* left, float* right) const;

    // Raw measurement access (for designing Ambisonics-to-binaural decoders).
    // Directions are returned as azimuth/elevation in degrees (SOFA convention).
    Spherical measurementDirection(int index) const;
    const float* measurementIr(int index, int ear) const;  // filterLength() samples

private:
    MYSOFA_EASY* easy_ = nullptr;
    int filterLength_ = 0;
    float sampleRate_ = 48000.0f;
    int numMeasurements_ = 0;
    mutable std::vector<float> scratchL_, scratchR_;
};

}  // namespace sp::dsp
