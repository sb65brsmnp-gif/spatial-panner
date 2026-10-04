#include <map>

#include "sp/Scene.h"

namespace sp::materials {

namespace {

// Random-incidence absorption coefficients at 125, 250, 500, 1k, 2k, 4k Hz.
// Typical published values (Vorländer, "Auralization"; Cox & D'Antonio).
const std::map<std::string, std::array<float, kNumBands>>& table() {
    static const std::map<std::string, std::array<float, kNumBands>> t = {
        {"concrete",      {0.01f, 0.01f, 0.02f, 0.02f, 0.02f, 0.03f}},
        {"brick",         {0.03f, 0.03f, 0.03f, 0.04f, 0.05f, 0.07f}},
        {"plaster",       {0.10f, 0.08f, 0.05f, 0.04f, 0.04f, 0.05f}},
        {"glass",         {0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f}},
        {"wood_panel",    {0.28f, 0.22f, 0.17f, 0.09f, 0.10f, 0.11f}},
        {"wood_floor",    {0.15f, 0.11f, 0.10f, 0.07f, 0.06f, 0.07f}},
        {"carpet",        {0.08f, 0.24f, 0.57f, 0.69f, 0.71f, 0.73f}},
        {"curtain",       {0.07f, 0.31f, 0.49f, 0.75f, 0.70f, 0.60f}},
        {"acoustic_tile", {0.50f, 0.70f, 0.60f, 0.70f, 0.70f, 0.50f}},
        {"absorber",      {0.60f, 0.90f, 0.95f, 0.98f, 0.98f, 0.98f}},
        {"grass",         {0.11f, 0.26f, 0.60f, 0.69f, 0.92f, 0.99f}},
        {"gravel",        {0.25f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f}},
        {"asphalt",       {0.02f, 0.03f, 0.03f, 0.03f, 0.03f, 0.02f}},
        {"water",         {0.01f, 0.01f, 0.01f, 0.01f, 0.02f, 0.02f}},
        {"snow",          {0.45f, 0.75f, 0.90f, 0.95f, 0.95f, 0.95f}},
        {"audience",      {0.52f, 0.68f, 0.85f, 0.97f, 0.93f, 0.85f}},
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
    m.absorption = it->second;
    return m;
}

std::vector<std::string> names() {
    std::vector<std::string> out;
    for (const auto& kv : table()) out.push_back(kv.first);
    return out;
}

}  // namespace sp::materials
