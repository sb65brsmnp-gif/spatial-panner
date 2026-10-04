#include "dsp/Fft.h"

#include <algorithm>

#include "saf.h"

namespace sp::dsp {

RealFft::RealFft(int size) : size_(size), scratch_(size), cscratch_(size / 2 + 1) {
    saf_rfft_create(&handle_, size);
}

RealFft::~RealFft() {
    if (handle_) saf_rfft_destroy(&handle_);
}

void RealFft::forward(const float* in, cfloat* out) {
    std::copy(in, in + size_, scratch_.begin());
    saf_rfft_forward(handle_, scratch_.data(), reinterpret_cast<float_complex*>(out));
}

void RealFft::inverse(const cfloat* in, float* out) {
    std::copy(in, in + bins(), cscratch_.begin());
    saf_rfft_backward(handle_, reinterpret_cast<float_complex*>(cscratch_.data()), out);
}

}  // namespace sp::dsp
