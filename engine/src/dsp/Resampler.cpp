#include "dsp/Resampler.h"

#include <algorithm>
#include <cmath>

namespace sp::dsp {

namespace {

double besselI0(double x) {
    double sum = 1, term = 1;
    const double q = x * x / 4;
    for (int k = 1; k < 50; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

}  // namespace

std::vector<float> resample(const std::vector<float>& in, double fromRate, double toRate) {
    if (in.empty() || fromRate <= 0 || toRate <= 0) return {};
    if (std::fabs(fromRate - toRate) < 1e-9) return in;
    const double ratio = fromRate / toRate;             // input samples per output sample
    const double cutoff = 0.92 * std::min(1.0, 1.0 / ratio);  // relative to the input Nyquist
    const int half = 32;
    const double beta = 9.0;
    // Kaiser window tabulated over |x| / half in [0, 1].
    const int tab = 4096;
    std::vector<double> window(tab + 1);
    const double i0b = besselI0(beta);
    for (int i = 0; i <= tab; ++i) {
        const double w = static_cast<double>(i) / tab;
        window[i] = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - w * w))) / i0b;
    }
    const size_t outLen = static_cast<size_t>(std::ceil(in.size() / ratio));
    std::vector<float> out(outLen);
    for (size_t n = 0; n < outLen; ++n) {
        const double t = n * ratio;
        const long i0 = static_cast<long>(std::floor(t));
        double acc = 0;
        for (long k = std::max(0L, i0 - half + 1); k <= std::min(i0 + half, static_cast<long>(in.size()) - 1); ++k) {
            const double x = t - k;  // -half .. half
            const double w = std::fabs(x) / half * tab;
            const int wi = static_cast<int>(w);
            if (wi >= tab) continue;
            const double kaiser = window[wi] + (window[wi + 1] - window[wi]) * (w - wi);
            const double s = std::fabs(x) < 1e-12 ? cutoff : std::sin(M_PI * cutoff * x) / (M_PI * x);
            acc += in[static_cast<size_t>(k)] * s * kaiser;
        }
        out[n] = static_cast<float>(acc);
    }
    return out;
}

}  // namespace sp::dsp
