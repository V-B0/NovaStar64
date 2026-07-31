/**
 * @file N64Math.hpp
 * @brief Matrix4x4 RDRAM load + matrix multiply, used by the modelview/projection stack
 */

#pragma once

#include "N64Types.hpp"

#include <cstdint>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix Operations
// ═══════════════════════════════════════════════════════════════════════════

namespace MatrixOps {

/// Multiply two 4x4 matrices: result = A * B
void Multiply(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& result);

} // namespace MatrixOps

} // namespace Nova64::Video
