/**
 * @file VectorMath.cpp
 * @brief High-Level Vector/Matrix Math.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - High-Level Vector/Matrix Math Implementation

#include "VectorMath.hpp"
#include "RDRAMAccess.hpp"
#include <cstring>

namespace Nova64 { namespace Video { namespace Math {

// ═══════════════════════════════════════════════════════════════════════════════
// MATRIX CONSTRUCTION UTILITIES
// ═══════════════════════════════════════════════════════════════════════════════

Mat4x4 PerspectiveProjection(float fovYRadians, float aspect, float nearZ, float farZ) {
    float tanHalf = std::tan(fovYRadians * 0.5f);
    Mat4x4 m = Mat4x4::Zero();

    m(0,0) = 1.0f / (aspect * tanHalf);
    m(1,1) = 1.0f / tanHalf;
    m(2,2) = -(farZ + nearZ) / (farZ - nearZ);
    m(2,3) = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    m(3,2) = -1.0f;

    return m;
}

Mat4x4 OrthographicProjection(float left, float right, float bottom, float top,
                               float nearZ, float farZ) {
    Mat4x4 m = Mat4x4::Zero();

    m(0,0) = 2.0f / (right - left);
    m(1,1) = 2.0f / (top - bottom);
    m(2,2) = -2.0f / (farZ - nearZ);
    m(0,3) = -(right + left) / (right - left);
    m(1,3) = -(top + bottom) / (top - bottom);
    m(2,3) = -(farZ + nearZ) / (farZ - nearZ);
    m(3,3) = 1.0f;

    return m;
}

Mat4x4 LookAt(const Vec3f& eye, const Vec3f& target, const Vec3f& up) {
    Vec3f f = (target - eye).Normalized();
    Vec3f s = f.Cross(up).Normalized();
    Vec3f u = s.Cross(f);

    Mat4x4 m;
    m(0,0) = s.x;  m(0,1) = s.y;  m(0,2) = s.z;  m(0,3) = -s.Dot(Vec3f(eye.x, eye.y, eye.z));
    m(1,0) = u.x;  m(1,1) = u.y;  m(1,2) = u.z;  m(1,3) = -u.Dot(Vec3f(eye.x, eye.y, eye.z));
    m(2,0) = -f.x; m(2,1) = -f.y; m(2,2) = -f.z; m(2,3) = f.Dot(Vec3f(eye.x, eye.y, eye.z));
    m(3,0) = 0;    m(3,1) = 0;    m(3,2) = 0;    m(3,3) = 1.0f;

    return m;
}

Mat4x4 Translation(float x, float y, float z) {
    Mat4x4 m;
    m(0,3) = x;
    m(1,3) = y;
    m(2,3) = z;
    return m;
}

Mat4x4 Translation(const Vec3f& t) {
    return Translation(t.x, t.y, t.z);
}

Mat4x4 Scale(float x, float y, float z) {
    Mat4x4 m;
    m(0,0) = x;
    m(1,1) = y;
    m(2,2) = z;
    return m;
}

Mat4x4 Scale(float uniform) {
    return Scale(uniform, uniform, uniform);
}

Mat4x4 Scale(const Vec3f& s) {
    return Scale(s.x, s.y, s.z);
}

Mat4x4 RotationX(float radians) {
    float c = std::cos(radians);
    float s = std::sin(radians);
    Mat4x4 m;
    m(1,1) = c;  m(1,2) = -s;
    m(2,1) = s;  m(2,2) = c;
    return m;
}

Mat4x4 RotationY(float radians) {
    float c = std::cos(radians);
    float s = std::sin(radians);
    Mat4x4 m;
    m(0,0) = c;  m(0,2) = s;
    m(2,0) = -s; m(2,2) = c;
    return m;
}

Mat4x4 RotationZ(float radians) {
    float c = std::cos(radians);
    float s = std::sin(radians);
    Mat4x4 m;
    m(0,0) = c; m(0,1) = -s;
    m(1,0) = s; m(1,1) = c;
    return m;
}

Mat4x4 RotationAxis(const Vec3f& axis, float radians) {
    Vec3f a = axis.Normalized();
    float c = std::cos(radians);
    float s = std::sin(radians);
    float t = 1.0f - c;

    Mat4x4 m;
    m(0,0) = t*a.x*a.x + c;       m(0,1) = t*a.x*a.y - s*a.z;   m(0,2) = t*a.x*a.z + s*a.y;
    m(1,0) = t*a.x*a.y + s*a.z;   m(1,1) = t*a.y*a.y + c;       m(1,2) = t*a.y*a.z - s*a.x;
    m(2,0) = t*a.x*a.z - s*a.y;   m(2,1) = t*a.y*a.z + s*a.x;   m(2,2) = t*a.z*a.z + c;
    m(0,3) = m(1,3) = m(2,3) = 0;
    m(3,0) = m(3,1) = m(3,2) = 0; m(3,3) = 1.0f;

    return m;
}

// ═══════════════════════════════════════════════════════════════════════════════
// N64-SPECIFIC MATRIX UTILITIES
// ═══════════════════════════════════════════════════════════════════════════════

Mat4x4 LoadN64Matrix(const uint8_t* rdram, uint32_t address) {
    // N64 matrix format: first 32 bytes = integer parts (s15 each)
    // next 32 bytes = fractional parts (u16 each)
    // RDRAM uses 32-bit word-swapped storage

    Mat4x4 result;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int idx = row * 4 + col;
            uint32_t intOffset = address + idx * 2;
            uint32_t fracOffset = address + 32 + idx * 2;

            int16_t intPart = RDRAMReadHalfSigned(rdram, intOffset);
            uint16_t fracPart = RDRAMReadHalf(rdram, fracOffset);

            // Combine: integer.fraction as s15.16
            int32_t combined = (static_cast<int32_t>(intPart) << 16) | fracPart;
            result(row, col) = static_cast<float>(combined) / 65536.0f;
        }
    }
    return result;
}

N64Viewport LoadN64Viewport(const uint8_t* rdram, uint32_t address) {
    // N64 viewport struct (16 bytes):
    // [0-1] scale_x (s10.2), [2-3] scale_y (s10.2)
    // [4-5] scale_z (s10.2), [6-7] padding
    // [8-9] trans_x (s10.2), [10-11] trans_y (s10.2)
    // [12-13] trans_z (s10.2), [14-15] padding
    // RDRAM uses 32-bit word-swapped storage

    auto readS102 = [&](uint32_t off) -> float {
        int16_t raw = RDRAMReadHalfSigned(rdram, address + off);
        return static_cast<float>(raw) / 4.0f;
    };

    N64Viewport vp;
    float scaleX = readS102(0);
    float scaleY = readS102(2);
    float scaleZ = readS102(4);
    float transX = readS102(8);
    float transY = readS102(10);
    float transZ = readS102(12);

    vp.width  = std::fabs(scaleX);
    vp.height = std::fabs(scaleY);
    vp.x      = transX;
    vp.y      = transY;
    vp.nearZ  = transZ - std::fabs(scaleZ);
    vp.farZ   = transZ + std::fabs(scaleZ);

    return vp;
}

Vec3f ApplyN64Viewport(const Vec4f& clipSpace, const N64Viewport& vp) {
    // Perspective division
    if (std::fabs(clipSpace.f[3]) < kEpsilon) {
        return Vec3f(vp.x, vp.y, 0.0f);
    }
    float invW = 1.0f / clipSpace.f[3];
    float ndcX = clipSpace.f[0] * invW;
    float ndcY = clipSpace.f[1] * invW;
    float ndcZ = clipSpace.f[2] * invW;

    float screenX = vp.x + ndcX * vp.width;
    float screenY = vp.y + ndcY * vp.height;
    float depth   = (vp.nearZ + vp.farZ) * 0.5f + ndcZ * (vp.farZ - vp.nearZ) * 0.5f;

    return Vec3f(screenX, screenY, depth);
}

Vec2f N64ScreenToNDC(float screenX, float screenY, float screenW, float screenH) {
    return Vec2f(
        (screenX / screenW) * 2.0f - 1.0f,
        1.0f - (screenY / screenH) * 2.0f  // Y is flipped
    );
}

// ═══════════════════════════════════════════════════════════════════════════════
// FRUSTUM
// ═══════════════════════════════════════════════════════════════════════════════

Frustum Frustum::FromViewProjection(const Mat4x4& vp) {
    Frustum f;
    // Extract from combined view-projection matrix (column-major)
    // Row 0: vp.Row(0), etc.
    Vec4f r0 = vp.Row(0);
    Vec4f r1 = vp.Row(1);
    Vec4f r2 = vp.Row(2);
    Vec4f r3 = vp.Row(3);

    // Left:   row3 + row0
    f.planes[Left] = VMPlane(r3[0]+r0[0], r3[1]+r0[1], r3[2]+r0[2], r3[3]+r0[3]);
    // Right:  row3 - row0
    f.planes[Right] = VMPlane(r3[0]-r0[0], r3[1]-r0[1], r3[2]-r0[2], r3[3]-r0[3]);
    // Bottom: row3 + row1
    f.planes[Bottom] = VMPlane(r3[0]+r1[0], r3[1]+r1[1], r3[2]+r1[2], r3[3]+r1[3]);
    // Top:    row3 - row1
    f.planes[Top] = VMPlane(r3[0]-r1[0], r3[1]-r1[1], r3[2]-r1[2], r3[3]-r1[3]);
    // Near:   row3 + row2
    f.planes[Near] = VMPlane(r3[0]+r2[0], r3[1]+r2[1], r3[2]+r2[2], r3[3]+r2[3]);
    // Far:    row3 - row2
    f.planes[Far] = VMPlane(r3[0]-r2[0], r3[1]-r2[1], r3[2]-r2[2], r3[3]-r2[3]);

    return f;
}

bool Frustum::TestAABB(const AABB& box) const {
    for (int i = 0; i < Count; i++) {
        const VMPlane& p = planes[i];
        // Find the vertex most aligned with the plane normal (P-vertex)
        Vec3f pv;
        pv.x = (p.normal.x >= 0) ? box.max.x : box.min.x;
        pv.y = (p.normal.y >= 0) ? box.max.y : box.min.y;
        pv.z = (p.normal.z >= 0) ? box.max.z : box.min.z;

        if (p.DistanceToPoint(pv) < 0.0f) {
            return false; // Entirely outside this plane
        }
    }
    return true;
}

bool Frustum::TestSphere(const Vec3f& center, float radius) const {
    for (int i = 0; i < Count; i++) {
        if (planes[i].DistanceToPoint(center) < -radius) {
            return false;
        }
    }
    return true;
}

bool Frustum::TestPoint(const Vec3f& point) const {
    for (int i = 0; i < Count; i++) {
        if (planes[i].DistanceToPoint(point) < 0.0f) {
            return false;
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// TRIANGLE UTILITIES
// ═══════════════════════════════════════════════════════════════════════════════
namespace TriangleUtil {

float Area2D(const Vec2f& a, const Vec2f& b, const Vec2f& c) {
    return std::fabs(SignedArea2D(a, b, c));
}

float SignedArea2D(const Vec2f& a, const Vec2f& b, const Vec2f& c) {
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

Vec3f Barycentric(const Vec2f& p, const Vec2f& a, const Vec2f& b, const Vec2f& c) {
    float totalArea = SignedArea2D(a, b, c);
    if (std::fabs(totalArea) < kEpsilonSq) {
        return Vec3f(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f); // Degenerate
    }
    float invArea = 1.0f / totalArea;
    float u = SignedArea2D(p, b, c) * invArea;
    float v = SignedArea2D(a, p, c) * invArea;
    float w = 1.0f - u - v;
    return Vec3f(u, v, w);
}

bool PointInTriangle(const Vec2f& p, const Vec2f& a, const Vec2f& b, const Vec2f& c) {
    Vec3f bary = Barycentric(p, a, b, c);
    return bary.x >= 0.0f && bary.y >= 0.0f && bary.z >= 0.0f;
}

Vec3f FaceNormal(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    Vec3f edge1 = b - a;
    Vec3f edge2 = c - a;
    return edge1.Cross(edge2).Normalized();
}

float EdgeFunction(const Vec2f& a, const Vec2f& b, const Vec2f& p) {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

TriangleGradients ComputeGradients(
    const Vec2f& v0, const Vec2f& v1, const Vec2f& v2,
    float attr0, float attr1, float attr2)
{
    float dx01 = v0.x - v2.x;
    float dx12 = v1.x - v2.x;
    float dy01 = v0.y - v2.y;
    float dy12 = v1.y - v2.y;
    float da01 = attr0 - attr2;
    float da12 = attr1 - attr2;

    float det = dx01 * dy12 - dx12 * dy01;
    TriangleGradients grad;
    if (std::fabs(det) < kEpsilonSq) {
        grad.dAttributedX = 0.0f;
        grad.dAttributedY = 0.0f;
    } else {
        float invDet = 1.0f / det;
        grad.dAttributedX = (da01 * dy12 - da12 * dy01) * invDet;
        grad.dAttributedY = (dx01 * da12 - dx12 * da01) * invDet;
    }
    return grad;
}

void SetupEdgeEquations(const Vec2f& v0, const Vec2f& v1, const Vec2f& v2,
                        EdgeEquation edges[3]) {
    // Edge 0: v0 -> v1
    edges[0].a = v0.y - v1.y;
    edges[0].b = v1.x - v0.x;
    edges[0].c = v0.x * v1.y - v1.x * v0.y;
    // Top-left fill rule: top edge (horizontal going left) or left edge
    edges[0].isTopLeft = (v0.y == v1.y && v0.x > v1.x) || (v0.y < v1.y);

    // Edge 1: v1 -> v2
    edges[1].a = v1.y - v2.y;
    edges[1].b = v2.x - v1.x;
    edges[1].c = v1.x * v2.y - v2.x * v1.y;
    edges[1].isTopLeft = (v1.y == v2.y && v1.x > v2.x) || (v1.y < v2.y);

    // Edge 2: v2 -> v0
    edges[2].a = v2.y - v0.y;
    edges[2].b = v0.x - v2.x;
    edges[2].c = v2.x * v0.y - v0.x * v2.y;
    edges[2].isTopLeft = (v2.y == v0.y && v2.x > v0.x) || (v2.y < v0.y);
}

} // namespace TriangleUtil

// ═══════════════════════════════════════════════════════════════════════════════
// INTERPOLATION UTILITIES
// ═══════════════════════════════════════════════════════════════════════════════
namespace Interp {

Color4f BilinearColor(const Color4f& c00, const Color4f& c10,
                      const Color4f& c01, const Color4f& c11,
                      float u, float v) {
    float ou = 1.0f - u;
    float ov = 1.0f - v;
    float w00 = ou * ov;
    float w10 = u * ov;
    float w01 = ou * v;
    float w11 = u * v;
    return Color4f(
        c00.r*w00 + c10.r*w10 + c01.r*w01 + c11.r*w11,
        c00.g*w00 + c10.g*w10 + c01.g*w01 + c11.g*w11,
        c00.b*w00 + c10.b*w10 + c01.b*w01 + c11.b*w11,
        c00.a*w00 + c10.a*w10 + c01.a*w01 + c11.a*w11
    );
}

Vec3f PerspectiveCorrectBarycentric(const Vec3f& bary, float w0, float w1, float w2) {
    float b0 = bary.x * w0;
    float b1 = bary.y * w1;
    float b2 = bary.z * w2;
    float sum = b0 + b1 + b2;
    if (std::fabs(sum) < kEpsilonSq) {
        return Vec3f(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
    }
    float inv = 1.0f / sum;
    return Vec3f(b0 * inv, b1 * inv, b2 * inv);
}

float Hermite(float p0, float p1, float t0, float t1, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2*t3 - 3*t2 + 1;
    float h10 = t3 - 2*t2 + t;
    float h01 = -2*t3 + 3*t2;
    float h11 = t3 - t2;
    return h00*p0 + h10*t0 + h01*p1 + h11*t1;
}

float CatmullRom(float p0, float p1, float p2, float p3, float t) {
    float t0 = (p2 - p0) * 0.5f;
    float t1 = (p3 - p1) * 0.5f;
    return Hermite(p1, p2, t0, t1, t);
}

} // namespace Interp

// ═══════════════════════════════════════════════════════════════════════════════
// FIXED-POINT UTILITIES
// ═══════════════════════════════════════════════════════════════════════════════
namespace FixedPoint {

float ApplyTexCoordShift(int16_t rawCoord, int shift) {
    float coord = static_cast<float>(rawCoord) / 32.0f; // s10.5 -> float
    if (shift > 10) {
        // Left shift (scale up): shift = 16 - actual_shift
        coord = std::ldexp(coord, 16 - shift);
    } else if (shift > 0) {
        // Right shift (scale down)
        coord = std::ldexp(coord, -shift);
    }
    return coord;
}

} // namespace FixedPoint

}}} // namespace Nova64::Video::Math
