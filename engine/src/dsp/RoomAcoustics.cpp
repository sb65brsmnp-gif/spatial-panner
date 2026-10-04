#include "dsp/RoomAcoustics.h"

#include <algorithm>
#include <cmath>

#include "dsp/AirAbsorption.h"

namespace sp::dsp {

namespace {

// Mirror coordinate `v` (relative to the room's min corner) across walls of
// extent L, n times. Returns the image coordinate and the wall hit counts.
inline float mirrorAxis(float v, float L, int n, int& negHits, int& posHits) {
    const int a = std::abs(n);
    const bool even = (n % 2 == 0);
    const float img = n * L + (even ? v : L - v);
    if (n > 0) { posHits = (a + 1) / 2; negHits = a / 2; }
    else if (n < 0) { negHits = (a + 1) / 2; posHits = a / 2; }
    else { negHits = posHits = 0; }
    return img;
}

}  // namespace

Vec3 mirrorPoint(const Room& room, const Vec3& p, const std::array<int, 3>& idx) {
    const Vec3 mn = room.minCorner();
    const Vec3 rel = p - mn;
    int a, b;
    Vec3 out;
    out.x = mn.x + mirrorAxis(rel.x, room.size.x, idx[0], a, b);
    out.y = mn.y + mirrorAxis(rel.y, room.size.y, idx[1], a, b);
    out.z = mn.z + mirrorAxis(rel.z, room.size.z, idx[2], a, b);
    return out;
}

std::vector<ImageSource> computeImages(const Room& room, const Vec3& source, int order) {
    std::vector<ImageSource> images;
    ImageSource direct;
    direct.position = source;
    images.push_back(direct);
    if (room.type == RoomType::None || order <= 0) return images;

    if (room.type == RoomType::Outdoor) {
        // Ground plane at y = origin.y, material = floor.
        ImageSource g;
        g.index = {0, -1, 0};
        g.position = source;
        g.position.y = 2.0f * room.origin.y - source.y;
        g.wallHits[WallNegY] = 1;
        g.order = 1;
        for (int b = 0; b < kNumBands; ++b)
            g.reflectance[b] = std::sqrt(std::max(0.0f, 1.0f - room.materials[WallNegY].absorption[b]));
        g.specular = std::sqrt(clamp(1.0f - room.materials[WallNegY].scattering, 0.0f, 1.0f));
        images.push_back(g);
        return images;
    }

    // Box room. Clamp the source just inside the walls so a layer placed on
    // or outside a wall still gets a sane image set.
    const Vec3 mn = room.minCorner(), mx = room.maxCorner();
    const Vec3 s{clamp(source.x, mn.x + 0.01f, mx.x - 0.01f), clamp(source.y, mn.y + 0.01f, mx.y - 0.01f),
                 clamp(source.z, mn.z + 0.01f, mx.z - 0.01f)};
    const Vec3 rel = s - mn;
    for (int nx = -order; nx <= order; ++nx) {
        for (int ny = -order; ny <= order; ++ny) {
            for (int nz = -order; nz <= order; ++nz) {
                const int o = std::abs(nx) + std::abs(ny) + std::abs(nz);
                if (o == 0 || o > order) continue;
                ImageSource img;
                img.index = {nx, ny, nz};
                img.order = o;
                int negX, posX, negY, posY, negZ, posZ;
                img.position.x = mn.x + mirrorAxis(rel.x, room.size.x, nx, negX, posX);
                img.position.y = mn.y + mirrorAxis(rel.y, room.size.y, ny, negY, posY);
                img.position.z = mn.z + mirrorAxis(rel.z, room.size.z, nz, negZ, posZ);
                img.wallHits = {negX, posX, negY, posY, negZ, posZ};
                for (int b = 0; b < kNumBands; ++b) {
                    float r = 1.0f;
                    for (int w = 0; w < kNumWalls; ++w) {
                        const float refl = std::sqrt(std::max(0.0f, 1.0f - room.materials[w].absorption[b]));
                        for (int h = 0; h < img.wallHits[w]; ++h) r *= refl;
                    }
                    img.reflectance[b] = r;
                }
                img.specular = 1.0f;
                for (int w = 0; w < kNumWalls; ++w)
                    for (int h = 0; h < img.wallHits[w]; ++h)
                        img.specular *= std::sqrt(clamp(1.0f - room.materials[w].scattering, 0.0f, 1.0f));
                images.push_back(img);
            }
        }
    }
    // Direct first, then by increasing order (perceptually: cheap to truncate).
    std::stable_sort(images.begin(), images.end(),
                     [](const ImageSource& a, const ImageSource& b) { return a.order < b.order; });
    return images;
}

RoomStats computeRoomStats(const Room& room, const Environment& env) {
    RoomStats st;
    if (room.type != RoomType::Box) return st;
    const float W = room.size.x, H = room.size.y, D = room.size.z;
    st.volume = W * H * D;
    const std::array<float, kNumWalls> area{H * D, H * D, W * D, W * D, W * H, W * H};
    st.surface = 0;
    for (float a : area) st.surface += a;
    st.meanFreePath = 4.0f * st.volume / st.surface;
    for (int b = 0; b < kNumBands; ++b) {
        float sa = 0;
        for (int w = 0; w < kNumWalls; ++w) sa += area[w] * clamp(room.materials[w].absorption[b], 0.0f, 0.99f);
        const float a = sa / st.surface;
        st.meanAbsorption[b] = a;
        // Eyring with air absorption: T = 0.161 V / (-S ln(1-a) + 4 m V),
        // m = alpha_dB/m / 4.343 (power attenuation per metre).
        float m = 0;
        if (env.airAbsorption)
            m = isoAirAttenuationDbPerMetre(kBandCentresHz[b], env.temperatureC, env.relativeHumidity, env.pressureKPa) /
                4.343f;
        const float denom = -st.surface * std::log(1.0f - a) + 4.0f * m * st.volume;
        st.rt60[b] = denom > 1e-6f ? 0.161f * st.volume / denom : 10.0f;
        st.rt60[b] *= room.reverbTimeScale;
        st.roomConstant[b] = st.surface * a / std::max(1.0f - a, 0.01f);
    }
    return st;
}

}  // namespace sp::dsp
