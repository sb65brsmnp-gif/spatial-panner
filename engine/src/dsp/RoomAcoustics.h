// Image-source geometry for box rooms and Eyring reverberation times.
#pragma once

#include <array>
#include <vector>

#include "sp/Math.h"
#include "sp/Scene.h"

namespace sp::dsp {

// One image of a source: the mirrored position and how many times each wall
// was hit on the way.
struct ImageSource {
    Vec3 position;
    std::array<int, kNumWalls> wallHits{};
    int order = 0;
    // Per-band amplitude factor from the wall materials (product of sqrt(1 - alpha)).
    std::array<float, kNumBands> reflectance{1, 1, 1, 1, 1, 1};
    // Position of the listener's mirror image for directivity: the direction
    // the sound leaves the real source is (listenerImage - source).
    std::array<int, 3> index{};  // (nx, ny, nz)
};

// Enumerate images of `source` up to `order` (|nx|+|ny|+|nz| <= order),
// including the direct path (order 0) first. Outdoor rooms give the direct
// path and a single ground reflection.
std::vector<ImageSource> computeImages(const Room& room, const Vec3& source, int order);

// Mirror a point with the same wall sequence as an image (used to get the
// listener's image for directivity).
Vec3 mirrorPoint(const Room& room, const Vec3& p, const std::array<int, 3>& index);

// Mean absorption coefficient per band (area weighted) and Eyring RT60 per band.
struct RoomStats {
    float volume = 0;
    float surface = 0;
    float meanFreePath = 0;  // metres
    std::array<float, kNumBands> meanAbsorption{};
    std::array<float, kNumBands> rt60{};
    std::array<float, kNumBands> roomConstant{};  // R = S * a / (1 - a)
};
RoomStats computeRoomStats(const Room& room, const Environment& env);

// Band index helpers: low = bands 0-1, mid = 2-3, high = 4-5.
inline float bandAverage(const std::array<float, kNumBands>& v, int from, int to) {
    float s = 0;
    for (int i = from; i <= to; ++i) s += v[i];
    return s / (to - from + 1);
}

}  // namespace sp::dsp
