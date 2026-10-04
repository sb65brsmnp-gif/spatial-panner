// An Ambisonic recording as a sphere the listener can move through.
//
// The recording holds the field at one point. The sphere of radius a around
// the layer's position stands for where the recorded sounds were: every
// direction d of the recording is a source at a * d on the surface. For a
// listener offset o from the centre, that source is at distance r = |a d - o|
// in direction (a d - o) / r, and its amplitude relative to the centre is
// (a / r) ^ rolloff (clamped inside minDistance). The matrix below turns the
// recording's spherical-harmonic coefficients into the coefficients of that
// warped field, in the head frame:
//
//   T = (1 / Q) sum_q  y_out(R d'_q) g_q y_in(d_q)^T
//
// over a spherical quadrature (t-design) of Q directions d_q, where d'_q and
// g_q are the direction and gain the surface point at d_q has from o, and R
// rotates the recording's frame into the head's. This is the sources-on-a-
// sphere model done in the Ambisonics domain (Kronlachner and Zotter's
// directional warping and gain, with the sphere's geometry giving both). At
// the centre T is the plain rotation of the field; far outside, the whole
// field narrows into the direction of the centre and falls off as a / |o|.
//
// Output order may exceed the input's: the warped field is sharper than a
// first-order recording and the extra orders carry that.
#pragma once

#include <vector>

#include "sp/Math.h"

namespace sp::dsp {

class SoundFieldTransform {
public:
    void init(int inOrder, int outOrder);
    int numIn() const { return nIn_; }
    int numOut() const { return nOut_; }

    // `offset`: listener relative to the sphere's centre, in the recording's
    // frame (engine axes). `recToHead` rotates recording-frame directions into
    // the head frame. `matrix` gets numOut() rows of numIn() values (ACN/N3D
    // in, ACN/N3D out).
    void compute(const Vec3& offset, float radius, const Quat& recToHead, float rolloff, float minDistance,
                 float* matrix) const;

private:
    int inOrder_ = 1, outOrder_ = 3, nIn_ = 4, nOut_ = 16, nQ_ = 0;
    std::vector<Vec3> dirs_;      // quadrature directions, engine axes
    std::vector<float> yIn_;      // nQ x nIn: input SH at each direction
};

}  // namespace sp::dsp
