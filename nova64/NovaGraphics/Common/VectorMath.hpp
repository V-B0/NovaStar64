/**
 * @file VectorMath.hpp
 * @brief Vector, matrix and geometry helpers for N64 display list processing, built on SIMDMath.
 */
// SPDX-License-Identifier: MIT

#pragma once

#include "SIMDMath.hpp"
#include <cstdint>
#include <cmath>

namespace Nova64 {
namespace Video {
namespace Math {

using SIMD::Mat4x4;
using SIMD::Vec4f;
using SIMD::Vec4i;

// --- Constants ---
static constexpr float kPi = 3.14159265358979323846f;
static constexpr float kTwoPi = 6.28318530717958647692f;
static constexpr float kHalfPi = 1.57079632679489661923f;
static constexpr float kDegToRad = 0.01745329251994329577f;
static constexpr float kRadToDeg = 57.2957795130823208768f;
static constexpr float kEpsilon = 1e-6f;
static constexpr float kEpsilonSq = 1e-12f;
static constexpr float kSqrt2 = 1.41421356237309504880f;
static constexpr float kInvSqrt2 = 0.70710678118654752440f;

// --- 2D Vector ---
struct Vec2f {
    float x, y;

    Vec2f() : x(0), y(0) {}
    Vec2f(float _x, float _y) : x(_x), y(_y) {}
    explicit Vec2f(float s) : x(s), y(s) {}

    Vec2f operator+(const Vec2f& o) const { return {x + o.x, y + o.y}; }
    Vec2f operator-(const Vec2f& o) const { return {x - o.x, y - o.y}; }
    Vec2f operator*(float s) const { return {x * s, y * s}; }
    Vec2f operator/(float s) const {
        float inv = 1.0f / s;
        return {x * inv, y * inv};
    }
    Vec2f operator*(const Vec2f& o) const { return {x * o.x, y * o.y}; }
    Vec2f& operator+=(const Vec2f& o) {
        x += o.x;
        y += o.y;
        return *this;
    }
    Vec2f& operator-=(const Vec2f& o) {
        x -= o.x;
        y -= o.y;
        return *this;
    }
    Vec2f& operator*=(float s) {
        x *= s;
        y *= s;
        return *this;
    }
    bool operator==(const Vec2f& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2f& o) const { return !(*this == o); }

    float Dot(const Vec2f& o) const { return x * o.x + y * o.y; }
    float Cross(const Vec2f& o) const { return x * o.y - y * o.x; }
    float Length() const { return std::sqrt(x * x + y * y); }
    float LengthSq() const { return x * x + y * y; }
    Vec2f Normalized() const {
        float len = Length();
        return len > kEpsilon ? Vec2f(x / len, y / len) : Vec2f(0);
    }
    Vec2f Perp() const { return {-y, x}; }
    Vec2f Abs() const { return {std::fabs(x), std::fabs(y)}; }

    static Vec2f Lerp(const Vec2f& a, const Vec2f& b, float t) { return a + (b - a) * t; }
    static Vec2f Min(const Vec2f& a, const Vec2f& b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
    static Vec2f Max(const Vec2f& a, const Vec2f& b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
};

// --- 3D Vector ---
struct Vec3f {
    float x, y, z;

    Vec3f() : x(0), y(0), z(0) {}
    Vec3f(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    explicit Vec3f(float s) : x(s), y(s), z(s) {}
    explicit Vec3f(const Vec4f& v) : x(v.f[0]), y(v.f[1]), z(v.f[2]) {}

    Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3f operator/(float s) const {
        float inv = 1.0f / s;
        return {x * inv, y * inv, z * inv};
    }
    Vec3f operator*(const Vec3f& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3f operator-() const { return {-x, -y, -z}; }
    Vec3f& operator+=(const Vec3f& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    Vec3f& operator-=(const Vec3f& o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    Vec3f& operator*=(float s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    bool operator==(const Vec3f& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3f& o) const { return !(*this == o); }

    float Dot(const Vec3f& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3f Cross(const Vec3f& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float LengthSq() const { return x * x + y * y + z * z; }
    Vec3f Normalized() const {
        float len = Length();
        return len > kEpsilon ? Vec3f(x / len, y / len, z / len) : Vec3f(0);
    }
    Vec3f Abs() const { return {std::fabs(x), std::fabs(y), std::fabs(z)}; }
    Vec4f ToVec4f(float w = 0.0f) const { return Vec4f(x, y, z, w); }

    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }

    static Vec3f Lerp(const Vec3f& a, const Vec3f& b, float t) { return a + (b - a) * t; }
    static Vec3f Min(const Vec3f& a, const Vec3f& b) {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    }
    static Vec3f Max(const Vec3f& a, const Vec3f& b) {
        return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    }
    static Vec3f Reflect(const Vec3f& dir, const Vec3f& normal) { return dir - normal * (2.0f * dir.Dot(normal)); }
};

// --- Color (RGBA float) ---
struct Color4f {
    float r, g, b, a;

    Color4f() : r(0), g(0), b(0), a(1) {}
    Color4f(float _r, float _g, float _b, float _a = 1.0f) : r(_r), g(_g), b(_b), a(_a) {}
    explicit Color4f(uint32_t rgba32) {
        r = ((rgba32 >> 24) & 0xFF) / 255.0f;
        g = ((rgba32 >> 16) & 0xFF) / 255.0f;
        b = ((rgba32 >> 8) & 0xFF) / 255.0f;
        a = (rgba32 & 0xFF) / 255.0f;
    }

    Color4f operator+(const Color4f& o) const { return {r + o.r, g + o.g, b + o.b, a + o.a}; }
    Color4f operator*(float s) const { return {r * s, g * s, b * s, a * s}; }
    Color4f operator*(const Color4f& o) const { return {r * o.r, g * o.g, b * o.b, a * o.a}; }
    Color4f& operator+=(const Color4f& o) {
        r += o.r;
        g += o.g;
        b += o.b;
        a += o.a;
        return *this;
    }

    uint32_t ToRGBA32() const {
        auto c = [](float f) -> uint8_t {
            return static_cast<uint8_t>(std::min(std::max(f, 0.0f), 1.0f) * 255.0f + 0.5f);
        };
        return (c(r) << 24) | (c(g) << 16) | (c(b) << 8) | c(a);
    }

    uint16_t ToRGBA5551() const {
        auto c5 = [](float f) -> uint16_t {
            return static_cast<uint16_t>(std::min(std::max(f, 0.0f), 1.0f) * 31.0f + 0.5f);
        };
        uint16_t ra = (a >= 0.5f) ? 1 : 0;
        return (c5(r) << 11) | (c5(g) << 6) | (c5(b) << 1) | ra;
    }

    Color4f Clamped() const {
        return {std::min(std::max(r, 0.0f), 1.0f), std::min(std::max(g, 0.0f), 1.0f), std::min(std::max(b, 0.0f), 1.0f),
                std::min(std::max(a, 0.0f), 1.0f)};
    }

    Vec4f ToVec4f() const { return Vec4f(r, g, b, a); }

    static Color4f FromRGBA5551(uint16_t px) {
        return {((px >> 11) & 0x1F) / 31.0f, ((px >> 6) & 0x1F) / 31.0f, ((px >> 1) & 0x1F) / 31.0f,
                static_cast<float>(px & 1)};
    }

    static Color4f FromIA8(uint8_t px) {
        float i = (px >> 4) / 15.0f;
        float a = (px & 0xF) / 15.0f;
        return {i, i, i, a};
    }

    static Color4f Lerp(const Color4f& a, const Color4f& b, float t) {
        float ot = 1.0f - t;
        return {a.r * ot + b.r * t, a.g * ot + b.g * t, a.b * ot + b.b * t, a.a * ot + b.a * t};
    }

    static Color4f White() { return {1, 1, 1, 1}; }
    static Color4f Black() { return {0, 0, 0, 1}; }
    static Color4f Transparent() { return {0, 0, 0, 0}; }
    static Color4f Red() { return {1, 0, 0, 1}; }
    static Color4f Green() { return {0, 1, 0, 1}; }
    static Color4f Blue() { return {0, 0, 1, 1}; }
};

// --- MATRIX CONSTRUCTION UTILITIES ---

// --- N64-SPECIFIC MATRIX UTILITIES ---

// Convert N64 viewport parameters to OpenGL viewport + depth range
struct N64Viewport {
    float x, y;          // center
    float width, height; // half-dimensions
    float nearZ, farZ;   // depth range
};

// --- GEOMETRY UTILITIES ---

// --- Bounding Box ---
struct AABB {
    Vec3f min, max;

    AABB() : min(1e30f), max(-1e30f) {}
    AABB(const Vec3f& _min, const Vec3f& _max) : min(_min), max(_max) {}

    void Expand(const Vec3f& point) {
        min = Vec3f::Min(min, point);
        max = Vec3f::Max(max, point);
    }

    void Expand(const AABB& other) {
        min = Vec3f::Min(min, other.min);
        max = Vec3f::Max(max, other.max);
    }

    Vec3f Center() const { return (min + max) * 0.5f; }
    Vec3f Size() const { return max - min; }
    float Volume() const {
        Vec3f s = Size();
        return s.x * s.y * s.z;
    }

    bool Contains(const Vec3f& point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y && point.z >= min.z &&
               point.z <= max.z;
    }

    bool Intersects(const AABB& other) const {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }

    bool IsValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
};

// --- Frustum (6 planes) ---
struct VMPlane {
    Vec3f normal;
    float distance;

    VMPlane() : distance(0) {}
    VMPlane(const Vec3f& n, float d) : normal(n), distance(d) {}
    VMPlane(float a, float b, float c, float d) : normal(a, b, c), distance(d) {
        float len = normal.Length();
        if (len > kEpsilon) {
            normal = normal / len;
            distance /= len;
        }
    }

    float DistanceToPoint(const Vec3f& p) const { return normal.Dot(p) + distance; }
};

struct Frustum {
    enum Side { Left = 0, Right, Bottom, Top, Near, Far, Count };
    VMPlane planes[Count];
};

// --- Triangle Utilities ---
namespace TriangleUtil {

// Compute triangle gradients for attribute interpolation
struct TriangleGradients {
    float dAttributedX; // dAttr/dx
    float dAttributedY; // dAttr/dy
};

// Setup edge equations for triangle rasterization
struct EdgeEquation {
    float a, b, c;  // ax + by + c = 0
    bool isTopLeft; // for fill rule
};

} // namespace TriangleUtil

// --- Interpolation Utilities ---
namespace Interp {

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Bilinear interpolation (for texture filtering)
inline float Bilinear(float c00, float c10, float c01, float c11, float u, float v) {
    float r0 = Lerp(c00, c10, u);
    float r1 = Lerp(c01, c11, u);
    return Lerp(r0, r1, v);
}

// Smooth step (cubic Hermite interpolation)
inline float SmoothStep(float edge0, float edge1, float x) {
    float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace Interp

// --- Fixed-Point Utilities ---
namespace FixedPoint {

// N64 uses various fixed-point formats
inline float S15_16ToFloat(int32_t v) { return static_cast<float>(v) / 65536.0f; }
inline int32_t FloatToS15_16(float f) { return static_cast<int32_t>(f * 65536.0f); }

inline float S10_5ToFloat(int16_t v) { return static_cast<float>(v) / 32.0f; }
inline int16_t FloatToS10_5(float f) { return static_cast<int16_t>(f * 32.0f); }

inline float S10_2ToFloat(int16_t v) { return static_cast<float>(v) / 4.0f; }
inline int16_t FloatToS10_2(float f) { return static_cast<int16_t>(f * 4.0f); }

inline float S5_10ToFloat(int16_t v) { return static_cast<float>(v) / 1024.0f; }
inline int16_t FloatToS5_10(float f) { return static_cast<int16_t>(f * 1024.0f); }

inline float U10_2ToFloat(uint16_t v) { return static_cast<float>(v) / 4.0f; }
inline float U5_11ToFloat(uint16_t v) { return static_cast<float>(v) / 2048.0f; }

// 1.11 DXT format
inline float DXT_1_11ToFloat(uint16_t v) { return static_cast<float>(v) / 2048.0f; }

// Quantize float to N64 color precision
inline uint8_t QuantizeColor8(float f) {
    return static_cast<uint8_t>(std::min(std::max(f * 255.0f + 0.5f, 0.0f), 255.0f));
}

inline uint8_t QuantizeColor5(float f) {
    return static_cast<uint8_t>(std::min(std::max(f * 31.0f + 0.5f, 0.0f), 31.0f));
}

} // namespace FixedPoint

} // namespace Math
} // namespace Video
} // namespace Nova64
