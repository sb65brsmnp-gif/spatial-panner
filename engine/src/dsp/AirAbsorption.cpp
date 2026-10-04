#include "dsp/AirAbsorption.h"

#include <algorithm>
#include <cmath>

namespace sp::dsp {

float isoAirAttenuationDbPerMetre(float f, float temperatureC, float relativeHumidity, float pressureKPa) {
    // ISO 9613-1:1993, equations (3) to (5).
    const double T = 273.15 + temperatureC;
    const double T0 = 293.15;
    const double T01 = 273.16;
    const double pr = 101.325;
    const double pa = pressureKPa;
    const double psatRatio = std::pow(10.0, -6.8346 * std::pow(T01 / T, 1.261) + 4.6151);
    const double h = relativeHumidity * psatRatio / (pa / pr);  // molar concentration of water vapour, %
    const double frO = (pa / pr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    const double frN = (pa / pr) * std::pow(T / T0, -0.5) *
                       (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(T / T0, -1.0 / 3.0) - 1.0)));
    const double f2 = static_cast<double>(f) * f;
    const double alpha =
        8.686 * f2 *
        (1.84e-11 * (pr / pa) * std::sqrt(T / T0) +
         std::pow(T / T0, -2.5) * (0.01275 * std::exp(-2239.1 / T) / (frO + f2 / frO) +
                                   0.1068 * std::exp(-3352.0 / T) / (frN + f2 / frN)));
    return static_cast<float>(alpha);
}

namespace {

// Attenuation in dB of two pairs of cascaded one-pole lowpass sections. A
// cutoff at or above `fOff` means "pair disabled" (no attenuation at all),
// which is also how AirAbsorptionTable::apply() treats it.
inline double pairDb(double f, double fc, double fOff) {
    if (fc >= fOff) return 0.0;
    const double a = f / fc;
    return 20.0 * std::log10(1.0 + a * a);
}
inline double cascadeDb(double f, double fc1, double fc2, double fOff) {
    return pairDb(f, fc1, fOff) + pairDb(f, fc2, fOff);
}

}  // namespace

AirAbsorptionTable::AirAbsorptionTable(const Environment& env, float sampleRate, float maxDistance, float step)
    : step_(step), sampleRate_(sampleRate) {
    if (!env.airAbsorption) return;
    const int n = static_cast<int>(std::ceil(std::max(maxDistance, step) / step)) + 2;
    table_.resize(n);

    // Fit frequencies (log spaced, 500 Hz to 16 kHz). Errors are weighted
    // towards moderate attenuations: once a band is 30 dB down the exact
    // amount no longer matters, so both target and fit are capped there.
    const int nf = 16;
    double freqs[nf], alpha[nf];
    for (int i = 0; i < nf; ++i) {
        freqs[i] = 500.0 * std::pow(32.0, static_cast<double>(i) / (nf - 1));
        alpha[i] = isoAirAttenuationDbPerMetre(static_cast<float>(freqs[i]), env.temperatureC,
                                               env.relativeHumidity, env.pressureKPa);
    }
    const double capDb = 24.0;
    const double fmax = 0.45 * sampleRate;   // highest usable cutoff; at/above = pair off
    const double fcap = fmax * 1.1;          // search just past fmax so "off" is reachable

    for (int i = 0; i < n; ++i) {
        const double d = i * step;
        if (d < 1e-3) { table_[i] = {static_cast<float>(fcap), static_cast<float>(fcap)}; continue; }
        // Coarse-to-fine grid search over (fc1, fc2) in log space.
        double best1 = fcap, best2 = fcap, bestErr = 1e30;
        double lo = std::log(200.0), hi = std::log(fcap);
        double c1 = 0.5 * (lo + hi), c2 = c1, span = 0.5 * (hi - lo);
        for (int pass = 0; pass < 5; ++pass) {
            const int steps = pass == 0 ? 24 : 8;
            double nb1 = best1, nb2 = best2;
            for (int a = 0; a <= steps; ++a) {
                const double l1 = c1 - span + 2.0 * span * a / steps;
                for (int b = a; b <= steps; ++b) {  // fc1 <= fc2 (symmetric)
                    const double l2 = c2 - span + 2.0 * span * b / steps;
                    const double f1 = std::exp(l1), f2 = std::exp(l2);
                    double err = 0;
                    for (int k = 0; k < nf; ++k) {
                        const double target = std::min(alpha[k] * d, capDb);
                        const double got = std::min(cascadeDb(freqs[k], f1, f2, fmax), capDb);
                        const double e = got - target;
                        const double w = 1.0 / (1.0 + (target / 8.0) * (target / 8.0));
                        err += w * e * e;
                    }
                    if (err < bestErr) { bestErr = err; nb1 = f1; nb2 = f2; }
                }
            }
            best1 = nb1; best2 = nb2;
            c1 = std::log(best1); c2 = std::log(best2);
            span *= 0.25;
        }
        table_[i] = {static_cast<float>(std::min(best1, fcap)), static_cast<float>(std::min(best2, fcap))};
    }
}

AirAbsorptionTable::Coeffs AirAbsorptionTable::lookup(float distance) const {
    if (table_.empty()) return {1e9f, 1e9f};
    const float f = std::max(distance, 0.0f) / step_;
    const size_t i = std::min(static_cast<size_t>(f), table_.size() - 2);
    const float u = std::min(f - static_cast<float>(i), 1.0f);
    return {lerp(table_[i].fc1, table_[i + 1].fc1, u), lerp(table_[i].fc2, table_[i + 1].fc2, u)};
}

void AirAbsorptionTable::apply(float distance, AirFilter& filter) const {
    if (table_.empty()) { filter.setIdentity(); return; }
    const Coeffs c = lookup(distance);
    const float nyq = 0.45f * sampleRate_;
    for (int k = 0; k < 2; ++k) {
        const float fc = k == 0 ? c.fc1 : c.fc2;
        if (fc >= nyq) { filter.s[2 * k].setIdentity(); filter.s[2 * k + 1].setIdentity(); }
        else { filter.s[2 * k].setLowpass(fc, sampleRate_); filter.s[2 * k + 1].copyCoefficients(filter.s[2 * k]); }
    }
}

}  // namespace sp::dsp
