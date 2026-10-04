#include "dsp/Panning.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "dsp/Ambisonics.h"
#include "dsp/Fft.h"
#include "saf.h"

namespace sp::dsp {

// ----------------------------------------------------------- VbapPanner

void VbapPanner::init(const SpeakerLayout& layout, int resolutionDeg, float spreadDeg) {
    numChannels_ = layout.numChannels();
    res_ = std::max(1, resolutionDeg);
    spatialIndex_.clear();
    std::vector<float> dirs;
    for (int i = 0; i < numChannels_; ++i) {
        if (layout.speakers[i].lfe) continue;
        spatialIndex_.push_back(i);
        dirs.push_back(layout.speakers[i].azimuthDeg);
        dirs.push_back(layout.speakers[i].elevationDeg);
    }
    numSpatial_ = static_cast<int>(spatialIndex_.size());
    planar_ = layout.isPlanar();
    table_.clear();
    stereoAz_.clear();
    if (numSpatial_ == 0) return;
    if (numSpatial_ <= 2) {
        stereoAz_.assign(dirs.begin(), dirs.end());
        return;
    }
    float* gtable = nullptr;
    int nTable = 0, nTri = 0;
    if (planar_) {
        // 2-D layouts (stereo, quad, 5.1, 7.1): pairwise panning on azimuth.
        generateVBAPgainTable2D(dirs.data(), numSpatial_, res_, &gtable, &nTable, &nTri);
        nAzi_ = static_cast<int>(360.0f / res_ + 0.5f) + 1;
        nElev_ = 1;
    } else {
        generateVBAPgainTable3D(dirs.data(), numSpatial_, res_, res_, /*omitLargeTriangles*/ 0,
                                /*enableDummies*/ 1, spreadDeg, &gtable, &nTable, &nTri);
        nAzi_ = static_cast<int>(360.0f / res_ + 0.5f) + 1;
        nElev_ = static_cast<int>(180.0f / res_ + 0.5f) + 1;
    }
    if (!gtable) throw std::runtime_error("VBAP triangulation failed for layout '" + layout.name + "'");
    table_.assign(gtable, gtable + static_cast<size_t>(nTable) * numSpatial_);
    std::free(gtable);
}

void VbapPanner::gains(const Vec3& localDir, float* out) const {
    std::fill(out, out + numChannels_, 0.0f);
    if (numSpatial_ == 0) return;
    const Spherical s = toSpherical(localDir);
    if (numSpatial_ == 1) { out[spatialIndex_[0]] = 1.0f; return; }
    if (numSpatial_ == 2) {
        // Tangent-law stereo pan between the two speakers, clamped to the pair.
        const float a0 = stereoAz_[0], a1 = stereoAz_[2];
        const float lo = std::min(a0, a1), hi = std::max(a0, a1);
        // Fold rear directions to the front.
        float az = s.azimuthDeg;
        if (az > 90) az = 180 - az;
        if (az < -90) az = -180 - az;
        az = clamp(az, lo, hi);
        const float u = (hi - lo) > 1e-3f ? (az - lo) / (hi - lo) : 0.5f;  // 0 at lo, 1 at hi
        const float gHi = std::sin(u * kPi * 0.5f), gLo = std::cos(u * kPi * 0.5f);
        out[spatialIndex_[a0 < a1 ? 0 : 1]] = gLo;
        out[spatialIndex_[a0 < a1 ? 1 : 0]] = gHi;
        return;
    }
    float az = s.azimuthDeg;
    az = std::fmod(az + 180.0f + 720.0f, 360.0f);  // 0..360
    const int aziIndex = std::min(static_cast<int>(az / res_ + 0.5f), nAzi_ - 1);
    int idx = aziIndex;
    if (!planar_) {
        const int elevIndex = clamp(static_cast<int>((s.elevationDeg + 90.0f) / res_ + 0.5f), 0, nElev_ - 1);
        idx = elevIndex * nAzi_ + aziIndex;
    }
    const float* g = table_.data() + static_cast<size_t>(idx) * numSpatial_;
    for (int i = 0; i < numSpatial_; ++i) out[spatialIndex_[i]] = g[i];
}

// --------------------------------------------------- AmbiSpeakerDecoder

void AmbiSpeakerDecoder::init(const SpeakerLayout& layout, int order) {
    numChannels_ = layout.numChannels();
    nSh_ = ambiChannels(order);
    matrix_.assign(static_cast<size_t>(numChannels_) * nSh_, 0.0f);
    std::vector<int> spatial;
    std::vector<float> dirs;
    for (int i = 0; i < numChannels_; ++i) {
        if (layout.speakers[i].lfe) continue;
        spatial.push_back(i);
        dirs.push_back(layout.speakers[i].azimuthDeg);
        dirs.push_back(layout.speakers[i].elevationDeg);
    }
    const int nLs = static_cast<int>(spatial.size());
    if (nLs == 0) return;
    std::vector<float> dec(static_cast<size_t>(nLs) * nSh_, 0.0f);
    if (nLs >= 4 && !layout.isPlanar()) {
        getLoudspeakerDecoderMtx(dirs.data(), nLs, LOUDSPEAKER_DECODER_ALLRAD, order, /*maxrE*/ 1, dec.data());
    } else {
        // Planar or very small layouts: AllRAD needs a 3-D hull, so fall back
        // to energy-preserving decoding (EPAD), which handles 2-D rings; for
        // one or two speakers, use the sampling decoder.
        const auto method = nLs >= 3 ? LOUDSPEAKER_DECODER_EPAD : LOUDSPEAKER_DECODER_SAD;
        getLoudspeakerDecoderMtx(dirs.data(), nLs, method, order, /*maxrE*/ 1, dec.data());
    }
    for (int i = 0; i < nLs; ++i)
        for (int c = 0; c < nSh_; ++c) matrix_[static_cast<size_t>(spatial[i]) * nSh_ + c] = dec[static_cast<size_t>(i) * nSh_ + c];
}

// ------------------------------------------------- designAmbiBinauralFilters

AmbiBinauralFilters designAmbiBinauralFilters(const HrtfSet& hrtf, int order, int filterLength) {
    AmbiBinauralFilters out;
    out.order = order;
    out.nSh = ambiChannels(order);
    out.length = filterLength;

    // Pick the measured directions nearest to a uniform t-design so the
    // least-squares fit is not biased by the SOFA grid's density.
    const int nTd = __Tdesign_degree_21_nPoints;
    const float* td = &__Tdesign_degree_21_dirs_deg[0][0];
    std::vector<int> chosen;
    std::vector<float> dirsDeg;
    const int M = hrtf.numMeasurements();
    std::vector<Vec3> measDirs(M);
    for (int m = 0; m < M; ++m) measDirs[m] = fromSpherical(hrtf.measurementDirection(m));
    for (int t = 0; t < nTd; ++t) {
        const Vec3 want = fromSpherical({td[t * 2], td[t * 2 + 1]});
        int best = 0;
        float bestDot = -2;
        for (int m = 0; m < M; ++m) {
            const float d = want.dot(measDirs[m]);
            if (d > bestDot) { bestDot = d; best = m; }
        }
        if (std::find(chosen.begin(), chosen.end(), best) != chosen.end()) continue;
        chosen.push_back(best);
        const Spherical s = hrtf.measurementDirection(best);
        dirsDeg.push_back(s.azimuthDeg);
        dirsDeg.push_back(s.elevationDeg);
    }
    const int nDirs = static_cast<int>(chosen.size());

    // HRTF spectra, FLAT: nBins x 2 x nDirs.
    const int fftSize = filterLength;
    const int nBins = fftSize / 2 + 1;
    RealFft fft(fftSize);
    std::vector<float> padded(fftSize);
    std::vector<cfloat> spec(nBins);
    std::vector<cfloat> hrtfs(static_cast<size_t>(nBins) * 2 * nDirs);
    const int irLen = std::min(hrtf.filterLength(), fftSize);
    for (int d = 0; d < nDirs; ++d) {
        for (int ear = 0; ear < 2; ++ear) {
            std::fill(padded.begin(), padded.end(), 0.0f);
            const float* ir = hrtf.measurementIr(chosen[d], ear);
            std::copy(ir, ir + irLen, padded.begin());
            fft.forward(padded.data(), spec.data());
            for (int b = 0; b < nBins; ++b) hrtfs[(static_cast<size_t>(b) * 2 + ear) * nDirs + d] = spec[b];
        }
    }

    std::vector<float> dec(static_cast<size_t>(2) * out.nSh * fftSize, 0.0f);
    getBinauralAmbiDecoderFilters(reinterpret_cast<float_complex*>(hrtfs.data()), dirsDeg.data(), nDirs, fftSize,
                                  hrtf.sampleRate(), BINAURAL_DECODER_MAGLS, order, /*itd*/ nullptr,
                                  /*weights*/ nullptr, /*diffCM*/ 1, /*maxrE*/ 1, dec.data());
    out.filters = std::move(dec);  // SAF layout: NUM_EARS x nSH x fftSize
    return out;
}

}  // namespace sp::dsp
