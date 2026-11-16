/**
 * @file RDPCommand.cpp
 * @brief RDP command decoding implementation
 */

#include "RDPCommand.hpp"
#include "../Common/Logger.hpp"

#include <cstring>
#include <cstdlib>

namespace Nova64::Video {

// ═══════════════════════════════════════════════════════════════════════════
//  Helper Functions
// ═══════════════════════════════════════════════════════════════════════════

uint32_t RDPCommandDecoder::ReadWord(uint32_t address) const {
    if (!m_rdram || address + 3 >= m_rdramSize) return 0;
    // RDRAM is stored in native 32-bit word order (PJ64-style)
    return *reinterpret_cast<const uint32_t*>(m_rdram + address);
}

double RDPCommandDecoder::ReadFixed_S15_16(uint32_t hi, uint32_t lo) {
    int32_t value = (static_cast<int32_t>(hi) << 16) | (lo & 0xFFFF);
    return static_cast<double>(value) / 65536.0;
}

double RDPCommandDecoder::ReadFixed_S11_2(uint32_t packed) {
    int16_t raw = static_cast<int16_t>(packed & 0x3FFF);
    if (packed & 0x2000) raw |= 0xC000; // Sign extend
    return static_cast<double>(raw) / 4.0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Command ID Decoding
// ═══════════════════════════════════════════════════════════════════════════

RDPCommandID RDPCommandDecoder::DecodeCommand(const uint32_t* cmdWords, int /*wordCount*/) {
    uint8_t cmdByte = (cmdWords[0] >> 24) & 0x3F;
    return static_cast<RDPCommandID>(cmdByte);
}

// ═══════════════════════════════════════════════════════════════════════════
//  State-Setting Decoders
// ═══════════════════════════════════════════════════════════════════════════

void RDPCommandDecoder::DecodeSetOtherModes(const uint32_t* words, RDPState& state) {
    // Word 0: cmd[7:0] | otherModeH[23:0]
    // Word 1: otherModeL[31:0]
    state.otherModeH = words[0] & 0x00FFFFFF;
    state.otherModeL = words[1];
    // Keep RDP-level copies that persist across GBI SetOtherModeH/L overwrites.
    // On real N64 hardware, the RSP shadow (GBI 0xE2/0xE3) and RDP state (0xEF)
    // are independent; in HLE they share otherModeH/L.  FillRect and TextureRect
    // (RDP passthrough draws) should use the 0xEF-level state, not the GBI shadow.
    state.rdpOtherModeH = state.otherModeH;
    state.rdpOtherModeL = state.otherModeL;
    state.UpdateDerivedOtherModes();
    state.MarkDirty(RDPDirtyFlag::OtherModes | RDPDirtyFlag::BlendMode);
}

void RDPCommandDecoder::DecodeSetCombine(const uint32_t* words, RDPState& state) {
    state.combineModeRaw = (static_cast<uint64_t>(words[0] & 0x00FFFFFF) << 32) | static_cast<uint64_t>(words[1]);
    state.combineMode.Decode(state.combineModeRaw);
    state.MarkDirty(RDPDirtyFlag::CombineMode);
}

void RDPCommandDecoder::DecodeSetTile(const uint32_t* words, TileDescriptor& tile) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    tile.format = static_cast<uint8_t>((w0 >> 21) & 0x07);
    tile.size = static_cast<uint8_t>((w0 >> 19) & 0x03);
    tile.line = static_cast<uint16_t>((w0 >> 9) & 0x1FF);
    tile.tmemAddr = static_cast<uint16_t>(w0 & 0x1FF);
    tile.palette = static_cast<uint8_t>((w1 >> 20) & 0x0F);

    // clampT encodes: bit1=clamp, bit0=mirror
    tile.clampT = static_cast<uint8_t>((((w1 >> 19) & 1) << 1) | ((w1 >> 18) & 1));
    tile.maskT = static_cast<uint8_t>((w1 >> 14) & 0x0F);
    tile.shiftT = static_cast<uint8_t>((w1 >> 10) & 0x0F);

    // clampS encodes: bit1=clamp, bit0=mirror
    tile.clampS = static_cast<uint8_t>((((w1 >> 9) & 1) << 1) | ((w1 >> 8) & 1));
    tile.maskS = static_cast<uint8_t>((w1 >> 4) & 0x0F);
    tile.shiftS = static_cast<uint8_t>(w1 & 0x0F);

    // N64 hardware rule: when mask is 0 (no wrap mask), force clamp ON.
    // Without this, coordinates pass through unmodified causing texture bleeding.
    if (tile.maskS == 0) tile.clampS |= 2;
    if (tile.maskT == 0) tile.clampT |= 2;
}

void RDPCommandDecoder::DecodeSetTileSize(const uint32_t* words, TileDescriptor& tile) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    // Store as raw 10.2 fixed-point int32_t values
    tile.sl = static_cast<int32_t>((w0 >> 12) & 0xFFF);
    tile.tl = static_cast<int32_t>(w0 & 0xFFF);
    tile.sh = static_cast<int32_t>((w1 >> 12) & 0xFFF);
    tile.th = static_cast<int32_t>(w1 & 0xFFF);
}

void RDPCommandDecoder::DecodeSetScissor(const uint32_t* words, ScissorRect& scissor) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    // Store as raw 11.2 fixed-point int32_t values (xh/yh=upper-left, xl/yl=lower-right)
    scissor.xh = static_cast<int32_t>((w0 >> 12) & 0xFFF);
    scissor.yh = static_cast<int32_t>(w0 & 0xFFF);
    scissor.mode = static_cast<uint8_t>((w1 >> 24) & 0x03);
    scissor.xl = static_cast<int32_t>((w1 >> 12) & 0xFFF);
    scissor.yl = static_cast<int32_t>(w1 & 0xFFF);
}

void RDPCommandDecoder::DecodeSetColorImage(const uint32_t* words, ImageDescriptor& img) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    img.format = static_cast<ImageFormat>((w0 >> 21) & 0x07);
    img.size = static_cast<ImageSize>((w0 >> 19) & 0x03);
    img.width = (w0 & 0xFFF) + 1;
    img.address = w1 & 0x03FFFFFF;
}

void RDPCommandDecoder::DecodeSetMaskImage(const uint32_t* words, ImageDescriptor& img) {
    img.address = words[1] & 0x03FFFFFF;
    // Mask image inherits format/size/width from color image typically
}

void RDPCommandDecoder::DecodeSetTextureImage(const uint32_t* words, ImageDescriptor& img) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    img.format = static_cast<ImageFormat>((w0 >> 21) & 0x07);
    img.size = static_cast<ImageSize>((w0 >> 19) & 0x03);
    img.width = (w0 & 0xFFF) + 1;
    img.address = w1 & 0x03FFFFFF;
}

void RDPCommandDecoder::DecodeSetFillColor(const uint32_t* words, RDPColor& color) {
    // Fill color is packed differently depending on framebuffer format:
    // 16-bit: two 16-bit pixels packed into 32 bits
    // 32-bit: one RGBA8888 pixel
    color.FromCommand(words[1]);
}

void RDPCommandDecoder::DecodeSetFogColor(const uint32_t* words, RDPColor& color) { color.FromCommand(words[1]); }

void RDPCommandDecoder::DecodeSetBlendColor(const uint32_t* words, RDPColor& color) { color.FromCommand(words[1]); }

void RDPCommandDecoder::DecodeSetPrimColor(const uint32_t* words, RDPState& state) {
    uint32_t w0 = words[0];
    state.primLODMin = static_cast<float>((w0 >> 8) & 0x1F) / 32.0f; // 5-bit minLevel [12:8]
    state.primLODFrac = static_cast<float>(w0 & 0xFF) / 255.0f;
    state.primColor.FromCommand(words[1]);
}

void RDPCommandDecoder::DecodeSetEnvColor(const uint32_t* words, RDPColor& color) { color.FromCommand(words[1]); }

void RDPCommandDecoder::DecodeSetPrimDepth(const uint32_t* words, uint16_t& depth, uint16_t& deltaZ) {
    depth = static_cast<uint16_t>((words[1] >> 16) & 0xFFFF);
    deltaZ = static_cast<uint16_t>(words[1] & 0xFFFF);
}

void RDPCommandDecoder::DecodeSetConvert(const uint32_t* words, int16_t convertK[6]) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    // K0-K3: 9-bit signed, reconstructed as (SIGN(k,9) * 2 + 1) per N64 hardware
    auto signExt9 = [](uint32_t v) -> int16_t {
        return static_cast<int16_t>(static_cast<int16_t>((v & 0x1FF) << 7) >> 7);
    };
    convertK[0] = static_cast<int16_t>(signExt9((w0 >> 13) & 0x1FF) * 2 + 1);
    convertK[1] = static_cast<int16_t>(signExt9((w0 >> 4) & 0x1FF) * 2 + 1);
    convertK[2] = static_cast<int16_t>(signExt9(((w0 & 0x0F) << 5) | ((w1 >> 27) & 0x1F)) * 2 + 1);
    convertK[3] = static_cast<int16_t>(signExt9((w1 >> 18) & 0x1FF) * 2 + 1);
    // K4-K5: 9-bit unsigned (no sign extension, no *2+1)
    convertK[4] = static_cast<int16_t>((w1 >> 9) & 0x1FF);
    convertK[5] = static_cast<int16_t>(w1 & 0x1FF);
}

void RDPCommandDecoder::DecodeSetKeyGB(const uint32_t* words, RDPState& state) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    state.chromaKey.widthG = (w0 >> 12) & 0xFFF;
    state.chromaKey.widthB = w0 & 0xFFF;
    state.chromaKey.centerG = (w1 >> 24) & 0xFF;
    state.chromaKey.scaleG = (w1 >> 16) & 0xFF;
    state.chromaKey.centerB = (w1 >> 8) & 0xFF;
    state.chromaKey.scaleB = w1 & 0xFF;
}

void RDPCommandDecoder::DecodeSetKeyR(const uint32_t* words, RDPState& state) {
    uint32_t w1 = words[1];
    state.chromaKey.widthR = (w1 >> 16) & 0xFFF;
    state.chromaKey.centerR = (w1 >> 8) & 0xFF;
    state.chromaKey.scaleR = w1 & 0xFF;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Load Decoders
// ═══════════════════════════════════════════════════════════════════════════

void RDPCommandDecoder::DecodeLoadBlock(const uint32_t* words, int& tile, int& uls, int& ult, int& texels, int& dxt) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    uls = (w0 >> 12) & 0xFFF;
    ult = w0 & 0xFFF;
    tile = (w1 >> 24) & 0x07;
    texels = (w1 >> 12) & 0xFFF;
    dxt = w1 & 0xFFF;
}

void RDPCommandDecoder::DecodeLoadTile(const uint32_t* words, int& tile, float& sl, float& tl, float& sh, float& th) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    sl = static_cast<float>((w0 >> 12) & 0xFFF) / 4.0f;
    tl = static_cast<float>(w0 & 0xFFF) / 4.0f;
    tile = (w1 >> 24) & 0x07;
    sh = static_cast<float>((w1 >> 12) & 0xFFF) / 4.0f;
    th = static_cast<float>(w1 & 0xFFF) / 4.0f;
}

void RDPCommandDecoder::DecodeLoadTLUT(const uint32_t* words, int& tile, int& count) {
    uint32_t w1 = words[1];
    tile = (w1 >> 24) & 0x07;
    count = ((w1 >> 14) & 0x3FF) + 1;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Triangle Decoding
// ═══════════════════════════════════════════════════════════════════════════

void RDPCommandDecoder::DecodeTriangle(const uint32_t* words, int wordCount, DecodedTriangle& tri) {
    uint8_t cmdByte = (words[0] >> 24) & 0x3F;
    tri.hasShade = (cmdByte & 0x04) != 0;
    tri.hasTexture = (cmdByte & 0x02) != 0;
    tri.hasDepth = (cmdByte & 0x01) != 0;
    tri.tileIndex = (words[0] >> 16) & 0x07;

    // Edge coefficients (words 0-7)
    tri.edge.leftMajor = ((words[0] >> 23) & 1) != 0;

    // YL, YM, YH (12.2 signed)
    int32_t ylRaw = static_cast<int32_t>(words[0] & 0x3FFF);
    if (ylRaw & 0x2000) ylRaw |= ~0x3FFF;
    tri.edge.yl = static_cast<double>(ylRaw) / 4.0;

    int32_t ymRaw = static_cast<int32_t>((words[1] >> 16) & 0x3FFF);
    if (ymRaw & 0x2000) ymRaw |= ~0x3FFF;
    tri.edge.ym = static_cast<double>(ymRaw) / 4.0;

    int32_t yhRaw = static_cast<int32_t>(words[1] & 0x3FFF);
    if (yhRaw & 0x2000) yhRaw |= ~0x3FFF;
    tri.edge.yh = static_cast<double>(yhRaw) / 4.0;

    // XL (s15.16)
    tri.edge.xl = static_cast<double>(static_cast<int32_t>(words[2])) / 65536.0;
    tri.edge.dxlDy = static_cast<double>(static_cast<int32_t>(words[3])) / 65536.0;

    // XH (s15.16)
    tri.edge.xh = static_cast<double>(static_cast<int32_t>(words[4])) / 65536.0;
    tri.edge.dxhDy = static_cast<double>(static_cast<int32_t>(words[5])) / 65536.0;

    // XM (s15.16)
    tri.edge.xm = static_cast<double>(static_cast<int32_t>(words[6])) / 65536.0;
    tri.edge.dxmDy = static_cast<double>(static_cast<int32_t>(words[7])) / 65536.0;

    int offset = 8; // Current word index

    // Shade coefficients (if present) — 16 words, interleaved int/frac halves
    // N64 RDP layout: RGBA components are packed as (R|G, B|A) pairs.
    // Words 0-3:   int halves of (val_RG, val_BA, dDx_RG, dDx_BA)
    // Words 4-7:   frac halves  of (val_RG, val_BA, dDx_RG, dDx_BA)
    // Words 8-11:  int halves of (dDe_RG, dDe_BA, dDy_RG, dDy_BA)
    // Words 12-15: frac halves  of (dDe_RG, dDe_BA, dDy_RG, dDy_BA)
    if (tri.hasShade && offset + 16 <= wordCount) {
        const uint32_t* s = &words[offset];

        auto combine = [](uint32_t intWord, uint32_t fracWord, bool hiHalf) -> float {
            int16_t iPart = hiHalf ? static_cast<int16_t>(intWord >> 16) : static_cast<int16_t>(intWord & 0xFFFF);
            uint16_t fPart = hiHalf ? static_cast<uint16_t>(fracWord >> 16) : static_cast<uint16_t>(fracWord & 0xFFFF);
            int32_t val = (static_cast<int32_t>(iPart) << 16) | fPart;
            return static_cast<float>(val) / 65536.0f;
        };

        // Value (initial color at start pixel)
        tri.shade.r = combine(s[0], s[4], true);
        tri.shade.g = combine(s[0], s[4], false);
        tri.shade.b = combine(s[1], s[5], true);
        tri.shade.a = combine(s[1], s[5], false);

        // dDx (X derivative)
        tri.shade.drDx = combine(s[2], s[6], true);
        tri.shade.dgDx = combine(s[2], s[6], false);
        tri.shade.dbDx = combine(s[3], s[7], true);
        tri.shade.daDx = combine(s[3], s[7], false);

        // dDe (edge derivative)
        tri.shade.drDe = combine(s[8], s[12], true);
        tri.shade.dgDe = combine(s[8], s[12], false);
        tri.shade.dbDe = combine(s[9], s[13], true);
        tri.shade.daDe = combine(s[9], s[13], false);

        // dDy (Y derivative / per-scanline correction)
        tri.shade.drDy = combine(s[10], s[14], true);
        tri.shade.dgDy = combine(s[10], s[14], false);
        tri.shade.dbDy = combine(s[11], s[15], true);
        tri.shade.daDy = combine(s[11], s[15], false);

        offset += 16;
    } else {
        std::memset(&tri.shade, 0, sizeof(tri.shade));
        if (tri.hasShade) offset += 16;
    }

    // Texture coefficients (if present) — 16 words, same interleaved layout
    // S,T paired (like R,G); W in hi16 of second word (like B). lo16 unused.
    if (tri.hasTexture && offset + 16 <= wordCount) {
        const uint32_t* t = &words[offset];

        auto combine = [](uint32_t intWord, uint32_t fracWord, bool hiHalf) -> float {
            int16_t iPart = hiHalf ? static_cast<int16_t>(intWord >> 16) : static_cast<int16_t>(intWord & 0xFFFF);
            uint16_t fPart = hiHalf ? static_cast<uint16_t>(fracWord >> 16) : static_cast<uint16_t>(fracWord & 0xFFFF);
            int32_t val = (static_cast<int32_t>(iPart) << 16) | fPart;
            return static_cast<float>(val) / 65536.0f;
        };

        // Value (initial texcoord)
        tri.texture.s = combine(t[0], t[4], true);
        tri.texture.t = combine(t[0], t[4], false);
        tri.texture.w = combine(t[1], t[5], true);

        // dDx
        tri.texture.dsDx = combine(t[2], t[6], true);
        tri.texture.dtDx = combine(t[2], t[6], false);
        tri.texture.dwDx = combine(t[3], t[7], true);

        // dDe
        tri.texture.dsDe = combine(t[8], t[12], true);
        tri.texture.dtDe = combine(t[8], t[12], false);
        tri.texture.dwDe = combine(t[9], t[13], true);

        // dDy
        tri.texture.dsDy = combine(t[10], t[14], true);
        tri.texture.dtDy = combine(t[10], t[14], false);
        tri.texture.dwDy = combine(t[11], t[15], true);

        offset += 16;
    } else {
        std::memset(&tri.texture, 0, sizeof(tri.texture));
        if (tri.hasTexture) offset += 16;
    }

    // Depth coefficients (if present)
    if (tri.hasDepth && offset + 4 <= wordCount) {
        int32_t intPart = static_cast<int32_t>(words[offset]) >> 16;
        uint16_t fracPart = words[offset] & 0xFFFF;
        tri.depth.z = static_cast<float>(intPart) + static_cast<float>(fracPart) / 65536.0f;

        intPart = static_cast<int32_t>(words[offset + 1]) >> 16;
        fracPart = words[offset + 1] & 0xFFFF;
        tri.depth.dzDx = static_cast<float>(intPart) + static_cast<float>(fracPart) / 65536.0f;

        intPart = static_cast<int32_t>(words[offset + 2]) >> 16;
        fracPart = words[offset + 2] & 0xFFFF;
        tri.depth.dzDe = static_cast<float>(intPart) + static_cast<float>(fracPart) / 65536.0f;

        intPart = static_cast<int32_t>(words[offset + 3]) >> 16;
        fracPart = words[offset + 3] & 0xFFFF;
        tri.depth.dzDy = static_cast<float>(intPart) + static_cast<float>(fracPart) / 65536.0f;
    } else {
        std::memset(&tri.depth, 0, sizeof(tri.depth));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Rectangle Decoding
// ═══════════════════════════════════════════════════════════════════════════

void RDPCommandDecoder::DecodeTextureRect(const uint32_t* words, DecodedRectangle& rect) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];
    uint32_t w2 = words[2];
    uint32_t w3 = words[3];

    rect.x1 = static_cast<float>((w0 >> 12) & 0xFFF) / 4.0f;
    rect.y1 = static_cast<float>(w0 & 0xFFF) / 4.0f;
    rect.tileIndex = (w1 >> 24) & 0x07;
    rect.x0 = static_cast<float>((w1 >> 12) & 0xFFF) / 4.0f;
    rect.y0 = static_cast<float>(w1 & 0xFFF) / 4.0f;

    rect.s = static_cast<float>(static_cast<int16_t>((w2 >> 16) & 0xFFFF)) / 32.0f;
    rect.t = static_cast<float>(static_cast<int16_t>(w2 & 0xFFFF)) / 32.0f;
    rect.dsDx = static_cast<float>(static_cast<int16_t>((w3 >> 16) & 0xFFFF)) / 1024.0f;
    rect.dtDy = static_cast<float>(static_cast<int16_t>(w3 & 0xFFFF)) / 1024.0f;
    rect.flipped = false;
}

void RDPCommandDecoder::DecodeTextureRectFlip(const uint32_t* words, DecodedRectangle& rect) {
    DecodeTextureRect(words, rect);
    rect.flipped = true;
}

void RDPCommandDecoder::DecodeFillRect(const uint32_t* words, float& x0, float& y0, float& x1, float& y1) {
    uint32_t w0 = words[0];
    uint32_t w1 = words[1];

    x1 = static_cast<float>((w0 >> 12) & 0xFFF) / 4.0f;
    y1 = static_cast<float>(w0 & 0xFFF) / 4.0f;
    x0 = static_cast<float>((w1 >> 12) & 0xFFF) / 4.0f;
    y0 = static_cast<float>(w1 & 0xFFF) / 4.0f;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Command Description
// ═══════════════════════════════════════════════════════════════════════════

const char* RDPCommandDecoder::GetCommandName(RDPCommandID cmd) {
    switch (cmd) {
    case RDPCommandID::Nop:              return "Nop";
    case RDPCommandID::TriNoShade:       return "Tri_NoShade";
    case RDPCommandID::TriNoShadeZ:      return "Tri_NoShade_Z";
    case RDPCommandID::TriTexNoShade:    return "Tri_Tex_NoShade";
    case RDPCommandID::TriTexNoShadeZ:   return "Tri_Tex_NoShade_Z";
    case RDPCommandID::TriShade:         return "Tri_Shade";
    case RDPCommandID::TriShadeZ:        return "Tri_Shade_Z";
    case RDPCommandID::TriTexShade:      return "Tri_Tex_Shade";
    case RDPCommandID::TriTexShadeZ:     return "Tri_Tex_Shade_Z";
    case RDPCommandID::TextureRectangle: return "TextureRect";
    case RDPCommandID::TextureRectFlip:  return "TextureRectFlip";
    case RDPCommandID::SyncLoad:         return "SyncLoad";
    case RDPCommandID::SyncPipe:         return "SyncPipe";
    case RDPCommandID::SyncTile:         return "SyncTile";
    case RDPCommandID::SyncFull:         return "SyncFull";
    case RDPCommandID::SetKeyGB:         return "SetKeyGB";
    case RDPCommandID::SetKeyR:          return "SetKeyR";
    case RDPCommandID::SetConvert:       return "SetConvert";
    case RDPCommandID::SetScissor:       return "SetScissor";
    case RDPCommandID::SetPrimDepth:     return "SetPrimDepth";
    case RDPCommandID::SetOtherModes:    return "SetOtherModes";
    case RDPCommandID::LoadTLUT:         return "LoadTLUT";
    case RDPCommandID::SetTileSize:      return "SetTileSize";
    case RDPCommandID::LoadBlock:        return "LoadBlock";
    case RDPCommandID::LoadTile:         return "LoadTile";
    case RDPCommandID::SetTile:          return "SetTile";
    case RDPCommandID::FillRect:         return "FillRect";
    case RDPCommandID::SetFillColor:     return "SetFillColor";
    case RDPCommandID::SetFogColor:      return "SetFogColor";
    case RDPCommandID::SetBlendColor:    return "SetBlendColor";
    case RDPCommandID::SetPrimColor:     return "SetPrimColor";
    case RDPCommandID::SetEnvColor:      return "SetEnvColor";
    case RDPCommandID::SetCombine:       return "SetCombine";
    case RDPCommandID::SetTextureImage:  return "SetTextureImage";
    case RDPCommandID::SetMaskImage:     return "SetMaskImage";
    case RDPCommandID::SetColorImage:    return "SetColorImage";
    default:                             return "Unknown";
    }
}

std::string RDPCommandDecoder::DescribeCommand(RDPCommandID cmd) { return GetCommandName(cmd); }

// ═══════════════════════════════════════════════════════════════════════════
//  RDP Command Buffer
// ═══════════════════════════════════════════════════════════════════════════

RDPCommandBuffer::RDPCommandBuffer() :
    m_readOffset(0), m_writeOffset(0), m_capacity(MAX_BUFFER_SIZE / sizeof(uint32_t)) {
    m_buffer = new uint32_t[m_capacity];
}

RDPCommandBuffer::~RDPCommandBuffer() { delete[] m_buffer; }

void RDPCommandBuffer::Reset() {
    m_readOffset = 0;
    m_writeOffset = 0;
}

void RDPCommandBuffer::Push(const uint32_t* words, int count) {
    if (m_writeOffset + count > m_capacity) {
        Compact();
        if (m_writeOffset + count > m_capacity) {
            // Buffer overflow - discard old data
            m_readOffset = 0;
            m_writeOffset = 0;
        }
    }

    std::memcpy(m_buffer + m_writeOffset, words, count * sizeof(uint32_t));
    m_writeOffset += count;
}

const uint32_t* RDPCommandBuffer::Peek() const {
    if (m_readOffset >= m_writeOffset) return nullptr;
    return m_buffer + m_readOffset;
}

void RDPCommandBuffer::Advance(int wordCount) {
    m_readOffset += wordCount;
    if (m_readOffset > m_writeOffset) m_readOffset = m_writeOffset;
}

void RDPCommandBuffer::Compact() {
    if (m_readOffset == 0) return;

    size_t remaining = m_writeOffset - m_readOffset;
    if (remaining > 0) {
        std::memmove(m_buffer, m_buffer + m_readOffset, remaining * sizeof(uint32_t));
    }
    m_writeOffset = remaining;
    m_readOffset = 0;
}

} // namespace Nova64::Video
