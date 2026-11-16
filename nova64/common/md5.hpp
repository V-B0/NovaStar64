/**
 * @file md5.hpp
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

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace Common {

/**
 * @struct MD5Digest
 * @brief Structure to hold a 16-byte MD5 digest
 */
struct MD5Digest {
    uint8_t digest[16];

    MD5Digest() { Reset(); }

    void Reset() { std::memset(digest, 0, sizeof(digest)); }

    bool IsClear() const {
        int isClear = 0;
        for (int i = 0; i < 16; i++) {
            isClear += digest[i];
        }
        return (isClear == 0);
    }

    std::string ToString() const {
        char s[33];
        std::memset(s, 0, sizeof(s));
        for (int i = 0; i < 16; i++) {
            std::snprintf(s + i * 2, 3, "%02x", digest[i]);
        }
        s[32] = '\0';
        return std::string(s);
    }

    std::string ToUpperString() const {
        char s[33];
        std::memset(s, 0, sizeof(s));
        for (int i = 0; i < 16; i++) {
            std::snprintf(s + i * 2, 3, "%02X", digest[i]);
        }
        s[32] = '\0';
        return std::string(s);
    }

    bool operator==(const MD5Digest& other) const { return std::memcmp(digest, other.digest, 16) == 0; }

    bool operator!=(const MD5Digest& other) const { return !(*this == other); }

    bool operator<(const MD5Digest& other) const { return std::memcmp(digest, other.digest, 16) < 0; }
};

/**
 * @class MD5
 * @brief MD5 Message-Digest Algorithm implementation
 */
class MD5 {
public:
    // Simple initializer
    MD5();
    ~MD5() = default;

    // Methods for controlled operation
    /**
     * @brief Update the digest with additional data
     * @param input Pointer to input data
     * @param inputLength Length of input data
     */
    void Update(const uint8_t* input, size_t inputLength);

    /**
     * @brief Finalize the MD5 calculation
     */
    void Finalize();

    // Constructors for special circumstances - all finalize the MD5 context
    /**
     * @brief Construct and compute MD5 of a file
     * @param filePath Path to the file
     */
    explicit MD5(const fs::path& filePath);

    /**
     * @brief Construct and compute MD5 of a byte array
     * @param input Pointer to input data
     * @param inputLength Length of input data
     */
    MD5(const uint8_t* input, size_t inputLength);

    /**
     * @brief Construct and compute MD5 of a string
     * @param str Input string
     */
    explicit MD5(const std::string& str);

    // Methods to acquire finalized result
    /**
     * @brief Get digest as a structure
     * @param outDigest Output digest structure
     */
    void GetDigest(MD5Digest& outDigest) const;

    /**
     * @brief Get raw digest as a 16-byte array
     * @return Pointer to 16-byte digest
     */
    const uint8_t* RawDigest() const;

    /**
     * @brief Get digest as a 32-character hex string (lowercase)
     * @return Hex string representation
     */
    std::string HexDigest() const;

    /**
     * @brief Get digest as a 32-character hex string (uppercase)
     * @return Hex string representation
     */
    std::string HexDigestUpper() const;

private:
    // Type definitions
    using uint4 = uint32_t;
    using uint2 = uint16_t;
    using uint1 = uint8_t;

    // Private data
    uint4 m_state[4];
    uint4 m_count[2];   // Number of *bits*, mod 2^64
    uint1 m_buffer[64]; // Input buffer
    uint1 m_digest[16];
    bool m_finalized;
    mutable std::string m_hexDigest;

    // Private methods
    void Init();
    void Transform(const uint1* buffer);

    static void Encode(uint1* dest, const uint4* src, size_t length);
    static void Decode(uint4* dest, const uint1* src, size_t length);

    // MD5 basic functions
    static inline uint4 RotateLeft(uint4 x, uint4 n);
    static inline uint4 F(uint4 x, uint4 y, uint4 z);
    static inline uint4 G(uint4 x, uint4 y, uint4 z);
    static inline uint4 H(uint4 x, uint4 y, uint4 z);
    static inline uint4 I(uint4 x, uint4 y, uint4 z);

    // MD5 transformation functions
    static inline void FF(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac);
    static inline void GG(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac);
    static inline void HH(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac);
    static inline void II(uint4& a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s, uint4 ac);
};

} // namespace Common
