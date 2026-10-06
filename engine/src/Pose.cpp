#include "sp/Pose.h"

#include <algorithm>
#include <cmath>

namespace sp {

float applyEasing(Easing e, float u) {
    u = clamp(u, 0.0f, 1.0f);
    switch (e) {
        case Easing::Linear: return u;
        case Easing::SmoothStep: return u * u * (3.0f - 2.0f * u);
        case Easing::EaseIn: return u * u;
        case Easing::EaseOut: return 1.0f - (1.0f - u) * (1.0f - u);
        case Easing::Hold: return 0.0f;
    }
    return u;
}

// ------------------------------------------------------------ SampledPath

namespace {

Vec3 catmullRom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float u) {
    // Centripetal Catmull-Rom (alpha = 0.5), which avoids cusps and
    // self-intersections on unevenly spaced points.
    auto knot = [](const Vec3& a, const Vec3& b) { return std::pow(std::max((b - a).length(), 1e-6f), 0.5f); };
    const float t0 = 0, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
    const float t = t1 + (t2 - t1) * u;
    auto l = [](const Vec3& a, const Vec3& b, float ta, float tb, float t) {
        return a * ((tb - t) / (tb - ta)) + b * ((t - ta) / (tb - ta));
    };
    const Vec3 a1 = l(p0, p1, t0, t1, t), a2 = l(p1, p2, t1, t2, t), a3 = l(p2, p3, t2, t3, t);
    const Vec3 b1 = l(a1, a2, t0, t2, t), b2 = l(a2, a3, t1, t3, t);
    return l(b1, b2, t1, t2, t);
}

}  // namespace

Vec3 SampledPath::evaluateSegment(const PathSegment& seg, float u) {
    u = clamp(u, 0.0f, 1.0f);
    const auto& p = seg.points;
    switch (seg.type) {
        case SegmentType::Line: {
            if (p.size() < 2) return p.empty() ? Vec3{} : p[0];
            return Vec3::lerp(p[0], p[1], u);
        }
        case SegmentType::CubicBezier: {
            if (p.size() < 4) return p.empty() ? Vec3{} : p[0];
            const float v = 1 - u;
            return p[0] * (v * v * v) + p[1] * (3 * v * v * u) + p[2] * (3 * v * u * u) + p[3] * (u * u * u);
        }
        case SegmentType::CatmullRom: {
            const int n = static_cast<int>(p.size());
            if (n == 0) return {};
            if (n == 1) return p[0];
            if (n == 2) return Vec3::lerp(p[0], p[1], u);
            const float f = u * (n - 1);
            int i = std::min(static_cast<int>(f), n - 2);
            const float lu = f - i;
            const Vec3& p1 = p[i];
            const Vec3& p2 = p[i + 1];
            const Vec3 p0 = i > 0 ? p[i - 1] : p1 + (p1 - p2);
            const Vec3 p3 = i + 2 < n ? p[i + 2] : p2 + (p2 - p1);
            return catmullRom(p0, p1, p2, p3, lu);
        }
        case SegmentType::Arc: {
            if (p.size() < 3) return p.empty() ? Vec3{} : p[0];
            const Vec3 c = p[0];
            const Vec3 a = p[1] - c, b = p[2] - c;
            const float ra = a.length(), rb = b.length();
            if (ra < 1e-6f) return c;
            Vec3 n = a.cross(b);
            if (n.lengthSquared() < 1e-10f) n = {0, 1, 0};  // start and end collinear with centre
            n = n.normalized();
            if (seg.arcClockwise) n = -n;
            // Angle from a to b about n, in [0, 2pi).
            const Vec3 ax = a.normalized();
            const Vec3 ay = n.cross(ax);
            const Vec3 bn = b.normalized();
            float ang = std::atan2(bn.dot(ay), bn.dot(ax));
            if (ang < 0) ang += kTwoPi;
            if (ang < 1e-5f && seg.arcTurns == 0 && (p[2] - p[1]).lengthSquared() < 1e-10f) ang = kTwoPi;  // full circle
            ang += kTwoPi * static_cast<float>(std::max(0, seg.arcTurns));
            const float th = ang * u;
            const float r = lerp(ra, rb, u);
            return c + (ax * std::cos(th) + ay * std::sin(th)) * r;
        }
    }
    return {};
}

SampledPath::SampledPath(const Path& path, float tolerance) {
    std::vector<PathSegment> segs = path.segments;
    if (path.closed && !segs.empty()) {
        const Vec3 first = evaluateSegment(segs.front(), 0);
        const Vec3 last = evaluateSegment(segs.back(), 1);
        if ((first - last).lengthSquared() > 1e-8f) {
            PathSegment join;
            join.type = SegmentType::Line;
            join.points = {last, first};
            segs.push_back(join);
        }
    }
    float s = 0;
    for (const auto& seg : segs) {
        if (seg.points.empty()) continue;
        // Adaptive subdivision: start coarse, split while the chord deviates
        // from the curve midpoint by more than `tolerance`.
        struct Span { float u0, u1; Vec3 p0, p1; int depth; };
        std::vector<Span> stack;
        const int coarse = seg.type == SegmentType::Line ? 1 : 16;
        for (int i = coarse - 1; i >= 0; --i) {
            const float u0 = static_cast<float>(i) / coarse, u1 = static_cast<float>(i + 1) / coarse;
            stack.push_back({u0, u1, evaluateSegment(seg, u0), evaluateSegment(seg, u1), 0});
        }
        if (samples_.empty()) samples_.push_back({0, stack.back().p0});
        while (!stack.empty()) {
            Span sp = stack.back();
            stack.pop_back();
            const float um = 0.5f * (sp.u0 + sp.u1);
            const Vec3 pm = evaluateSegment(seg, um);
            const Vec3 chordMid = (sp.p0 + sp.p1) * 0.5f;
            if ((pm - chordMid).length() > tolerance && sp.depth < 12) {
                stack.push_back({um, sp.u1, pm, sp.p1, sp.depth + 1});
                stack.push_back({sp.u0, um, sp.p0, pm, sp.depth + 1});
            } else {
                s += (sp.p1 - samples_.back().p).length();
                samples_.push_back({s, sp.p1});
            }
        }
    }
    length_ = s;
}

size_t SampledPath::findIndex(float s) const {
    // Largest i with samples_[i].s <= s, in [0, size-2].
    auto it = std::upper_bound(samples_.begin(), samples_.end(), s,
                               [](float v, const Sample& smp) { return v < smp.s; });
    size_t i = it == samples_.begin() ? 0 : static_cast<size_t>(it - samples_.begin()) - 1;
    return std::min(i, samples_.size() - 2);
}

Vec3 SampledPath::positionAt(float s) const {
    if (samples_.empty()) return {};
    if (samples_.size() == 1) return samples_[0].p;
    s = clamp(s, 0.0f, length_);
    const size_t i = findIndex(s);
    const float ds = samples_[i + 1].s - samples_[i].s;
    const float u = ds > 1e-9f ? (s - samples_[i].s) / ds : 0;
    return Vec3::lerp(samples_[i].p, samples_[i + 1].p, u);
}

Vec3 SampledPath::tangentAt(float s) const {
    if (samples_.size() < 2) return {0, 0, -1};
    s = clamp(s, 0.0f, length_);
    // Central difference over a short window gives a smooth tangent across
    // sample joints.
    const float h = std::max(0.02f, length_ * 1e-4f);
    const Vec3 a = positionAt(std::max(0.0f, s - h));
    const Vec3 b = positionAt(std::min(length_, s + h));
    const Vec3 d = b - a;
    if (d.lengthSquared() < 1e-12f) {
        const size_t i = findIndex(s);
        return (samples_[i + 1].p - samples_[i].p).normalized();
    }
    return d.normalized();
}

// ----------------------------------------------------------- SampledSpeed

float SampledSpeed::evalSpeed(const SpeedCurve& c, double t) {
    const auto& k = c.keys;
    if (k.empty()) return 1.4f;
    if (t <= k.front().time) return std::max(0.0f, k.front().speed);
    if (t >= k.back().time) return std::max(0.0f, k.back().speed);
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].time <= t) ++i;
    const auto& a = k[i];
    const auto& b = k[i + 1];
    const double span = b.time - a.time;
    const float u = span > 0 ? static_cast<float>((t - a.time) / span) : 1.0f;
    return std::max(0.0f, lerp(a.speed, b.speed, applyEasing(a.easing, u)));
}

SampledSpeed::SampledSpeed(const SpeedCurve& curve, double duration, double dt) : curve_(curve), dt_(dt) {
    const size_t n = static_cast<size_t>(std::ceil(std::max(duration, dt) / dt)) + 2;
    distance_.resize(n);
    distance_[0] = 0;
    // Trapezoidal integration of v(t).
    double prev = evalSpeed(curve_, 0);
    for (size_t i = 1; i < n; ++i) {
        const double v = evalSpeed(curve_, i * dt_);
        distance_[i] = distance_[i - 1] + 0.5 * (prev + v) * dt_;
        prev = v;
    }
}

float SampledSpeed::speedAt(double t) const { return evalSpeed(curve_, t); }

double SampledSpeed::distanceAt(double t) const {
    if (t <= 0 || distance_.size() < 2) return 0;
    const double f = t / dt_;
    const size_t i = static_cast<size_t>(f);
    if (i + 1 >= distance_.size()) {
        // Past the table: extrapolate with the final speed.
        const double tEnd = (distance_.size() - 1) * dt_;
        return distance_.back() + evalSpeed(curve_, tEnd) * (t - tEnd);
    }
    const double u = f - i;
    return distance_[i] + (distance_[i + 1] - distance_[i]) * u;
}

// --------------------------------------------------------- Layer motion

namespace {

// Heading of a direction about +Y, degrees: 0 = towards -Z, positive turns
// left (towards -X), the head's yaw convention.
float headingOf(const Vec3& d) { return radToDeg(std::atan2(-d.x, -d.z)); }

float wrap180(float a) {
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0) a += 360.0f;
    return a - 180.0f;
}

// 0 at x <= 0, 1 at x >= 1, smooth between.
float smoothUnit(float x) {
    x = clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Jumping back to the start of an open loop would click: the layer fades
// out over its last few millimetres (5 ms of travel) and in again after.
constexpr double kLoopFadeSeconds = 0.005;

}  // namespace

LayerMotionEvaluator::LayerMotionEvaluator(const LayerMotion& motion) : m_(motion) {
    if (!m_.hasPath()) return;
    path_ = SampledPath(m_.path);
    if (path_.empty() || path_.length() < 1e-4f) return;
    active_ = true;
    closed_ = m_.path.closed || (path_.positionAt(0) - path_.positionAt(path_.length())).length() < 0.01f;
    // After the last key the speed stays at the last key's, which distanceAt
    // extrapolates exactly, so the table only has to reach the last key.
    const double lastKey = m_.speed.keys.empty() ? 0.0 : m_.speed.keys.back().time;
    speed_ = SampledSpeed(m_.speed, std::max(lastKey, 0.0) + 1.0, 0.005);
    std::sort(m_.keys.begin(), m_.keys.end(), [](const PathKey& a, const PathKey& b) { return a.time < b.time; });
    startHeading_ = headingAt(0, true);
}

double LayerMotionEvaluator::travel(double t) const {
    const double rel = t - m_.startTime;
    return rel <= 0 ? 0.0 : speed_.distanceAt(rel);
}

float LayerMotionEvaluator::speedAt(double t) const {
    const double rel = t - m_.startTime;
    return rel < 0 ? 0.0f : speed_.speedAt(rel);
}

float LayerMotionEvaluator::headingAt(float s, bool forward) const {
    Vec3 d = path_.tangentAt(s);
    if (!forward) d = -d;
    d.y = 0;
    if (d.lengthSquared() < 1e-8f) return startHeading_;  // straight up or down: keep the start heading
    return headingOf(d);
}

float LayerMotionEvaluator::keyFraction(double t) const {
    const auto& k = m_.keys;
    if (k.empty()) return 0;
    if (t <= k.front().time) return clamp(k.front().fraction, 0.0f, 1.0f);
    if (t >= k.back().time) return clamp(k.back().fraction, 0.0f, 1.0f);
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].time <= t) ++i;
    const auto& a = k[i];
    const auto& b = k[i + 1];
    const double span = b.time - a.time;
    const float u = applyEasing(a.easing, span > 0 ? static_cast<float>((t - a.time) / span) : 1.0f);
    return clamp(lerp(a.fraction, b.fraction, u), 0.0f, 1.0f);
}

// The direction of the key move under way at `t`, or of the last one before
// it while the layer waits between moves (forward when it never moved).
bool LayerMotionEvaluator::keyDirection(double t) const {
    const auto& k = m_.keys;
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].time <= t) ++i;
    for (size_t j = std::min(i + 1, k.size() - 1); j >= 1; --j) {
        const float d = k[j].fraction - k[j - 1].fraction;
        if (std::fabs(d) > 1e-6f) return d > 0;
    }
    return true;
}

MotionState LayerMotionEvaluator::evaluate(double t, const MotionOverride& o) const {
    MotionState st;
    if (!active_) return st;
    const float L = path_.length();
    float s = 0;
    bool forward = true;
    switch (m_.timing) {
        case LayerTiming::Position:
            s = clamp(o.fraction.value_or(m_.fraction), 0.0f, 1.0f) * L;
            break;
        case LayerTiming::Speed: {
            const double u = std::max(0.0, o.distance.value_or(travel(t)));
            const float v = o.distance ? o.speed : speedAt(t);
            switch (m_.end) {
                case PathEnd::Stop:
                    s = static_cast<float>(std::min(u, static_cast<double>(L)));
                    break;
                case PathEnd::Loop: {
                    s = static_cast<float>(std::fmod(u, static_cast<double>(L)));
                    if (!closed_) {
                        const float wd = std::max(v * static_cast<float>(kLoopFadeSeconds), 0.002f);
                        if (u >= L) st.gain = std::min(st.gain, smoothUnit(s / wd));
                        st.gain = std::min(st.gain, smoothUnit((L - s) / wd));
                    }
                    break;
                }
                case PathEnd::PingPong: {
                    const double m = std::fmod(u, 2.0 * L);
                    forward = m <= L;
                    s = static_cast<float>(forward ? m : 2.0 * L - m);
                    break;
                }
            }
            break;
        }
        case LayerTiming::Keys: {
            const auto& k = m_.keys;
            if (k.empty()) break;
            const double t0 = k.front().time, tN = k.back().time, P = tN - t0;
            double tt = t;
            bool reversed = false;
            if (t > tN && P > 0) {
                if (m_.end == PathEnd::Loop) {
                    tt = t0 + std::fmod(t - t0, P);
                } else if (m_.end == PathEnd::PingPong) {
                    const double q = std::fmod(t - t0, 2.0 * P);
                    reversed = q > P;
                    tt = reversed ? tN - (q - P) : t0 + q;
                }
            }
            s = keyFraction(tt) * L;
            forward = keyDirection(tt) != reversed;
            if (m_.end == PathEnd::Loop && P > 0 && !closed_ && t >= t0 &&
                std::fabs(k.front().fraction - k.back().fraction) * L > 0.01f) {
                if (t > tN) st.gain = std::min(st.gain, smoothUnit(static_cast<float>((tt - t0) / kLoopFadeSeconds)));
                st.gain = std::min(st.gain, smoothUnit(static_cast<float>((tN - tt) / kLoopFadeSeconds)));
            }
            break;
        }
    }
    s = clamp(s, 0.0f, L);
    st.distance = s;
    st.forward = forward;
    st.offset = path_.positionAt(s) - path_.positionAt(0);
    if (m_.turnAlongPath) st.yawDeg = wrap180(headingAt(s, forward) - startHeading_);
    return st;
}

float levelKeysDb(const std::vector<LevelKey>& k, double t) {
    if (k.empty()) return 0;
    if (t <= k.front().time) return k.front().levelDb;
    if (t >= k.back().time) return k.back().levelDb;
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].time <= t) ++i;
    const auto& a = k[i];
    const auto& b = k[i + 1];
    const double span = b.time - a.time;
    const float u = applyEasing(a.easing, span > 0 ? static_cast<float>((t - a.time) / span) : 1.0f);
    // Fades towards silence run in gain, not dB, so they reach zero smoothly.
    if (a.levelDb <= kSilentDb || b.levelDb <= kSilentDb) {
        const float ga = a.levelDb <= kSilentDb ? 0.0f : dbToGain(a.levelDb);
        const float gb = b.levelDb <= kSilentDb ? 0.0f : dbToGain(b.levelDb);
        const float g = lerp(ga, gb, u);
        return g <= dbToGain(kSilentDb) ? kSilentDb : gainToDb(g);
    }
    return lerp(a.levelDb, b.levelDb, u);
}

float levelKeysGain(const std::vector<LevelKey>& k, double t) {
    const float db = levelKeysDb(k, t);
    return db <= kSilentDb ? 0.0f : dbToGain(db);
}

// ---------------------------------------------------------- PoseEvaluator

PoseEvaluator::PoseEvaluator(const Scene& scene, double duration) : scene_(scene), duration_(duration) {
    for (const auto& p : scene.listener.paths) paths_.emplace_back(p);
    speed_ = SampledSpeed(scene.listener.speed, std::max(duration, 1.0));
    layers_.reserve(scene.layers.size());
    for (const auto& l : scene.layers) layers_.emplace_back(l.motion);
}

const LayerMotionEvaluator& PoseEvaluator::layerMotion(int i) const {
    static const LayerMotionEvaluator none;
    return i >= 0 && i < static_cast<int>(layers_.size()) ? layers_[static_cast<size_t>(i)] : none;
}

Vec3 PoseEvaluator::layerPosition(int i, double time) const {
    if (i < 0 || i >= static_cast<int>(scene_.layers.size())) return {};
    return scene_.layers[static_cast<size_t>(i)].position + layerMotion(i).evaluate(time).offset;
}

const SampledPath* PoseEvaluator::activePath(const ListenerControls& controls) const {
    const int idx = controls.activePath.value_or(scene_.listener.activePath);
    if (idx < 0 || idx >= static_cast<int>(paths_.size())) return nullptr;
    if (paths_[idx].empty()) return nullptr;
    return &paths_[idx];
}

float PoseEvaluator::distanceAlongPath(double time, const ListenerControls& controls) const {
    const SampledPath* path = activePath(controls);
    if (!path) return 0;
    const auto& L = scene_.listener;
    if (L.positionMode == PositionMode::AlongPath) {
        const float frac = controls.pathFraction.value_or(L.pathFraction);
        return clamp(frac, 0.0f, 1.0f) * path->length();
    }
    const double t = time - L.pathStartTime;
    float s = static_cast<float>(speed_.distanceAt(t) * controls.speedMultiplier);
    if (L.loopPath && path->length() > 0) {
        s = std::fmod(s, path->length());
        if (s < 0) s += path->length();
    }
    return clamp(s, 0.0f, path->length());
}

namespace {

void keyedHeadAngles(const std::vector<HeadKey>& k, double time, float& yaw, float& pitch, float& roll) {
    yaw = pitch = roll = 0;
    if (k.empty()) return;
    if (time <= k.front().time) {
        yaw = k.front().yawDeg; pitch = k.front().pitchDeg; roll = k.front().rollDeg;
    } else if (time >= k.back().time) {
        yaw = k.back().yawDeg; pitch = k.back().pitchDeg; roll = k.back().rollDeg;
    } else {
        size_t i = 0;
        while (i + 1 < k.size() && k[i + 1].time <= time) ++i;
        const auto& a = k[i];
        const auto& b = k[i + 1];
        const double span = b.time - a.time;
        const float u = applyEasing(a.easing, span > 0 ? static_cast<float>((time - a.time) / span) : 1.0f);
        yaw = lerp(a.yawDeg, b.yawDeg, u);
        pitch = lerp(a.pitchDeg, b.pitchDeg, u);
        roll = lerp(a.rollDeg, b.rollDeg, u);
    }
}

}  // namespace

Quat PoseEvaluator::orientationAt(double time, const Vec3& position, const Vec3& tangent,
                                  const ListenerControls& controls) const {
    const auto& head = scene_.listener.head;
    Quat base;
    switch (head.mode) {
        case HeadMode::AlongPath: {
            Vec3 fwd = tangent;
            if (fwd.lengthSquared() < 1e-10f) fwd = {0, 0, -1};
            base = Quat::lookRotation(fwd);
            break;
        }
        case HeadMode::LookAt: {
            Vec3 target = head.lookAtPoint;
            if (head.lookAtLayer >= 0 && head.lookAtLayer < static_cast<int>(scene_.layers.size()))
                target = layerPosition(head.lookAtLayer, time);
            Vec3 fwd = target - position;
            if (fwd.lengthSquared() < 1e-8f) fwd = tangent.lengthSquared() > 1e-10f ? tangent : Vec3{0, 0, -1};
            base = Quat::lookRotation(fwd);
            break;
        }
        case HeadMode::Keyframed:
            break;
    }
    // Keyframes are the absolute head direction in Keyframed mode, and a
    // time-varying turn of the head on top of the path direction or look-at
    // target in the other modes ("look 60 degrees left while walking").
    if (head.mode == HeadMode::Keyframed || !head.keys.empty()) {
        float yaw = 0, pitch = 0, roll = 0;
        keyedHeadAngles(head.keys, time, yaw, pitch, roll);
        const Quat keyed = Quat::fromYawPitchRoll(degToRad(yaw), degToRad(pitch), degToRad(roll));
        base = head.mode == HeadMode::Keyframed ? keyed : base * keyed;
    }
    const Quat offset = Quat::fromYawPitchRoll(
        degToRad(head.yawOffsetDeg + controls.yawOffsetDeg),
        degToRad(head.pitchOffsetDeg + controls.pitchOffsetDeg),
        degToRad(head.rollOffsetDeg + controls.rollOffsetDeg));
    return (base * offset).normalized();
}

Pose PoseEvaluator::evaluate(double time, const ListenerControls& controls) const {
    Pose pose;
    const SampledPath* path = activePath(controls);
    if (!path) {
        pose.position = scene_.listener.staticPosition;
        pose.orientation = orientationAt(time, pose.position, {0, 0, -1}, controls);
        pose.velocity = {};
        return pose;
    }
    const float s = distanceAlongPath(time, controls);
    pose.position = path->positionAt(s);
    Vec3 tangent = path->tangentAt(s);
    const float v = scene_.listener.positionMode == PositionMode::Speed
                        ? speed_.speedAt(time - scene_.listener.pathStartTime) * controls.speedMultiplier
                        : 0.0f;
    pose.velocity = tangent * v;
    pose.orientation = orientationAt(time, pose.position, tangent, controls);
    return pose;
}

}  // namespace sp
