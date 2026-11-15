#pragma once
// Small vector and quaternion maths for the engine-neutral parts of the core. Right-handed, Y up, -Z forward: OpenXR's
// convention (and Godot's); the Unity plugin converts at its edge.
#include <cmath>

namespace mathx {
struct V3 {
    float x = 0, y = 0, z = 0;
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
    float length() const { return std::sqrt(dot(*this)); }
};

struct Q {
    float x = 0, y = 0, z = 0, w = 1;
    Q operator*(const Q& b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
    }
    V3 operator*(const V3& v) const {
        V3 u{x, y, z};
        V3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
        return v + t * w + V3{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    }
    Q inverse() const { return {-x, -y, -z, w}; }
};

constexpr float kPi = 3.14159265358979f;
inline Q yaw(float radians) { return {0, std::sin(radians / 2), 0, std::cos(radians / 2)}; }
// Heading of a rotation about +Y: 0 looks down -Z, positive turns left (towards -X).
inline float heading(const Q& q) {
    V3 f = q * V3{0, 0, -1};
    return std::atan2(-f.x, -f.z);
}
inline float pitch(const Q& q) {
    V3 f = q * V3{0, 0, -1};
    return std::asin(std::fmax(-1.f, std::fmin(1.f, f.y)));
}
// Angle difference wrapped to (-pi, pi].
inline float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a <= -kPi) a += 2 * kPi;
    return a;
}
}  // namespace mathx
