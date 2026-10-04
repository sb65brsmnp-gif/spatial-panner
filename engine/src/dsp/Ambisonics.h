// Real spherical harmonics (ACN ordering, N3D normalisation) up to 3rd order,
// closed form, for per-sub-block encoding of reflections and reverb.
#pragma once

#include <cmath>

#include "sp/Math.h"

namespace sp::dsp {

constexpr int kMaxAmbiOrder = 3;
constexpr int kMaxAmbiChannels = (kMaxAmbiOrder + 1) * (kMaxAmbiOrder + 1);

inline int ambiChannels(int order) { return (order + 1) * (order + 1); }

// SH weights for a direction given in head-local engine coordinates
// (Y up, -Z forward, +X right). `out` gets ambiChannels(order) values.
inline void encodeDirection(const Vec3& localDir, int order, float* out) {
    const Vec3 d = localDir.normalized();
    // Ambisonics frame: x forward, y left, z up.
    const float x = -d.z, y = -d.x, z = d.y;
    out[0] = 1.0f;
    if (order < 1) return;
    const float s3 = 1.7320508f;
    out[1] = s3 * y;
    out[2] = s3 * z;
    out[3] = s3 * x;
    if (order < 2) return;
    const float s15 = 3.8729833f, s5 = 2.2360680f;
    out[4] = s15 * x * y;
    out[5] = s15 * y * z;
    out[6] = 0.5f * s5 * (3 * z * z - 1);
    out[7] = s15 * x * z;
    out[8] = 0.5f * s15 * (x * x - y * y);
    if (order < 3) return;
    const float s35_8 = 2.0916500f, s105 = 10.2469508f, s21_8 = 1.6201852f, s7 = 2.6457513f;
    out[9] = s35_8 * y * (3 * x * x - y * y);
    out[10] = s105 * x * y * z;
    out[11] = s21_8 * y * (5 * z * z - 1);
    out[12] = 0.5f * s7 * z * (5 * z * z - 3);
    out[13] = s21_8 * x * (5 * z * z - 1);
    out[14] = 0.5f * s105 * z * (x * x - y * y);
    out[15] = s35_8 * x * (x * x - 3 * y * y);
}

// N3D -> SN3D scale per channel (ambiX uses SN3D).
inline float n3dToSn3d(int channel) {
    const int n = static_cast<int>(std::sqrt(static_cast<float>(channel)));
    return 1.0f / std::sqrt(2.0f * n + 1.0f);
}

}  // namespace sp::dsp
