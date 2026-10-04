#include "dsp/Hrtf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "mysofa.h"

namespace sp::dsp {

HrtfSet::~HrtfSet() {
    if (easy_) mysofa_close(easy_);
}

void HrtfSet::load(const std::string& sofaPath, float sampleRate) {
    if (easy_) { mysofa_close(easy_); easy_ = nullptr; }
    int filterLength = 0, err = 0;
    // normalise = true scales the set so the loudest frontal HRIR peaks at 1;
    // neighbour search and interpolation on; no resampling if rates match.
    easy_ = mysofa_open_advanced(sofaPath.c_str(), sampleRate, &filterLength, &err, /*norm*/ true,
                                 /*neighbor_angle_step*/ 5.0f, /*neighbor_radius_step*/ 0.01f);
    if (!easy_) {
        throw std::runtime_error("Failed to load SOFA file '" + sofaPath + "' (libmysofa error " +
                                 std::to_string(err) + ")");
    }
    filterLength_ = filterLength;
    sampleRate_ = sampleRate;
    numMeasurements_ = static_cast<int>(easy_->hrtf->M);
    scratchL_.assign(filterLength_, 0.0f);
    scratchR_.assign(filterLength_, 0.0f);
}

void HrtfSet::getFilter(const Vec3& localDir, float* left, float* right) const {
    float c[3];
    toSofaCartesian(localDir.normalized(), c);
    float delayL = 0, delayR = 0;
    mysofa_getfilter_float(easy_, c[0], c[1], c[2], left, right, &delayL, &delayR);
    // SOFA may carry a per-measurement onset delay (Data.Delay). SADIE II stores
    // the ITD inside the IRs (delay 0); when present, apply it as an integer
    // shift so the interaural timing survives.
    auto shift = [this](float* ir, float delaySamples) {
        const int d = static_cast<int>(std::lround(delaySamples));
        if (d <= 0 || d >= filterLength_) return;
        std::memmove(ir + d, ir, sizeof(float) * (filterLength_ - d));
        std::fill(ir, ir + d, 0.0f);
    };
    shift(left, delayL);
    shift(right, delayR);
}

void HrtfSet::getFilterSpread(const Vec3& localDir, float spreadDeg, float* left, float* right) const {
    if (spreadDeg < 1.0f) { getFilter(localDir, left, right); return; }
    const Vec3 d = localDir.normalized();
    // Ring of 6 directions at half the spread angle, plus the centre.
    Vec3 any = std::fabs(d.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 u = d.cross(any).normalized();
    const Vec3 v = d.cross(u).normalized();
    const float half = degToRad(std::min(spreadDeg, 180.0f) * 0.5f);
    const float cw = 0.4f, rw = 0.6f / 6.0f;
    getFilter(d, left, right);
    for (int i = 0; i < filterLength_; ++i) { left[i] *= cw; right[i] *= cw; }
    for (int k = 0; k < 6; ++k) {
        const float a = kTwoPi * k / 6.0f;
        const Vec3 dir = d * std::cos(half) + (u * std::cos(a) + v * std::sin(a)) * std::sin(half);
        getFilter(dir, scratchL_.data(), scratchR_.data());
        for (int i = 0; i < filterLength_; ++i) {
            left[i] += rw * scratchL_[i];
            right[i] += rw * scratchR_[i];
        }
    }
}

Spherical HrtfSet::measurementDirection(int index) const {
    const float* p = easy_->hrtf->SourcePosition.values + static_cast<size_t>(index) * 3;
    // libmysofa converts source positions to cartesian (x forward, y left, z up)
    // when opening with mysofa_open*; convert back to az/el.
    const float x = p[0], y = p[1], z = p[2];
    Spherical s;
    s.azimuthDeg = radToDeg(std::atan2(y, x));
    const float r = std::sqrt(x * x + y * y + z * z);
    s.elevationDeg = r > 0 ? radToDeg(std::asin(clamp(z / r, -1.0f, 1.0f))) : 0.0f;
    return s;
}

const float* HrtfSet::measurementIr(int index, int ear) const {
    const auto* h = easy_->hrtf;
    return h->DataIR.values + (static_cast<size_t>(index) * h->R + ear) * h->N;
}

}  // namespace sp::dsp
