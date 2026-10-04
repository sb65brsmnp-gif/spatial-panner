// Atmospheric absorption after ISO 9613-1.
//
// Air absorption in dB grows roughly with f^2, so the ideal filter has a
// Gaussian magnitude. We approximate it per distance with four cascaded
// one-pole lowpass sections (two pairs with their own cutoffs), fitted at
// initialisation into a table over distance.
#pragma once

#include <vector>

#include "dsp/Filters.h"
#include "sp/Scene.h"

namespace sp::dsp {

// Pure attenuation coefficient in dB/m at frequency f (Hz).
float isoAirAttenuationDbPerMetre(float frequencyHz, float temperatureC, float relativeHumidity,
                                  float pressureKPa);

// The per-path filter state.
struct AirFilter {
    FirstOrder s[4];
    inline float process(float x) { return s[3].process(s[2].process(s[1].process(s[0].process(x)))); }
    void reset() { for (auto& f : s) f.reset(); }
    void setIdentity() { for (auto& f : s) f.setIdentity(); }
};

class AirAbsorptionTable {
public:
    AirAbsorptionTable() = default;
    AirAbsorptionTable(const Environment& env, float sampleRate, float maxDistance, float step = 1.0f);

    struct Coeffs { float fc1, fc2; };
    // Fitted cutoff frequencies for a distance (linear interpolation).
    Coeffs lookup(float distance) const;

    // Configure a filter for `distance`.
    void apply(float distance, AirFilter& filter) const;

    bool enabled() const { return !table_.empty(); }

private:
    std::vector<Coeffs> table_;
    float step_ = 1.0f;
    float sampleRate_ = 48000.0f;
};

}  // namespace sp::dsp
