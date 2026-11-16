/**
 * @file md5.cpp
 * @brief MD5 Message-Digest Algorithm implementation
 *
 * Based on RFC 1321 and the RSA Data Security, Inc. reference implementation. The notices
 * below come from that code and its C++ translation, and must be retained:
 *
 * MD5.CC - source code for the C++/object oriented translation and modification of MD5.
 *
 * Translation and modification (c) 1995 by Mordechai T. Abzug
 *
 * This translation/ modification is provided "as is," without express or
 * implied warranty of any kind.
 *
 * The translator/ modifier does not claim (1) that MD5 will do what you think
 * it does; (2) that this translation/ modification is accurate; or (3) that
 * this software is "merchantable."  (language for this disclaimer partially
 * copied from the disclaimer below)
 *
 * based on:
 *
 * MD5.H - header file for MD5C.C
 * MDDRIVER.C - test driver for MD2, MD4 and MD5
 *
 * Copyright (C) 1991-1992, RSA Data Security, Inc. Created 1991. All rights reserved.
 *
 * License to copy and use this software is granted provided that it
 * is identified as the "RSA Data Security, Inc. MD5 Message-Digest
 * Algorithm" in all material mentioning or referencing this software
 * or this function.
 *
 * License is also granted to make and use derivative works provided
 * that such works are identified as "derived from the RSA Data
 * Security, Inc. MD5 Message-Digest Algorithm" in all material
 * mentioning or referencing the derived work.
 *
 * RSA Data Security, Inc. makes no representations concerning either
 * the merchantability of this software or the suitability of this
 * software for any particular purpose. It is provided "as is"
 * without express or implied warranty of any kind.
 *
 * These notices must be retained in any copies of any part of this
 * documentation and/or software.
 */

#include "md5.hpp"
#include <fstream>
#include <cstring>

namespace Common {

// Constants for MD5 Transform routine
static constexpr uint32_t S11 = 7;
static constexpr uint32_t S12 = 12;
static constexpr uint32_t S13 = 17;
static constexpr uint32_t S14 = 22;
static constexpr uint32_t S21 = 5;
static constexpr uint32_t S22 = 9;
static constexpr uint32_t S23 = 14;
static constexpr uint32_t S24 = 20;
static constexpr uint32_t S31 = 4;
static constexpr uint32_t S32 = 11;
static constexpr uint32_t S33 = 16;
static constexpr uint32_t S34 = 23;
static constexpr uint32_t S41 = 6;
static constexpr uint32_t S42 = 10;
static constexpr uint32_t S43 = 15;
static constexpr uint32_t S44 = 21;

// Padding for finalization
static const uint8_t PADDING[64] = {0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// ============================================================================
// Public Methods
// ============================================================================

MD5::MD5() { Init(); }

MD5::MD5(const fs::path& filePath) {
    Init();

    if (fs::exists(filePath)) {
        std::ifstream file(filePath, std::ios::binary);
        if (file) {
            uint8_t buffer[1024];
            while (file) {
                file.read(reinterpret_cast<char*>(buffer), sizeof(buffer));
                size_t bytesRead = static_cast<size_t>(file.gcount());
                if (bytesRead > 0) {
                    Update(buffer, bytesRead);
                }
            }
        }
    }
    Finalize();
}

MD5::MD5(const uint8_t* input, size_t inputLength) {
    Init();
    Update(input, inputLength);
    Finalize();
}

MD5::MD5(const std::string& str) {
    Init();
    Update(reinterpret_cast<const uint8_t*>(str.c_str()), str.length());
    Finalize();
}

void MD5::Update(const uint8_t* input, size_t inputLength) {
    if (m_finalized) {
        return;
    }

    // Compute number of bytes mod 64
    size_t bufferIndex = static_cast<size_t>((m_count[0] >> 3) & 0x3F);

    // Update number of bits
    m_count[0] += static_cast<uint4>(inputLength << 3);
    if (m_count[0] < (inputLength << 3)) {
        m_count[1]++;
    }
    m_count[1] += static_cast<uint4>(inputLength >> 29);

    size_t bufferSpace = 64 - bufferIndex;
    size_t inputIndex = 0;

    // Transform as many times as possible
    if (inputLength >= bufferSpace) {
        // Fill the rest of the buffer and transform
        std::memcpy(m_buffer + bufferIndex, input, bufferSpace);
        Transform(m_buffer);

        // Now, transform each 64-byte piece of the input
        for (inputIndex = bufferSpace; inputIndex + 63 < inputLength; inputIndex += 64) {
            Transform(input + inputIndex);
        }

        bufferIndex = 0;
    }

    // Buffer remaining input
    std::memcpy(m_buffer + bufferIndex, input + inputIndex, inputLength - inputIndex);
}

void MD5::Finalize() {
    if (m_finalized) {
        return;
    }

    uint8_t bits[8];

    // Save number of bits
    Encode(bits, m_count, 8);

    // Pad out to 56 mod 64
    size_t index = static_cast<size_t>((m_count[0] >> 3) & 0x3F);
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);
    Update(PADDING, padLen);

    // Append length (before padding)
    Update(bits, 8);

    // Store state in digest
    Encode(m_digest, m_state, 16);

    // Zeroize sensitive information
    std::memset(m_buffer, 0, sizeof(m_buffer));

    m_finalized = true;
}

void MD5::GetDigest(MD5Digest& outDigest) const {
    if (!m_finalized) {
        outDigest.Reset();
        return;
    }
    std::memcpy(outDigest.digest, m_digest, 16);
}

const uint8_t* MD5::RawDigest() const {
    if (!m_finalized) {
        static const uint8_t empty[16] = {0};
        return empty;
    }
    return m_digest;
}

std::string MD5::HexDigest() const {
    if (!m_finalized) {
        return "";
    }

    if (!m_hexDigest.empty()) {
        return m_hexDigest;
    }

    char c[33];
    std::memset(c, 0, sizeof(c));
    for (int i = 0; i < 16; i++) {
        std::snprintf(c + i * 2, 3, "%02x", m_digest[i]);
    }
    c[32] = '\0';

    m_hexDigest = c;
    return m_hexDigest;
}

std::string MD5::HexDigestUpper() const {
    if (!m_finalized) {
        return "";
    }

    char c[33];
    std::memset(c, 0, sizeof(c));
    for (int i = 0; i < 16; i++) {
        std::snprintf(c + i * 2, 3, "%02X", m_digest[i]);
    }
    c[32] = '\0';

    return std::string(c);
}

// ============================================================================
// Private Methods
// ============================================================================

void MD5::Init() {
    m_finalized = false;

    // Initialize bit count
    m_count[0] = 0;
    m_count[1] = 0;

    // Load magic initialization constants
    m_state[0] = 0x67452301;
    m_state[1] = 0xefcdab89;
    m_state[2] = 0x98badcfe;
    m_state[3] = 0x10325476;

    std::memset(m_digest, 0, sizeof(m_digest));
    std::memset(m_buffer, 0, sizeof(m_buffer));
    m_hexDigest.clear();
}

void MD5::Transform(const uint1* block) {
    uint4 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    uint4 x[16];

    Decode(x, block, 64);

    // Round 1
    FF(a, b, c, d, x[0], S11, 0xd76aa478);
    FF(d, a, b, c, x[1], S12, 0xe8c7b756);
    FF(c, d, a, b, x[2], S13, 0x242070db);
    FF(b, c, d, a, x[3], S14, 0xc1bdceee);
    FF(a, b, c, d, x[4], S11, 0xf57c0faf);
    FF(d, a, b, c, x[5], S12, 0x4787c62a);
    FF(c, d, a, b, x[6], S13, 0xa8304613);
    FF(b, c, d, a, x[7], S14, 0xfd469501);
    FF(a, b, c, d, x[8], S11, 0x698098d8);
    FF(d, a, b, c, x[9], S12, 0x8b44f7af);
    FF(c, d, a, b, x[10], S13, 0xffff5bb1);
    FF(b, c, d, a, x[11], S14, 0x895cd7be);
    FF(a, b, c, d, x[12], S11, 0x6b901122);
    FF(d, a, b, c, x[13], S12, 0xfd987193);
    FF(c, d, a, b, x[14], S13, 0xa679438e);
    FF(b, c, d, a, x[15], S14, 0x49b40821);

    // Round 2
    GG(a, b, c, d, x[1], S21, 0xf61e2562);
    GG(d, a, b, c, x[6], S22, 0xc040b340);
    GG(c, d, a, b, x[11], S23, 0x265e5a51);
    GG(b, c, d, a, x[0], S24, 0xe9b6c7aa);
    GG(a, b, c, d, x[5], S21, 0xd62f105d);
    GG(d, a, b, c, x[10], S22, 0x02441453);
    GG(c, d, a, b, x[15], S23, 0xd8a1e681);
    GG(b, c, d, a, x[4], S24, 0xe7d3fbc8);
    GG(a, b, c, d, x[9], S21, 0x21e1cde6);
    GG(d, a, b, c, x[14], S22, 0xc33707d6);
    GG(c, d, a, b, x[3], S23, 0xf4d50d87);
    GG(b, c, d, a, x[8], S24, 0x455a14ed);
    GG(a, b, c, d, x[13], S21, 0xa9e3e905);
    GG(d, a, b, c, x[2], S22, 0xfcefa3f8);
    GG(c, d, a, b, x[7], S23, 0x676f02d9);
    GG(b, c, d, a, x[12], S24, 0x8d2a4c8a);

    // Round 3
    HH(a, b, c, d, x[5], S31, 0xfffa3942);
    HH(d, a, b, c, x[8], S32, 0x8771f681);
    HH(c, d, a, b, x[11], S33, 0x6d9d6122);
    HH(b, c, d, a, x[14], S34, 0xfde5380c);
    HH(a, b, c, d, x[1], S31, 0xa4beea44);
    HH(d, a, b, c, x[4], S32, 0x4bdecfa9);
    HH(c, d, a, b, x[7], S33, 0xf6bb4b60);
    HH(b, c, d, a, x[10], S34, 0xbebfbc70);
    HH(a, b, c, d, x[13], S31, 0x289b7ec6);
    HH(d, a, b, c, x[0], S32, 0xeaa127fa);
    HH(c, d, a, b, x[3], S33, 0xd4ef3085);
    HH(b, c, d, a, x[6], S34, 0x04881d05);
    HH(a, b, c, d, x[9], S31, 0xd9d4d039);
    HH(d, a, b, c, x[12], S32, 0xe6db99e5);
    HH(c, d, a, b, x[15], S33, 0x1fa27cf8);
    HH(b, c, d, a, x[2], S34, 0xc4ac5665);

    // Round 4
    II(a, b, c, d, x[0], S41, 0xf4292244);
    II(d, a, b, c, x[7], S42, 0x432aff97);
    II(c, d, a, b, x[14], S43, 0xab9423a7);
    II(b, c, d, a, x[5], S44, 0xfc93a039);
    II(a, b, c, d, x[12], S41, 0x655b59c3);
    II(d, a, b, c, x[3], S42, 0x8f0ccc92);
    II(c, d, a, b, x[10], S43, 0xffeff47d);
    II(b, c, d, a, x[1], S44, 0x85845dd1);
    II(a, b, c, d, x[8], S41, 0x6fa87e4f);
    II(d, a, b, c, x[15], S42, 0xfe2ce6e0);
    II(c, d, a, b, x[6], S43, 0xa3014314);
    II(b, c, d, a, x[13], S44, 0x4e0811a1);
    II(a, b, c, d, x[4], S41, 0xf7537e82);
    II(d, a, b, c, x[11], S42, 0xbd3af235);
    II(c, d, a, b, x[2], S43, 0x2ad7d2bb);
    II(b, c, d, a, x[9], S44, 0xeb86d391);

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;

    // Zeroize sensitive information
    std::memset(x, 0, sizeof(x));
}

void MD5::Encode(uint1* dest, const uint4* src, size_t length) {
    for (size_t i = 0, j = 0; j < length; i++, j += 4) {
        dest[j] = static_cast<uint1>(src[i] & 0xff);
        dest[j + 1] = static_cast<uint1>((src[i] >> 8) & 0xff);
        dest[j + 2] = static_cast<uint1>((src[i] >> 16) & 0xff);
        dest[j + 3] = static_cast<uint1>((src[i] >> 24) & 0xff);
    }
}

void MD5::Decode(uint4* dest, const uint1* src, size_t length) {
    for (size_t i = 0, j = 0; j < length; i++, j += 4) {
        dest[i] = static_cast<uint4>(src[j]) | (static_cast<uint4>(src[j + 1]) << 8) |
                  (static_cast<uint4>(src[j + 2]) << 16) | (static_cast<uint4>(src[j + 3]) << 24);
    }
}

inline uint32_t MD5::RotateLeft(uint4 x, uint4 n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t MD5::F(uint4 x, uint4 y, uint4 z) { return (x & y) | (~x & z); }

inline uint32_t MD5::G(uint4 x, uint4 y, uint4 z) { return (x & z) | (y & ~z); }

inline uint32_t MD5::H(uint4 x, uint4 y, uint4 z) { return x ^ y ^ z; }

inline uint32_t MD5::I(uint4 x, uint4 y, uint4 z) { return y ^ (x | ~z); }

inline void MD5::FF(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac) {
    a += F(b, c, d) + x + ac;
    a = RotateLeft(a, s) + b;
}

inline void MD5::GG(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac) {
    a += G(b, c, d) + x + ac;
    a = RotateLeft(a, s) + b;
}

inline void MD5::HH(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac) {
    a += H(b, c, d) + x + ac;
    a = RotateLeft(a, s) + b;
}

inline void MD5::II(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac) {
    a += I(b, c, d) + x + ac;
    a = RotateLeft(a, s) + b;
}

} // namespace Common
