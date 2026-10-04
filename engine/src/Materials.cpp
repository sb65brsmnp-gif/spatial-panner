#include <map>

#include "sp/Scene.h"

namespace sp::materials {

namespace {

// Random-incidence absorption coefficients at 125, 250, 500, 1k, 2k, 4k Hz
// (typical published values: Vorländer, "Auralization"; Cox & D'Antonio),
// a scattering coefficient for the ray tracer, and the energy transmitted
// through the surface at low / mid / high frequencies when it occludes.
struct Entry {
    std::array<float, kNumBands> absorption;
    float scattering;
    std::array<float, 3> transmission;
};

// Scattering (0 = mirror-like, 1 = fully diffuse) is the share of reflected
// energy that leaves in random directions. It only matters to the ray
// tracer, where it sets how quickly the reflected field becomes diffuse:
// mirror-like walls in a box keep near-horizontal paths ringing long after
// the statistical (Eyring) decay. The values are for surfaces in a room
// with ordinary contents, not for bare laboratory walls.
const std::map<std::string, Entry>& table() {
    static const std::map<std::string, Entry> t = {
        {"concrete",      {{0.01f, 0.01f, 0.02f, 0.02f, 0.02f, 0.03f}, 0.15f, {0.015f, 0.002f, 0.001f}}},
        {"brick",         {{0.03f, 0.03f, 0.03f, 0.04f, 0.05f, 0.07f}, 0.25f, {0.015f, 0.015f, 0.015f}}},
        {"plaster",       {{0.10f, 0.08f, 0.05f, 0.04f, 0.04f, 0.05f}, 0.15f, {0.10f, 0.02f, 0.01f}}},
        {"glass",         {{0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f}, 0.10f, {0.06f, 0.03f, 0.02f}}},
        {"wood_panel",    {{0.28f, 0.22f, 0.17f, 0.09f, 0.10f, 0.11f}, 0.25f, {0.20f, 0.07f, 0.04f}}},
        {"wood_floor",    {{0.15f, 0.11f, 0.10f, 0.07f, 0.06f, 0.07f}, 0.20f, {0.10f, 0.05f, 0.03f}}},
        {"carpet",        {{0.08f, 0.24f, 0.57f, 0.69f, 0.71f, 0.73f}, 0.40f, {0.20f, 0.10f, 0.05f}}},
        {"curtain",       {{0.07f, 0.31f, 0.49f, 0.75f, 0.70f, 0.60f}, 0.50f, {0.70f, 0.50f, 0.30f}}},
        {"acoustic_tile", {{0.50f, 0.70f, 0.60f, 0.70f, 0.70f, 0.50f}, 0.40f, {0.10f, 0.05f, 0.02f}}},
        {"absorber",      {{0.60f, 0.90f, 0.95f, 0.98f, 0.98f, 0.98f}, 0.50f, {0.30f, 0.10f, 0.05f}}},
        {"grass",         {{0.11f, 0.26f, 0.60f, 0.69f, 0.92f, 0.99f}, 0.60f, {0.0f, 0.0f, 0.0f}}},
        {"gravel",        {{0.25f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f}, 0.60f, {0.0f, 0.0f, 0.0f}}},
        {"asphalt",       {{0.02f, 0.03f, 0.03f, 0.03f, 0.03f, 0.02f}, 0.15f, {0.0f, 0.0f, 0.0f}}},
        {"water",         {{0.01f, 0.01f, 0.01f, 0.01f, 0.02f, 0.02f}, 0.05f, {0.0f, 0.0f, 0.0f}}},
        {"snow",          {{0.45f, 0.75f, 0.90f, 0.95f, 0.95f, 0.95f}, 0.70f, {0.0f, 0.0f, 0.0f}}},
        {"audience",      {{0.52f, 0.68f, 0.85f, 0.97f, 0.93f, 0.85f}, 0.60f, {0.0f, 0.0f, 0.0f}}},
    };
    return t;
}

}  // namespace

Material byName(const std::string& name) {
    Material m;
    const auto& t = table();
    auto it = t.find(name);
    if (it == t.end()) it = t.find("plaster");
    m.name = it->first;
    m.absorption = it->second.absorption;
    m.scattering = it->second.scattering;
    m.transmission = it->second.transmission;
    return m;
}

std::vector<std::string> names() {
    std::vector<std::string> out;
    for (const auto& kv : table()) out.push_back(kv.first);
    return out;
}

}  // namespace sp::materials
