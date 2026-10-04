// Impulse-response late reverb: the summed reverb send convolved with a
// loaded IR (partitioned FFT convolution in blocks larger than the renderer's
// sub-block, so a seconds-long IR costs a few percent of a core) and placed
// into the Ambisonics bus:
//
//   mono IR    -> a diffuse field: the convolved signal is split into eight
//                 decorrelated copies (short velvet-noise filters) from eight
//                 directions around the listener;
//   stereo IR  -> two broad (first-order) plane waves at head-left and
//                 head-right;
//   ambiX IR   -> first-order B-format (ACN, SN3D -> N3D), world-fixed, so
//                 it is rotated by the inverse head orientation every
//                 sub-block.
//
// The IR is normalised to unit W energy like the FDN, so the renderer's
// Hopkins-Stryker level logic and the per-layer sends apply unchanged; the
// gain is a trim on top. The tail lags the FDN's position by block - subBlock
// samples (224 at the defaults: 4.7 ms at 48 kHz); the renderer reports it as
// Stats::reflectionLatency.
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "dsp/Ambisonics.h"
#include "dsp/Convolver.h"
#include "dsp/Fft.h"
#include "sp/Math.h"

namespace sp::dsp {

struct IrReverbSpec {
    float sampleRate = 48000;
    int subBlock = 32;     // the renderer's sub-block
    int blockSize = 256;   // convolution block; rounded up to a multiple of subBlock
    int ambiOrder = 3;
};

enum class IrKind { Mono, Stereo, AmbiX };

class IrReverb {
public:
    static constexpr int kDiffuseDirections = 8;

    // `ir` is [channel][sample] at `irRate` (resampled here if needed).
    // `channels` picks the interpretation: 0 = from the channel count
    // (1 mono, 2 stereo, 4 or more ambiX, anything else mono from channel 0),
    // 1 / 2 / 4 force it when the file has at least that many channels (a
    // stereo file forced to mono is averaged). Throws std::runtime_error.
    void init(const std::vector<std::vector<float>>& ir, float irRate, int channels, float gainDb, const IrReverbSpec& spec);
    void reset();

    // One sub-block: `send` holds spec.subBlock samples of the reverb send;
    // the tail is added into `bus` (ambiChannels(order) rows of subBlock
    // samples). `head` is the listener's orientation (ambiX IRs are world-fixed).
    void process(const float* send, const Quat& head, float* bus);

    int latency() const { return block_ - sub_; }
    float seconds() const { return seconds_; }
    int channels() const { return static_cast<int>(filters_.size()); }
    IrKind kind() const { return kind_; }
    bool loaded() const { return !filters_.empty(); }

private:
    void encode(const float* const* frame, int offset, const Quat& head, float* bus);

    IrKind kind_ = IrKind::Mono;
    int sub_ = 32, block_ = 256, nCh_ = 16, order_ = 3;
    float seconds_ = 0;
    std::unique_ptr<RealFft> fft_;
    ConvolverSpec spec_;
    SpectralInput in_;
    std::vector<PartitionedFilter> filters_;
    std::vector<cfloat> acc_;
    std::vector<float> inBuf_;                 // block_
    std::vector<std::vector<float>> outFrame_; // [ir channel][block_]
    std::vector<float*> outPtrs_;
    int fill_ = 0, outPos_ = 0;
    int tailBlocks_ = 0;                       // blocks of convolution still owed after the input went silent
    bool haveOutput_ = false;

    // Mono: velvet-noise decorrelators and their directions.
    struct Velvet {
        std::vector<int> offset;
        std::vector<float> gain;   // signed
        std::array<float, kMaxAmbiChannels> sh{};
    };
    std::array<Velvet, kDiffuseDirections> velvet_;
    std::vector<float> ring_;
    int ringMask_ = 0, ringPos_ = 0;

    // AmbiX: rotation of (X, Y, Z) from the world to the head frame, previous
    // and current sub-block, interpolated per sample.
    float rotPrev_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, rotCur_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool haveRot_ = false;
};

}  // namespace sp::dsp
