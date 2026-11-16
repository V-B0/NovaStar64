/**
 * @file RDPCommand.hpp
 * @brief RDP command parsing and decoding
 *
 * Decodes raw 64-bit RDP command words into structured representations (triangles, rectangles,
 * texture loads and the state-setting commands) and provides the buffer that queues commands.
 */

#pragma once

#include "N64Types.hpp"
#include "RDPState.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace Nova64::Video {

// --- Decoded Triangle Edge Coefficients ---

struct TriangleEdge {
    // Major edge (longest edge)
    double yh;      // Y top (s11.2 → double, top of triangle)
    double ym;      // Y middle
    double yl;      // Y bottom (s11.2 → double, bottom of triangle)

    double xh;      // X at top (s15.16 → double)
    double xm;      // X at middle
    double xl;      // X at bottom

    double dxhDy;   // dX/dY for high edge
    double dxmDy;   // dX/dY for middle edge
    double dxlDy;   // dX/dY for low edge

    bool leftMajor; // Left-major triangle
};

/// Shade (color) coefficients for a triangle
struct TriangleShade {
    float r, g, b, a; // Color at start pixel
    float drDx, dgDx, dbDx, daDx;
    float drDe, dgDe, dbDe, daDe;
    float drDy, dgDy, dbDy, daDy;
};

/// Texture coefficients for a triangle
struct TriangleTexture {
    float s, t, w;          // Start values
    float dsDx, dtDx, dwDx; // X derivatives
    float dsDe, dtDe, dwDe; // Edge derivatives
    float dsDy, dtDy, dwDy; // Y derivatives
};

/// Depth (Z) coefficients for a triangle
struct TriangleDepth {
    float z;
    float dzDx, dzDe, dzDy;
};

/// Fully decoded triangle command
struct DecodedTriangle {
    TriangleEdge edge;
    TriangleShade shade;
    TriangleTexture texture;
    TriangleDepth depth;

    uint8_t tileIndex;
    bool hasShade;
    bool hasTexture;
    bool hasDepth;
};

// --- Decoded Rectangle Command ---

struct DecodedRectangle {
    float x0, y0, x1, y1; // Screen coordinates
    float s, t;           // Texture start (s10.5 → float)
    float dsDx, dtDy;     // Texture slopes
    uint8_t tileIndex;
    bool flipped;         // TextureRectFlip swaps S/T increments
    // Optional texture-sampling bounds, used instead of x0/y0/x1/y1 when >= 0. They let sampling be
    // clamped to a smaller rect than the one drawn, which a COPY-mode block expansion would need:
    // otherwise up to 3 garbage texel columns/rows appear on each side (HUD glyph fringes). Nothing
    // sets them, so DrawTextureRect always bounds sampling by the rect corners.
    float boundX0 = -1.0f, boundY0 = -1.0f; // <0 = use x0/y0
    float boundX1 = -1.0f, boundY1 = -1.0f; // <0 = use x1/y1
};

// --- RDP Command Decoder ---

class RDPCommandDecoder {
public:
    RDPCommandDecoder() = default;

    void SetRDRAM(const uint8_t* rdram, size_t rdramSize) {
        m_rdram = rdram;
        m_rdramSize = rdramSize;
    }

    /// Command ID from the top bits of the first word. Consumes nothing; the caller advances by the
    /// command's length.
    RDPCommandID DecodeCommand(const uint32_t* cmdWords, int wordCount);

    // --- State-Setting Command Decoders ---

    /// Set Other Modes (0x2F): 64-bit mode word
    void DecodeSetOtherModes(const uint32_t* words, RDPState& state);

    /// Set Combine (0x3C): 64-bit combine mode
    void DecodeSetCombine(const uint32_t* words, RDPState& state);

    /// Set Tile (0x35): Configure a tile descriptor
    void DecodeSetTile(const uint32_t* words, TileDescriptor& tile);

    /// Set Tile Size (0x32): Set the rectangle within the tile
    void DecodeSetTileSize(const uint32_t* words, TileDescriptor& tile);

    /// Set Scissor (0x2D)
    void DecodeSetScissor(const uint32_t* words, ScissorRect& scissor);

    /// Set Color Image (0x3F)
    void DecodeSetColorImage(const uint32_t* words, ImageDescriptor& img);

    /// Set Mask Image (0x3E, Z-buffer)
    void DecodeSetMaskImage(const uint32_t* words, ImageDescriptor& img);

    /// Set Texture Image (0x3D, source for loads)
    void DecodeSetTextureImage(const uint32_t* words, ImageDescriptor& img);

    /// Set Fill Color (0x37)
    void DecodeSetFillColor(const uint32_t* words, RDPColor& color);

    /// Set Fog Color (0x38)
    void DecodeSetFogColor(const uint32_t* words, RDPColor& color);

    /// Set Blend Color (0x39)
    void DecodeSetBlendColor(const uint32_t* words, RDPColor& color);

    /// Set Prim Color (0x3A)
    void DecodeSetPrimColor(const uint32_t* words, RDPState& state);

    /// Set Env Color (0x3B)
    void DecodeSetEnvColor(const uint32_t* words, RDPColor& color);

    /// Set Prim Depth (0x2E)
    void DecodeSetPrimDepth(const uint32_t* words, uint16_t& depth, uint16_t& deltaZ);

    /// Set Convert (0x2C)
    void DecodeSetConvert(const uint32_t* words, int16_t convertK[6]);

    /// Set Key GB (0x2A), Set Key R (0x2B)
    void DecodeSetKeyGB(const uint32_t* words, RDPState& state);
    void DecodeSetKeyR(const uint32_t* words, RDPState& state);

    // --- Texture Load Decoders ---

    /// Load Block (0x33): Load a contiguous block of texels into TMEM
    void DecodeLoadBlock(const uint32_t* words, int& tile, int& uls, int& ult, int& texels, int& dxt);

    /// Load Tile (0x34): Load a rectangular region into TMEM
    void DecodeLoadTile(const uint32_t* words, int& tile, float& sl, float& tl, float& sh, float& th);

    /// Load TLUT (0x30): Load a palette into TMEM
    void DecodeLoadTLUT(const uint32_t* words, int& tile, int& count);

    // --- Primitive Decoders ---

    /// Decode a triangle command (0x08-0x0F)
    void DecodeTriangle(const uint32_t* words, int wordCount, DecodedTriangle& tri);

    /// Decode TextureRectangle (0x24)
    void DecodeTextureRect(const uint32_t* words, DecodedRectangle& rect);

    /// Decode TextureRectFlip (0x25)
    void DecodeTextureRectFlip(const uint32_t* words, DecodedRectangle& rect);

    /// Decode Fill Rectangle (0x36)
    void DecodeFillRect(const uint32_t* words, float& x0, float& y0, float& x1, float& y1);

    // --- Debug ---

    /// Get a human-readable description of a command
    static std::string DescribeCommand(RDPCommandID cmd);

    static const char* GetCommandName(RDPCommandID cmd);

private:
    const uint8_t* m_rdram = nullptr;
    size_t m_rdramSize = 0;

    /// Read a 32-bit word from RDRAM in native word order (PJ64-style); 0 if out of range
    uint32_t ReadWord(uint32_t address) const;

    /// Signed fixed point to double: s15.16 from a high/low half pair, s11.2 from a packed field
    static double ReadFixed_S15_16(uint32_t hi, uint32_t lo);
    static double ReadFixed_S11_2(uint32_t packed);
};

// --- RDP Command Buffer ---

/// Buffered RDP commands waiting to be processed
class RDPCommandBuffer {
public:
    static constexpr size_t MAX_BUFFER_SIZE = 0x100000; // 1 MB max

    RDPCommandBuffer();
    ~RDPCommandBuffer();

    void Reset();

    /// Append words; if the buffer is still full after compaction, all unread data is discarded
    void Push(const uint32_t* words, int count);

    /// Pointer to the next unread word, or nullptr if the buffer is empty
    const uint32_t* Peek() const;

    /// Advance the read offset, clamped to the end of the data
    void Advance(int wordCount);

    bool HasCommands() const { return m_readOffset < m_writeOffset; }

    size_t GetPendingWordCount() const { return m_writeOffset - m_readOffset; }

    /// Compact buffer (move unread data to front)
    void Compact();

private:
    uint32_t* m_buffer;
    size_t m_readOffset;
    size_t m_writeOffset;
    size_t m_capacity;
};

} // namespace Nova64::Video
