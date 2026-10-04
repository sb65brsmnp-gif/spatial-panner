#include "dsp/Convolver.h"

#include <algorithm>
#include <cstring>

namespace sp::dsp {

// ----------------------------------------------------- PartitionedFilter

void PartitionedFilter::reset(const ConvolverSpec& spec, RealFft& /*fft*/) {
    partitions_ = spec.partitions;
    bins_ = spec.bins();
    blockSize_ = spec.blockSize;
    spectra_.assign(static_cast<size_t>(partitions_) * bins_, cfloat{0, 0});
    padded_.assign(spec.fftSize(), 0.0f);
}

void PartitionedFilter::clear() { std::fill(spectra_.begin(), spectra_.end(), cfloat{0, 0}); }

void PartitionedFilter::set(const float* ir, int length, RealFft& fft) {
    for (int k = 0; k < partitions_; ++k) {
        std::fill(padded_.begin(), padded_.end(), 0.0f);
        const int start = k * blockSize_;
        const int n = std::max(0, std::min(blockSize_, length - start));
        if (n > 0) std::memcpy(padded_.data(), ir + start, sizeof(float) * n);
        fft.forward(padded_.data(), spectra_.data() + static_cast<size_t>(k) * bins_);
    }
}

// --------------------------------------------------------- SpectralInput

void SpectralInput::reset(const ConvolverSpec& spec, RealFft& /*fft*/) {
    partitions_ = spec.partitions;
    bins_ = spec.bins();
    blockSize_ = spec.blockSize;
    head_ = 0;
    ring_.assign(static_cast<size_t>(partitions_) * bins_, cfloat{0, 0});
    timeBuf_.assign(spec.fftSize(), 0.0f);
}

void SpectralInput::clear() {
    std::fill(ring_.begin(), ring_.end(), cfloat{0, 0});
    std::fill(timeBuf_.begin(), timeBuf_.end(), 0.0f);
    head_ = 0;
}

void SpectralInput::push(const float* block, RealFft& fft) {
    // Slide: [old B | new B]
    std::memmove(timeBuf_.data(), timeBuf_.data() + blockSize_, sizeof(float) * blockSize_);
    std::memcpy(timeBuf_.data() + blockSize_, block, sizeof(float) * blockSize_);
    head_ = (head_ + partitions_ - 1) % partitions_;
    fft.forward(timeBuf_.data(), ring_.data() + static_cast<size_t>(head_) * bins_);
}

const cfloat* SpectralInput::spectrum(int age) const {
    return ring_.data() + static_cast<size_t>((head_ + age) % partitions_) * bins_;
}

// ------------------------------------------------------------- helpers

void convolveAccumulate(const SpectralInput& input, const PartitionedFilter& filter, int partitions,
                        cfloat* acc) {
    // Hand-written complex MAC: std::complex's operator* goes through
    // __mulsc3 (NaN/Inf fix-ups) without -ffast-math, which is several times
    // slower and blocks vectorisation.
    const int n = filter.bins();
    float* a = reinterpret_cast<float*>(acc);
    for (int k = 0; k < partitions; ++k) {
        const float* x = reinterpret_cast<const float*>(input.spectrum(k));
        const float* h = reinterpret_cast<const float*>(filter.partition(k));
        for (int i = 0; i < n; ++i) {
            const float xr = x[2 * i], xi = x[2 * i + 1], hr = h[2 * i], hi = h[2 * i + 1];
            a[2 * i] += xr * hr - xi * hi;
            a[2 * i + 1] += xr * hi + xi * hr;
        }
    }
}

void spectrumToBlock(const cfloat* acc, RealFft& fft, int blockSize, float* outBlock) {
    thread_local std::vector<float> time;
    if (static_cast<int>(time.size()) < 2 * blockSize) time.resize(2 * blockSize);
    fft.inverse(acc, time.data());
    std::memcpy(outBlock, time.data() + blockSize, sizeof(float) * blockSize);
}

}  // namespace sp::dsp
