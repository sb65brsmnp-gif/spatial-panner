// Uniformly partitioned FFT convolution (overlap-save).
//
// One SpectralInput holds the recent spectra of a signal; any number of
// filters can be applied to it, which is how one source feeds two ears and
// how a 16-channel Ambisonics bus feeds 32 decoder filters.
#pragma once

#include <memory>
#include <vector>

#include "dsp/Fft.h"

namespace sp::dsp {

// Shared sizing for a family of convolutions: block size B, FFT size 2B.
struct ConvolverSpec {
    int blockSize = 32;     // samples per processing block
    int partitions = 8;     // filter length = partitions * blockSize

    int fftSize() const { return 2 * blockSize; }
    int bins() const { return blockSize + 1; }
    int filterLength() const { return partitions * blockSize; }
};

// A filter split into `partitions` spectra.
class PartitionedFilter {
public:
    PartitionedFilter() = default;
    PartitionedFilter(const ConvolverSpec& spec, RealFft& fft) { reset(spec, fft); }
    void reset(const ConvolverSpec& spec, RealFft& fft);

    // `ir` has up to spec.filterLength() samples; shorter is zero padded.
    void set(const float* ir, int length, RealFft& fft);
    void clear();

    const cfloat* partition(int k) const { return spectra_.data() + k * bins_; }
    int bins() const { return bins_; }
    int partitions() const { return partitions_; }
    bool empty() const { return spectra_.empty(); }

private:
    int partitions_ = 0, bins_ = 0, blockSize_ = 0;
    std::vector<cfloat> spectra_;
    std::vector<float> padded_;
};

// Ring of input spectra: spectrum(0) is the newest block.
class SpectralInput {
public:
    SpectralInput() = default;
    SpectralInput(const ConvolverSpec& spec, RealFft& fft) { reset(spec, fft); }
    void reset(const ConvolverSpec& spec, RealFft& fft);

    // Push one block of blockSize samples.
    void push(const float* block, RealFft& fft);
    const cfloat* spectrum(int age) const;  // age 0 = newest
    void clear();

private:
    int partitions_ = 0, bins_ = 0, blockSize_ = 0, head_ = 0;
    std::vector<cfloat> ring_;
    std::vector<float> timeBuf_;  // last 2B samples (overlap-save window)
};

// Multiply-accumulate `input` with `filter` into `acc` (bins() values).
void convolveAccumulate(const SpectralInput& input, const PartitionedFilter& filter, int partitions,
                        cfloat* acc);

// Inverse-transform an accumulated spectrum and keep the valid last B samples.
void spectrumToBlock(const cfloat* acc, RealFft& fft, int blockSize, float* outBlock);

}  // namespace sp::dsp
