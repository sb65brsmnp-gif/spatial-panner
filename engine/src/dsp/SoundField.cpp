#include "dsp/SoundField.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsp/Ambisonics.h"
#include "saf.h"

namespace sp::dsp {

void SoundFieldTransform::init(int inOrder, int outOrder) {
    inOrder_ = clamp(inOrder, 1, kMaxAmbiOrder);
    outOrder_ = clamp(outOrder, 1, kMaxAmbiOrder);
    nIn_ = ambiChannels(inOrder_);
    nOut_ = ambiChannels(outOrder_);
    // A degree-21 t-design (240 points) integrates products of SH up to
    // order 10 exactly: enough for the order-3 x order-3 outer products of
    // the unwarped field and a fine sampling of the warped one.
    nQ_ = __Tdesign_degree_21_nPoints;
    const float* td = &__Tdesign_degree_21_dirs_deg[0][0];
    dirs_.resize(static_cast<size_t>(nQ_));
    yIn_.assign(static_cast<size_t>(nQ_) * nIn_, 0.0f);
    for (int q = 0; q < nQ_; ++q) {
        dirs_[static_cast<size_t>(q)] = fromSpherical({td[q * 2], td[q * 2 + 1]});
        encodeDirection(dirs_[static_cast<size_t>(q)], inOrder_, yIn_.data() + static_cast<size_t>(q) * nIn_);
    }
}

void SoundFieldTransform::compute(const Vec3& offset, float radius, const Quat& recToHead, float rolloff,
                                  float minDistance, float* matrix) const {
    const float a = std::max(radius, 1e-3f);
    const float dmin = std::max(minDistance, 1e-3f);
    const float w = 1.0f / static_cast<float>(nQ_);
    std::memset(matrix, 0, sizeof(float) * static_cast<size_t>(nOut_) * nIn_);
    float yOut[kMaxAmbiChannels];
    for (int q = 0; q < nQ_; ++q) {
        const Vec3 src = dirs_[static_cast<size_t>(q)] * a - offset;
        const float r = src.length();
        const float g = w * std::pow(a / std::max(r, dmin), rolloff);
        const Vec3 dir = r > 1e-6f ? recToHead.rotate(src / r) : recToHead.rotate(dirs_[static_cast<size_t>(q)]);
        encodeDirection(dir, outOrder_, yOut);
        const float* yi = yIn_.data() + static_cast<size_t>(q) * nIn_;
        for (int k = 0; k < nOut_; ++k) {
            const float s = yOut[k] * g;
            float* row = matrix + static_cast<size_t>(k) * nIn_;
            for (int c = 0; c < nIn_; ++c) row[c] += s * yi[c];
        }
    }
}

}  // namespace sp::dsp
