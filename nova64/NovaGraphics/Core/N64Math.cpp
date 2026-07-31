/**
 * @file N64Math.cpp
 * @brief Matrix4x4 RDRAM load + matrix multiply, used by the modelview/projection stack
 */

#include "N64Math.hpp"
#include "../Common/RDRAMAccess.hpp"

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix4x4::LoadFromN64
// ═══════════════════════════════════════════════════════════════════════════

void Matrix4x4::LoadFromN64(const uint8_t* rdram, uint32_t address) {
    // N64 matrix format: 64 bytes total
    // Bytes 0-31: 4x4 int16 (integer parts)
    // Bytes 32-63: 4x4 uint16 (fraction parts)
    // RDRAM uses 32-bit word-swapped storage
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            uint32_t intOff = address + (i * 8) + (j * 2);
            uint32_t fracOff = address + 32 + (i * 8) + (j * 2);

            int16_t intPart = RDRAMReadHalfSigned(rdram, intOff);
            uint16_t fracPart = RDRAMReadHalf(rdram, fracOff);

            // Combine: integer + fraction/65536
            m[i][j] = static_cast<float>(intPart) + static_cast<float>(fracPart) / 65536.0f;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix Operations
// ═══════════════════════════════════════════════════════════════════════════

namespace MatrixOps {

void Multiply(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& result) {
    Matrix4x4 temp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp.m[i][j] = a.m[i][0]*b.m[0][j] + a.m[i][1]*b.m[1][j] +
                           a.m[i][2]*b.m[2][j] + a.m[i][3]*b.m[3][j];
        }
    }
    result = temp;
}

} // namespace MatrixOps

} // namespace Nova64::Video
