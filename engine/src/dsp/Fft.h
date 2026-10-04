// Real FFT wrapper (SAF's saf_rfft, KISS FFT underneath unless a platform
// FFT is configured).
#pragma once

#include <complex>
#include <vector>

namespace sp::dsp {

using cfloat = std::complex<float>;

class RealFft {
public:
    explicit RealFft(int size);
    ~RealFft();
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    int size() const { return size_; }
    int bins() const { return size_ / 2 + 1; }

    // out: bins() complex values. Unnormalised.
    void forward(const float* in, cfloat* out);
    // in: bins() complex values; out: size() real values, scaled by 1/size.
    void inverse(const cfloat* in, float* out);

private:
    int size_;
    void* handle_ = nullptr;
    std::vector<float> scratch_;
    std::vector<cfloat> cscratch_;
};

}  // namespace sp::dsp
