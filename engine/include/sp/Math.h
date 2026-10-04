// Small vector/quaternion math for the engine.
//
// Conventions (section 10 of the spec): metres, right-handed, Y up, -Z forward.
// Yaw is rotation about +Y (positive = turn left), pitch about +X (positive =
// look up), roll about -Z (positive = right ear down).
#pragma once

#include <algorithm>
#include <cmath>

namespace sp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

inline float degToRad(float d) { return d * (kPi / 180.0f); }
inline float radToDeg(float r) { return r * (180.0f / kPi); }
inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

template <typename T>
inline T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

struct Vec3 {
    float x = 0, y = 0, z = 0;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(dot(*this)); }
    float lengthSquared() const { return dot(*this); }
    Vec3 normalized() const {
        const float l = length();
        return l > 1e-12f ? *this / l : Vec3{0, 0, -1};
    }

    static Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

// Unit quaternion, (x, y, z) imaginary, w real.
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;

    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat identity() { return {}; }

    static Quat fromAxisAngle(const Vec3& axis, float radians) {
        const Vec3 a = axis.normalized();
        const float s = std::sin(radians * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
    }

    // Yaw about +Y, then pitch about the rotated +X, then roll about the
    // rotated -Z (intrinsic Y-X-Z order). Radians.
    static Quat fromYawPitchRoll(float yaw, float pitch, float roll) {
        const Quat qy = fromAxisAngle({0, 1, 0}, yaw);
        const Quat qp = fromAxisAngle({1, 0, 0}, pitch);
        const Quat qr = fromAxisAngle({0, 0, -1}, roll);
        return (qy * qp * qr).normalized();
    }

    // Orientation whose -Z axis points along `forward`, with `up` as the
    // approximate up vector.
    static Quat lookRotation(const Vec3& forward, const Vec3& up = {0, 1, 0}) {
        const Vec3 f = forward.normalized();
        Vec3 r = up.cross(-f);  // right = up x back... compute right = f x up
        r = f.cross(up);
        if (r.lengthSquared() < 1e-10f) r = f.cross({0, 0, -1});
        if (r.lengthSquared() < 1e-10f) r = {1, 0, 0};
        r = r.normalized();
        const Vec3 u = r.cross(f).normalized();
        // Rotation matrix columns: right = r, up = u, back = -f.
        const float m00 = r.x, m01 = u.x, m02 = -f.x;
        const float m10 = r.y, m11 = u.y, m12 = -f.y;
        const float m20 = r.z, m21 = u.z, m22 = -f.z;
        const float tr = m00 + m11 + m22;
        Quat q;
        if (tr > 0) {
            const float s = std::sqrt(tr + 1.0f) * 2;
            q.w = 0.25f * s; q.x = (m21 - m12) / s; q.y = (m02 - m20) / s; q.z = (m10 - m01) / s;
        } else if (m00 > m11 && m00 > m22) {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
            q.w = (m21 - m12) / s; q.x = 0.25f * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s;
        } else if (m11 > m22) {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
            q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25f * s; q.z = (m12 + m21) / s;
        } else {
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
            q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25f * s;
        }
        return q.normalized();
    }

    Quat operator*(const Quat& o) const {
        return {w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w,
                w * o.w - x * o.x - y * o.y - z * o.z};
    }

    Quat conjugate() const { return {-x, -y, -z, w}; }

    Quat normalized() const {
        const float n = std::sqrt(x * x + y * y + z * z + w * w);
        return n > 0 ? Quat{x / n, y / n, z / n, w / n} : Quat{};
    }

    // Rotate a vector from local to world frame.
    Vec3 rotate(const Vec3& v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = u.cross(v) * 2.0f;
        return v + t * w + u.cross(t);
    }
    // Rotate a vector from world to local frame.
    Vec3 inverseRotate(const Vec3& v) const { return conjugate().rotate(v); }

    Vec3 forward() const { return rotate({0, 0, -1}); }
    Vec3 up() const { return rotate({0, 1, 0}); }
    Vec3 right() const { return rotate({1, 0, 0}); }

    static Quat slerp(const Quat& a, Quat b, float t) {
        float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
        if (d > 0.9995f) {
            return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                        a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
        }
        const float th0 = std::acos(d), th = th0 * t;
        const float s0 = std::cos(th) - d * std::sin(th) / std::sin(th0);
        const float s1 = std::sin(th) / std::sin(th0);
        return {a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1, a.w * s0 + b.w * s1};
    }

    // Yaw/pitch/roll in radians, inverse of fromYawPitchRoll (within gimbal limits).
    void toYawPitchRoll(float& yaw, float& pitch, float& roll) const {
        const Vec3 f = forward();
        yaw = std::atan2(-f.x, -f.z);
        pitch = std::asin(clamp(f.y, -1.0f, 1.0f));
        const Vec3 u = up();
        const Quat noRoll = fromYawPitchRoll(yaw, pitch, 0);
        const Vec3 r0 = noRoll.right();
        const Vec3 u0 = noRoll.up();
        roll = std::atan2(u.dot(r0), u.dot(u0));
    }
};

// Direction in the listener's head frame expressed as SOFA/SAF spherical
// coordinates: azimuth in degrees counter-clockwise from the front (positive =
// left), elevation in degrees (positive = up).
struct Spherical {
    float azimuthDeg = 0;
    float elevationDeg = 0;
};

// Convert a direction in head-local engine coordinates (Y up, -Z forward,
// +X right) to spherical azimuth/elevation.
inline Spherical toSpherical(const Vec3& localDir) {
    const Vec3 d = localDir.normalized();
    const float fwd = -d.z, left = -d.x, up = d.y;
    Spherical s;
    s.azimuthDeg = radToDeg(std::atan2(left, fwd));
    s.elevationDeg = radToDeg(std::asin(clamp(up, -1.0f, 1.0f)));
    return s;
}

// Engine head-local direction to SOFA cartesian (x forward, y left, z up).
inline void toSofaCartesian(const Vec3& localDir, float out[3]) {
    out[0] = -localDir.z;
    out[1] = -localDir.x;
    out[2] = localDir.y;
}

inline Vec3 fromSpherical(const Spherical& s) {
    const float az = degToRad(s.azimuthDeg), el = degToRad(s.elevationDeg);
    const float fwd = std::cos(el) * std::cos(az);
    const float left = std::cos(el) * std::sin(az);
    const float up = std::sin(el);
    return {-left, up, -fwd};
}

}  // namespace sp
