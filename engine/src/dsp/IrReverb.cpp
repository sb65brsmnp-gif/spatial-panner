#include "dsp/IrReverb.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "dsp/Resampler.h"

namespace sp::dsp {

namespace {

constexpr float kSqrt3 = 1.7320508f;

// World-frame (X, Y, Z) first-order components -> head frame. First-order
// spherical harmonics transform like the direction vector, so this is the
// inverse head rotation expressed in the Ambisonics axes (x forward, y left,
// z up; engine axes: x right, y up, z back).
void rotationMatrix(const Quat& head, float m[9]) {
    // Column k of m is the image of the k-th Ambisonics axis.
    const Vec3 axes[3] = {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}};  // ambi x, y, z in engine coordinates
    for (int k = 0; k < 3; ++k) {
        const Vec3 r = head.inverseRotate(axes[k]);
        m[0 * 3 + k] = -r.z;  // ambi x component of the rotated axis
        m[1 * 3 + k] = -r.x;  // ambi y
        m[2 * 3 + k] = r.y;   // ambi z
    }
}

}  // namespace

void IrReverb::init(const std::vector<std::vector<float>>& irIn, float irRate, int channels, float gainDb, const IrReverbSpec& spec) {
    if (irIn.empty() || irIn[0].empty()) throw std::runtime_error("impulse response is empty");
    const int have = static_cast<int>(irIn.size());
    int use = channels;
    if (use == 0) use = have == 2 ? 2 : have >= 4 ? 4 : 1;
    if (use != 1 && use != 2 && use != 4) throw std::runtime_error("impulse response channels must be 1, 2 or 4");
    if (use > have && !(use == 1)) throw std::runtime_error("impulse response has " + std::to_string(have) + " channels, " + std::to_string(use) + " asked for");
    kind_ = use == 1 ? IrKind::Mono : use == 2 ? IrKind::Stereo : IrKind::AmbiX;

    sub_ = std::max(1, spec.subBlock);
    block_ = std::max(spec.blockSize, sub_);
    block_ = ((block_ + sub_ - 1) / sub_) * sub_;
    order_ = std::min(spec.ambiOrder, kMaxAmbiOrder);
    nCh_ = ambiChannels(order_);

    // Pick / mix the channels, resample to the engine rate.
    std::vector<std::vector<float>> ir(use);
    if (use == 1 && have >= 2 && channels == 1) {
        ir[0].assign(irIn[0].size(), 0.0f);
        for (int c = 0; c < have; ++c)
            for (size_t i = 0; i < ir[0].size(); ++i) ir[0][i] += irIn[c][i] / have;
    } else {
        for (int c = 0; c < use; ++c) ir[c] = irIn[c];
    }
    if (std::fabs(irRate - spec.sampleRate) > 0.5f)
        for (auto& ch : ir) ch = resample(ch, irRate, spec.sampleRate);
    // Trim trailing silence below -100 dB of the peak (keeps the partition count honest).
    float peak = 0;
    for (const auto& ch : ir) for (float v : ch) peak = std::max(peak, std::fabs(v));
    if (!(peak > 0)) throw std::runtime_error("impulse response is silent");
    size_t len = 0;
    for (const auto& ch : ir)
        for (size_t i = ch.size(); i-- > 0;)
            if (std::fabs(ch[i]) > 1e-5f * peak) { len = std::max(len, i + 1); break; }
    len = std::max<size_t>(len, 1);
    for (auto& ch : ir) ch.resize(len, 0.0f);
    seconds_ = static_cast<float>(len) / spec.sampleRate;

    // Unit W energy: mono, the IR itself; stereo, L + R summed in energy
    // (W = L + R); ambiX, the W channel.
    double energy = 0;
    if (kind_ == IrKind::AmbiX) {
        for (float v : ir[0]) energy += static_cast<double>(v) * v;
    } else {
        for (const auto& ch : ir) for (float v : ch) energy += static_cast<double>(v) * v;
    }
    const float norm = static_cast<float>(1.0 / std::sqrt(std::max(energy, 1e-20))) * dbToGain(gainDb);
    for (auto& ch : ir) for (float& v : ch) v *= norm;
    if (kind_ == IrKind::AmbiX)
        for (int c = 1; c < 4; ++c) for (float& v : ir[c]) v *= kSqrt3;  // SN3D -> N3D

    // Convolution engine.
    spec_.blockSize = block_;
    spec_.partitions = static_cast<int>((len + block_ - 1) / block_);
    fft_ = std::make_unique<RealFft>(spec_.fftSize());
    in_.reset(spec_, *fft_);
    filters_.assign(use, PartitionedFilter{});
    for (int c = 0; c < use; ++c) {
        filters_[c].reset(spec_, *fft_);
        filters_[c].set(ir[c].data(), static_cast<int>(len), *fft_);
    }
    acc_.assign(spec_.bins(), cfloat{});
    inBuf_.assign(block_, 0.0f);
    outFrame_.assign(use, std::vector<float>(block_, 0.0f));
    outPtrs_.resize(use);
    for (int c = 0; c < use; ++c) outPtrs_[c] = outFrame_[c].data();

    // Mono: eight decorrelators, 20 ms of velvet noise each (24 taps on a
    // jittered grid, random signs, a gentle decay), each carrying 1/8 of the
    // energy so the W sum stays at unit energy, from eight directions spread
    // over the sphere (a Fibonacci set).
    uint32_t rng = 0x1234567u;
    auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return static_cast<float>(rng >> 8) / 16777216.0f; };
    const int velvetLen = static_cast<int>(0.020f * spec.sampleRate), taps = 24;
    for (int k = 0; k < kDiffuseDirections; ++k) {
        Velvet& v = velvet_[k];
        v.offset.resize(taps);
        v.gain.resize(taps);
        double e = 0;
        for (int j = 0; j < taps; ++j) {
            v.offset[j] = std::min(velvetLen - 1, static_cast<int>((j + rnd()) * velvetLen / taps));
            v.gain[j] = (rnd() < 0.5f ? -1.0f : 1.0f) * std::exp(-1.5f * j / taps);
            e += static_cast<double>(v.gain[j]) * v.gain[j];
        }
        const float g = static_cast<float>(1.0 / std::sqrt(e * kDiffuseDirections));
        for (float& x : v.gain) x *= g;
        const float y = 1.0f - 2.0f * (k + 0.5f) / kDiffuseDirections;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y)), phi = k * 2.3999632f + 0.7f;
        v.sh.fill(0.0f);
        encodeDirection({r * std::cos(phi), y, r * std::sin(phi)}, order_, v.sh.data());
    }
    // W sums the eight copies: where taps from different copies coincide
    // they add coherently, so normalise the summed filter to unit energy.
    {
        std::vector<double> wsum(velvetLen, 0.0);
        for (const auto& v : velvet_)
            for (size_t j = 0; j < v.offset.size(); ++j) wsum[v.offset[j]] += v.gain[j];
        double e = 0;
        for (double x : wsum) e += x * x;
        const float g = static_cast<float>(1.0 / std::sqrt(std::max(e, 1e-12)));
        for (auto& v : velvet_)
            for (float& x : v.gain) x *= g;
    }
    int ringSize = 1;
    while (ringSize < velvetLen + sub_ + 2) ringSize <<= 1;
    ring_.assign(ringSize, 0.0f);
    ringMask_ = ringSize - 1;
    reset();
}

void IrReverb::reset() {
    in_.clear();
    std::fill(inBuf_.begin(), inBuf_.end(), 0.0f);
    for (auto& f : outFrame_) std::fill(f.begin(), f.end(), 0.0f);
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    ringPos_ = 0;
    fill_ = 0;
    outPos_ = 0;
    tailBlocks_ = 0;
    haveOutput_ = false;
    haveRot_ = false;
}

void IrReverb::process(const float* send, const Quat& head, float* bus) {
    if (filters_.empty()) return;
    bool nonZero = false;
    for (int i = 0; i < sub_; ++i) nonZero |= send[i] != 0.0f;
    std::copy(send, send + sub_, inBuf_.begin() + fill_);
    fill_ += sub_;
    if (nonZero) tailBlocks_ = spec_.partitions + 1;
    if (fill_ == block_) {
        fill_ = 0;
        outPos_ = 0;
        if (tailBlocks_ > 0) {
            --tailBlocks_;
            in_.push(inBuf_.data(), *fft_);
            for (size_t c = 0; c < filters_.size(); ++c) {
                std::fill(acc_.begin(), acc_.end(), cfloat{});
                convolveAccumulate(in_, filters_[c], spec_.partitions, acc_.data());
                spectrumToBlock(acc_.data(), *fft_, block_, outFrame_[c].data());
            }
            haveOutput_ = true;
        } else {
            // Nothing left to ring out: skip the FFTs. The input ring must
            // still move on so a later block sees the right history, and the
            // encode keeps running on zeros so the mono decorrelators empty.
            in_.push(inBuf_.data(), *fft_);
            for (auto& f : outFrame_) std::fill(f.begin(), f.end(), 0.0f);
            haveOutput_ = true;
        }
    }
    if (haveOutput_ && outPos_ + sub_ <= block_) encode(outPtrs_.data(), outPos_, head, bus);
    if (outPos_ + sub_ <= block_) outPos_ += sub_;
}

void IrReverb::encode(const float* const* frame, int offset, const Quat& head, float* bus) {
    const int B = sub_;
    auto row = [&](int c) { return bus + static_cast<size_t>(c) * B; };
    switch (kind_) {
        case IrKind::Mono: {
            const float* y = frame[0] + offset;
            for (int i = 0; i < B; ++i) {
                ring_[ringPos_] = y[i];
                for (int k = 0; k < kDiffuseDirections; ++k) {
                    const Velvet& v = velvet_[k];
                    float s = 0;
                    for (size_t j = 0; j < v.offset.size(); ++j) s += v.gain[j] * ring_[(ringPos_ - v.offset[j]) & ringMask_];
                    for (int c = 0; c < nCh_; ++c) row(c)[i] += s * v.sh[c];
                }
                ringPos_ = (ringPos_ + 1) & ringMask_;
            }
            break;
        }
        case IrKind::Stereo: {
            // Plane waves from head-left (ambi y = +1) and head-right, first order only.
            const float* l = frame[0] + offset;
            const float* r = frame[1] + offset;
            float* w = row(0);
            float* yc = row(1);
            for (int i = 0; i < B; ++i) {
                w[i] += l[i] + r[i];
                yc[i] += kSqrt3 * (l[i] - r[i]);
            }
            break;
        }
        case IrKind::AmbiX: {
            rotationMatrix(head, rotCur_);
            if (!haveRot_) { std::copy(rotCur_, rotCur_ + 9, rotPrev_); haveRot_ = true; }
            const float* W = frame[0] + offset;
            const float* Y = frame[1] + offset;
            const float* Z = frame[2] + offset;
            const float* X = frame[3] + offset;
            float* bw = row(0);
            float* by = row(1);
            float* bz = row(2);
            float* bx = row(3);
            const float invB = 1.0f / static_cast<float>(B);
            for (int i = 0; i < B; ++i) {
                const float t = static_cast<float>(i + 1) * invB;
                float m[9];
                for (int k = 0; k < 9; ++k) m[k] = rotPrev_[k] + (rotCur_[k] - rotPrev_[k]) * t;
                const float x = X[i], y = Y[i], z = Z[i];
                bw[i] += W[i];
                bx[i] += m[0] * x + m[1] * y + m[2] * z;
                by[i] += m[3] * x + m[4] * y + m[5] * z;
                bz[i] += m[6] * x + m[7] * y + m[8] * z;
            }
            std::copy(rotCur_, rotCur_ + 9, rotPrev_);
            break;
        }
    }
}

}  // namespace sp::dsp
