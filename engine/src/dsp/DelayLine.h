// Fractional delay line with 4th-order Lagrange interpolation (5 taps).
// One write head, any number of read taps (direct path + image sources).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace sp::dsp {

class DelayLine {
public:
    void init(int maxDelaySamples) {
        size_ = 1;
        while (size_ < maxDelaySamples + 8) size_ <<= 1;
        mask_ = size_ - 1;
        buf_.assign(static_cast<size_t>(size_), 0.0f);
        write_ = 0;
    }

    int maxDelay() const { return size_ - 8; }

    inline void write(float x) {
        buf_[write_] = x;
        write_ = (write_ + 1) & mask_;
    }

    // Read `delay` samples behind the most recent write (delay >= 2).
    inline float read(float delay) const {
        if (delay < 2.0f) delay = 2.0f;
        const float maxD = static_cast<float>(size_ - 6);
        if (delay > maxD) delay = maxD;
        // Centre the 5-point stencil on the integer part.
        const int di = static_cast<int>(delay);
        const float f = delay - static_cast<float>(di);  // 0..1
        // Samples at offsets di-2 .. di+2 relative to the read point; the
        // interpolation variable x = 2 + f over nodes 0..4 (sample at node 2
        // is `di` samples back, node 3 is di+1 samples back).
        const int base = (write_ - 1 - di) & mask_;  // sample `di` back
        const float y0 = buf_[(base + 2) & mask_];   // di-2 back
        const float y1 = buf_[(base + 1) & mask_];   // di-1
        const float y2 = buf_[base];                 // di
        const float y3 = buf_[(base - 1) & mask_];   // di+1
        const float y4 = buf_[(base - 2) & mask_];   // di+2
        // Lagrange weights for x = 2 + f on nodes 0,1,2,3,4.
        const float x = 2.0f + f;
        const float x0 = x, x1 = x - 1, x2 = x - 2, x3 = x - 3, x4 = x - 4;
        const float w0 = (x1 * x2 * x3 * x4) / 24.0f;
        const float w1 = -(x0 * x2 * x3 * x4) / 6.0f;
        const float w2 = (x0 * x1 * x3 * x4) / 4.0f;
        const float w3 = -(x0 * x1 * x2 * x4) / 6.0f;
        const float w4 = (x0 * x1 * x2 * x3) / 24.0f;
        return w0 * y0 + w1 * y1 + w2 * y2 + w3 * y3 + w4 * y4;
    }

    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); }

private:
    std::vector<float> buf_;
    int size_ = 0, mask_ = 0, write_ = 0;
};

// Integer delay with linear interpolation, for speaker distance alignment.
class SimpleDelay {
public:
    void init(int maxDelaySamples) {
        size_ = 1;
        while (size_ < maxDelaySamples + 4) size_ <<= 1;
        mask_ = size_ - 1;
        buf_.assign(static_cast<size_t>(size_), 0.0f);
        write_ = 0;
    }
    inline float process(float x, float delay) {
        buf_[write_] = x;
        const int di = static_cast<int>(delay);
        const float f = delay - di;
        const float a = buf_[(write_ - di) & mask_];
        const float b = buf_[(write_ - di - 1) & mask_];
        write_ = (write_ + 1) & mask_;
        return a + (b - a) * f;
    }

private:
    std::vector<float> buf_;
    int size_ = 0, mask_ = 0, write_ = 0;
};

}  // namespace sp::dsp
