/**
 * @file RDPProcessor_Texture.cpp
 * @brief RDPProcessor texture load commands (LoadBlock, LoadTile, LoadTLUT, ScrollTex) and the sync commands.
 */

#include "RDPProcessor_Private.hpp"
void RDPProcessor::HandleLoadBlock(const uint32_t* words) {
    // Flush pending draws before overwriting TMEM: batched draws need the TMEM content that was
    // present when they were queued.
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile, uls, ult, texels, dxt;
    m_decoder.DecodeLoadBlock(words, tile, uls, ult, texels, dxt);
    // GLideN64: gDPSetTileSize(tile, uls, ult, lrs, dxt) inside gDPLoadBlock
    auto& tileRef = m_state.rdp.tiles[tile];
    tileRef.sl = uls;
    tileRef.tl = ult;
    tileRef.sh = texels;
    tileRef.th = dxt;

    MM_RDP_TRACE("LoadBlock tile={} tex=0x{:06X} texFmt={} texSize={} texWidth={} uls={} ult={} texels={} dxt={} "
                 "tileFmt={} tileSize={} tmem={} line={} pal={}",
                 tile, m_state.rdp.textureImage.address & 0x7FFFFF, static_cast<int>(m_state.rdp.textureImage.format),
                 static_cast<int>(m_state.rdp.textureImage.size), m_state.rdp.textureImage.width, uls, ult, texels, dxt,
                 tileRef.format, tileRef.size, tileRef.tmemAddr, tileRef.line, tileRef.palette);

    // Track which RDRAM source fed this tile (for correct 2-cycle texture keys)
    m_state.rdp.tileLoadAddress[tile & 7] = m_state.rdp.textureImage.address;
    // Also index by TMEM slot: the render tile (tile 0) and loading tile (tile 7)
    // share the same tmemAddr, so this bridges them in PopulateTextureInfo.
    m_state.rdp.tmemSourceAddress[m_state.rdp.tiles[tile & 7].tmemAddr & 0x1FF] = m_state.rdp.textureImage.address;
    // Clear any stale LOADTYPE_TILE record for this slot: a block load must not
    // inherit a previous tile load's dimensions.
    m_state.rdp.tmemLoadType[m_state.rdp.tiles[tile & 7].tmemAddr & 0x1FF] = RDPState::LOADTYPE_BLOCK;
    m_state.rdp.tmemLoadDxt[m_state.rdp.tiles[tile & 7].tmemAddr & 0x1FF] = static_cast<uint32_t>(dxt);
    m_state.rdp.tmemLoadBlockWidth[m_state.rdp.tiles[tile & 7].tmemAddr & 0x1FF] = static_cast<uint16_t>(texels);
    ExecuteLoadBlock(tile, uls, ult, texels, dxt);
}

void RDPProcessor::HandleLoadTile(const uint32_t* words) {
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile;
    float sl, tl, sh, th;
    m_decoder.DecodeLoadTile(words, tile, sl, tl, sh, th);
    // GLideN64: gDPSetTileSize(tile, uls, ult, lrs, lrt) inside gDPLoadTile
    auto& tileRef = m_state.rdp.tiles[tile];
    tileRef.sl = static_cast<int32_t>(sl * 4.0f);
    tileRef.tl = static_cast<int32_t>(tl * 4.0f);
    tileRef.sh = static_cast<int32_t>(sh * 4.0f);
    tileRef.th = static_cast<int32_t>(th * 4.0f);

    MM_RDP_TRACE("LoadTile tile={} tex=0x{:06X} texFmt={} texSize={} texWidth={} sl={:.2f} tl={:.2f} sh={:.2f} "
                 "th={:.2f} tileFmt={} tileSize={} tmem={} line={} pal={}",
                 tile, m_state.rdp.textureImage.address & 0x7FFFFF, static_cast<int>(m_state.rdp.textureImage.format),
                 static_cast<int>(m_state.rdp.textureImage.size), m_state.rdp.textureImage.width, sl, tl, sh, th,
                 tileRef.format, tileRef.size, tileRef.tmemAddr, tileRef.line, tileRef.palette);

    // Track which RDRAM source fed this tile (for correct 2-cycle texture keys)
    m_state.rdp.tileLoadAddress[tile & 7] = m_state.rdp.textureImage.address;
    // Also index by TMEM slot: connects loading tile to render tile via shared tmemAddr.
    m_state.rdp.tmemSourceAddress[m_state.rdp.tiles[tile & 7].tmemAddr & 0x1FF] = m_state.rdp.textureImage.address;
    // Clear any stale LOADTYPE_TILE record for this slot; ExecuteLoadTile sets it again once the
    // load is accepted.
    m_state.rdp.tmemLoadType[m_state.rdp.tiles[tile & 7].tmemAddr & 0x1FF] = RDPState::LOADTYPE_BLOCK;
    ExecuteLoadTile(tile, sl, tl, sh, th);
}

void RDPProcessor::HandleLoadTLUT(const uint32_t* words) {
    if (!m_batch.drawCalls.empty()) FlushBatch();
    int tile, count;
    m_decoder.DecodeLoadTLUT(words, tile, count);
    // Real hardware / GLideN64 gDPLoadTLUT: G_LOADTLUT's w0 carries no uls/ult fields (w0 is the
    // bare opcode), and the command implicitly performs gDPSetTileSize(tile, 0, 0, count-1, 0),
    // overwriting this tile's sl/tl/sh/th registers. That side effect is mirrored here; otherwise
    // ExecuteLoadTLUT's address math falls through to whatever sl/tl an earlier
    // SetTileSize/LoadTile/LoadBlock left on this tile index (typically tile 7, reused across
    // loads), producing a garbage source address and palette noise.
    auto& tileRef = m_state.rdp.tiles[tile];
    tileRef.sl = 0;
    tileRef.tl = 0;
    tileRef.sh = static_cast<int32_t>((count - 1) << 2);
    tileRef.th = 0;
    MM_RDP_TRACE("LoadTLUT tile={} count={} tex=0x{:06X} texFmt={} texSize={} texWidth={} tmem={} sl={} tl={} pal={}",
                 tile, count, m_state.rdp.textureImage.address & 0x7FFFFF,
                 static_cast<int>(m_state.rdp.textureImage.format), static_cast<int>(m_state.rdp.textureImage.size),
                 m_state.rdp.textureImage.width, tileRef.tmemAddr, tileRef.sl, tileRef.tl, tileRef.palette);
    ExecuteLoadTLUT(tile, count);
}

void RDPProcessor::HandleScrollTex(const uint32_t* words) {
    int tile = (words[1] >> 8) & 0x7;
    int s = static_cast<int16_t>(words[1] & 0xFFFF);
    int t = static_cast<int16_t>(words[0] >> 16);

    // Convert 10.2 fixed point to texel units
    m_state.rdp.scrollS[tile] = static_cast<float>(s) / 1024.0f;
    m_state.rdp.scrollT[tile] = static_cast<float>(t) / 1024.0f;
}

// --- GLideN64-matching TMEM copy helpers ---

// Byte-swap copy from big-endian N64 RDRAM into little-endian TMEM byte array.
// Mirrors GLideN64's UnswapCopyWrap: reverses bytes within each 32-bit word
// while copying, wrapping the destination at destMask (0xFFF for TMEM).
static void UnswapCopyWrap(const uint8_t* src, uint32_t srcIdx, uint8_t* dest, uint32_t destIdx, uint32_t destMask,
                           uint32_t numBytes) {
    // Copy leading bytes to reach 4-byte alignment in source
    uint32_t leadingBytes = srcIdx & 3;
    if (leadingBytes != 0) {
        leadingBytes = 4 - leadingBytes;
        if (leadingBytes > numBytes) leadingBytes = numBytes;
        numBytes -= leadingBytes;
        srcIdx ^= 3;
        for (uint32_t i = 0; i < leadingBytes; i++) {
            dest[destIdx & destMask] = src[srcIdx];
            ++destIdx;
            --srcIdx;
        }
        srcIdx += 5;
    }
    // Copy aligned 32-bit words (byte-swap within each word)
    uint32_t numDWords = numBytes >> 2;
    while (numDWords--) {
        dest[(destIdx + 3) & destMask] = src[srcIdx++];
        dest[(destIdx + 2) & destMask] = src[srcIdx++];
        dest[(destIdx + 1) & destMask] = src[srcIdx++];
        dest[(destIdx + 0) & destMask] = src[srcIdx++];
        destIdx += 4;
    }
    // Copy trailing bytes
    uint32_t trailingBytes = numBytes & 3;
    if (trailingBytes) {
        srcIdx ^= 3;
        for (uint32_t i = 0; i < trailingBytes; i++) {
            dest[destIdx & destMask] = src[srcIdx];
            ++destIdx;
            --srcIdx;
        }
    }
}

// Swap 32-bit halves of each 64-bit word on odd TMEM rows.
// Mirrors GLideN64's DWordInterleaveWrap: works on uint32_t view of TMEM.
// srcIdx and srcMask are in uint32_t units (not bytes).
static void DWordInterleaveWrap(uint32_t* src, uint32_t srcIdx, uint32_t srcMask, uint32_t numQWords) {
    while (numQWords--) {
        uint32_t idx0 = srcIdx++ & srcMask;
        uint32_t idx1 = srcIdx++ & srcMask;
        uint32_t p0 = src[idx0];
        src[idx0] = src[idx1];
        src[idx1] = p0;
    }
}

void RDPProcessor::ExecuteLoadBlock(int tile, int uls, int ult, int texels, int dxt) {
    if (!m_rdram) return;
    FlushBatch();

    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];

    // Byte count from tile size (matches GLideN64)
    uint32_t width = (texels - uls + 1) & 0x0FFF;
    uint32_t bytes = width << tileDesc.size >> 1;
    if ((bytes & 7) != 0) bytes = (bytes & ~7u) + 8;

    uint32_t address = texImg.address + ult * texImg.width * GetBitsPerPixel(texImg.size) / 8 +
                       (uls << static_cast<int>(texImg.size) >> 1);
    address &= (m_rdramSize - 1);

    if (bytes == 0 || (address + bytes) > m_rdramSize) return;

    GPUFramebuffer* rawSrcFB = g_fbManager.FindByAddress(address);
    bool canAliasSrcFB = CanAliasFramebuffer(rawSrcFB, address, IsRGBAFramebufferFormat(texImg.format, texImg.size));
    GPUFramebuffer* srcFB = canAliasSrcFB ? rawSrcFB : nullptr;
    MM_RDP_TRACE("ExecuteLoadBlock tile={} src=0x{:06X} bytes={} tmem={} texFmt={} texSize={} fbFound={} fbAlias={} "
                 "fbAddr=0x{:06X} gpuModified={}",
                 tile, address & 0x7FFFFF, bytes, tileDesc.tmemAddr, static_cast<int>(texImg.format),
                 static_cast<int>(texImg.size), rawSrcFB != nullptr, canAliasSrcFB,
                 rawSrcFB ? rawSrcFB->desc.address : 0u, rawSrcFB ? rawSrcFB->gpuModified : false);
    if (srcFB && srcFB->gpuModified) {
        MM_RDP_TRACE("CopyFromGPU LoadBlock src=0x{:06X} fbAddr=0x{:06X} fbSize={}x{} stride={}", address & 0x7FFFFF,
                     srcFB->desc.address, srcFB->desc.width, srcFB->desc.height, srcFB->desc.stride);
        g_fbManager.CopyFromGPU(srcFB, m_rdram);

        // Vulkan: CopyFromGPU is a no-op (no per-N64-FB GL textures).
        // Instead, snapshot the current swapchain image so the subsequent
        // FB-as-texture draw (inventory background, lens of truth, etc.) can
        // sample it directly via m_renderer->BindTexture().
        if (!g_fbManager.IsGL() && m_renderer) {
            const uint32_t snapW = m_renderer->GetWidth();
            const uint32_t snapH = m_renderer->GetHeight();
            if (snapW > 0 && snapH > 0) {
                // Ping-pong: flip the slot first so the previously bound snapshot is not overwritten
                // while the in-flight frame is still sampling it.
                m_vkFBSnapshotIdx = (m_vkFBSnapshotIdx + 1) % kVkFBSnapshotSlots;
                auto& slot = m_vkFBSnapshots[m_vkFBSnapshotIdx];
                bool needRecreate = !slot || slot->GetWidth() != snapW || slot->GetHeight() != snapH;
                if (needRecreate) {
                    TextureDesc snapDesc{};
                    snapDesc.width = snapW;
                    snapDesc.height = snapH;
                    snapDesc.format = TextureFormat::RGBA8;
                    snapDesc.usage = BufferUsage::Dynamic;
                    snapDesc.mipLevels = 1;
                    slot = m_renderer->CreateTexture(snapDesc);
                }
                if (slot) {
                    // GPU-side copy: records vkCmdBlitImage into the in-flight cmd buffer.
                    m_renderer->BlitToTexture(slot.get());
                    m_vkFBSnapshotAddrs[m_vkFBSnapshotIdx] = srcFB->desc.address & 0x7FFFFF;
                    // Mark as clean so subsequent LoadBlocks in this and future frames
                    // don't retrigger BlitToTexture until the FB is rendered to again.
                    srcFB->gpuModified = false;
                }
            }
        }
    }

    uint32_t tmemWordAddr = tileDesc.tmemAddr; // in 64-bit word units

    // RGBA32: split-bank storage (matching GLideN64's gDPLoadBlock32)
    if (tileDesc.size == static_cast<uint8_t>(ImageSize::Bpp32)) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(m_rdram);
        uint16_t* tmem16 = reinterpret_cast<uint16_t*>(m_state.rdp.tmem.data);
        const uint32_t tb = tileDesc.tmemAddr << 2; // in u16 units
        const uint32_t line32 = tileDesc.line << 2; // in u16 units
        uint32_t srcAddr = m_state.rdp.textureImage.address >> 2;
        uint32_t w32 = (static_cast<uint32_t>(texels) - uls + 1) << 2;
        if (w32 == 4) w32 = 1;
        else if (w32 & 7) w32 = (w32 & ~7u) + 8;
        uint32_t maxIdx = m_rdramSize >> 2;

        // The GLideN64-derived w32 formula (<<2 scaling) inflates the loop
        // bound to 4x the true dword count for a plain, unpadded 32-bit block
        // (e.g. lrs=1023, dxt=128, line=0 for a standalone 32x32 icon, the
        // pattern the SDK's gDPLoadTextureBlock macro emits). The dxt/t addressing
        // wraps TMEM writes at 1024 entries (&0x3ff), so the full inflated range would rewrite every
        // entry 4 times, and the last pass, reading 12-16KB past the true image, is the one that
        // survives: noise from whatever unrelated data follows the icon in RDRAM. The true source
        // dword count never depends on the <<2 scaling (padding only changes where in TMEM data lands,
        // not how many real texels are read), so reads are bounded to it directly.
        uint32_t trueDwords = ((static_cast<uint32_t>(texels) - uls + 1) & 0xFFFu);
        uint32_t readLimit = std::min(w32, trueDwords);

        if (dxt != 0) {
            uint32_t j = 0, t = 0, oldt = 0;
            for (uint32_t i = 0; i < w32; i += 2) {
                oldt = t;
                t = ((j >> 11) & 1) ? 3 : 1;
                if (t != oldt) i += line32;
                if (i >= w32) break;
                if (i >= readLimit) break;
                if (srcAddr + i + 1 < maxIdx) {
                    uint32_t ptr = ((tb + i) ^ t) & 0x3ff;
                    uint32_t c = src[srcAddr + i];
                    tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                    tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
                    ptr = ((tb + i + 1) ^ t) & 0x3ff;
                    c = src[srcAddr + i + 1];
                    tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                    tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
                }
                j += dxt;
            }
        } else {
            for (uint32_t i = 0; i < readLimit && (srcAddr + i) < maxIdx; i++) {
                uint32_t ptr = ((tb + i) ^ 1) & 0x3ff;
                uint32_t c = src[srcAddr + i];
                tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
            }
        }
    } else {
        // Non-RGBA32: UnswapCopy + DXT interleave (matching GLideN64)
        UnswapCopyWrap(m_rdram, address, m_state.rdp.tmem.data, tmemWordAddr << 3, 0xFFF, bytes);
        if (dxt != 0) {
            uint32_t dxtCounter = 0;
            uint32_t qwords = (bytes >> 3);
            uint32_t line = 0;
            uint32_t tmAddr = tmemWordAddr;
            while (true) {
                do {
                    ++tmAddr;
                    --qwords;
                    if (qwords == 0) goto end_dxt;
                    dxtCounter += dxt;
                } while ((dxtCounter & 0x800) == 0);
                do {
                    ++line;
                    --qwords;
                    if (qwords == 0) goto end_dxt;
                    dxtCounter += dxt;
                } while ((dxtCounter & 0x800) != 0);
                DWordInterleaveWrap(reinterpret_cast<uint32_t*>(m_state.rdp.tmem.data), tmAddr << 1, 0x3FF, line);
                tmAddr += line;
                line = 0;
            }
        end_dxt:
            DWordInterleaveWrap(reinterpret_cast<uint32_t*>(m_state.rdp.tmem.data), tmAddr << 1, 0x3FF, line);
        }
    }

    // Compute CRC for texture cache
    uint32_t tmemByteAddr = tmemWordAddr << 3;
    int crcLen = std::min(static_cast<int>(bytes), static_cast<int>(TMEM_SIZE - tmemByteAddr));
    if (crcLen > 0) {
        uint32_t crc = CRC32_Fast(m_state.rdp.tmem.data + tmemByteAddr, crcLen);
        m_state.rdp.tmem.RecordLoad(address, tileDesc.tmemAddr, static_cast<uint16_t>(crcLen), crc);
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

void RDPProcessor::ExecuteLoadTile(int tile, float sl, float tl, float sh, float th) {
    if (!m_rdram) return;
    FlushBatch();

    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];

    int iuls = static_cast<int>(sl) & 0x3FF;
    int iult = static_cast<int>(tl) & 0x3FF;
    int ilrs = static_cast<int>(sh) & 0x3FF;
    int ilrt = static_cast<int>(th) & 0x3FF;
    int width = (ilrs - iuls + 1) & 0x3FF;
    int height = (ilrt - iult + 1) & 0x3FF;

    if (width <= 0 || height <= 0) return;

    // GLideN64: return early when tile line=0 (degenerate config)
    if (tileDesc.line == 0) return;

    int srcBpp = GetBitsPerPixel(texImg.size);
    int srcLineBytes = texImg.width * srcBpp / 8;
    int dstLineBytes = tileDesc.line * 8; // TMEM line in bytes

    // Width-mask alignment (matches GLideN64)
    int wmask = 0;
    switch (texImg.size) {
    case ImageSize::Bpp8:  wmask = 7; break;
    case ImageSize::Bpp16: wmask = 3; break;
    case ImageSize::Bpp32: wmask = 1; break;
    default:               break;
    }
    int alignedWidth = width;
    if (wmask != 0 && (width & wmask) != 0) alignedWidth = (width & ~wmask) + wmask + 1;

    int tileBpp = GetBitsPerPixel(static_cast<ImageSize>(tileDesc.size));
    int rowBytes = alignedWidth * tileBpp / 8;
    if (dstLineBytes == 0) dstLineBytes = rowBytes;

    uint32_t address = texImg.address + static_cast<uint32_t>(iult) * srcLineBytes +
                       (static_cast<uint32_t>(iuls) << static_cast<int>(texImg.size) >> 1);
    address &= (m_rdramSize - 1);

    // Override the base texImg.address that HandleLoadTile stored in tmemSourceAddress.
    // FlushBatch builds key0.address from tmemSourceAddress, which it uses to find the
    // source FB via FindByAddress.  For tiles with tl > 0, the computed address lands
    // inside the FB's range while the base address (0x000000 for MM startup) does not.
    m_state.rdp.tmemSourceAddress[tileDesc.tmemAddr & 0x1FF] = address;
    // Record load info for GLideN64-parity tile sizing (see RDPState's
    // tmemLoad* comment and CalcTileSize's LOADTYPE_TILE branch).
    {
        const uint32_t slot = tileDesc.tmemAddr & 0x1FF;
        m_state.rdp.tmemLoadType[slot] = RDPState::LOADTYPE_TILE;
        m_state.rdp.tmemLoadWidth[slot] = static_cast<uint16_t>(width);
        m_state.rdp.tmemLoadHeight[slot] = static_cast<uint16_t>(height);
        m_state.rdp.tmemLoadTexWidth[slot] = static_cast<uint16_t>(texImg.width);
        m_state.rdp.tmemLoadSize[slot] = static_cast<uint8_t>(texImg.size);
    }

    GPUFramebuffer* rawSrcFB = g_fbManager.FindByAddress(address);
    if (!g_fbManager.IsGL() && !rawSrcFB) {
        rawSrcFB = g_fbManager.FindContaining(address);
    }
    bool canAliasSrcFB = CanAliasFramebuffer(rawSrcFB, address, IsRGBAFramebufferFormat(texImg.format, texImg.size));
    // Memory reused since the RDP last drew here: the buffer is stale, and the
    // readback below would overwrite the game's new data with its old pixels.
    if (rawSrcFB && !canAliasSrcFB && !g_fbManager.RdramUnchanged(rawSrcFB, m_rdram, m_rdramSize)) rawSrcFB = nullptr;
    GPUFramebuffer* srcFB = canAliasSrcFB ? rawSrcFB : nullptr;
    MM_RDP_TRACE("ExecuteLoadTile tile={} src=0x{:06X} size={}x{} rowBytes={} tmem={} line={} texFmt={} texSize={} "
                 "fbFound={} fbAlias={} fbAddr=0x{:06X} fbType={} fbW={} fbH={} gpuModified={} fbCount={}",
                 tile, address & 0x7FFFFF, width, height, rowBytes, tileDesc.tmemAddr, tileDesc.line,
                 static_cast<int>(texImg.format), static_cast<int>(texImg.size), rawSrcFB != nullptr, canAliasSrcFB,
                 rawSrcFB ? rawSrcFB->desc.address : 0u, rawSrcFB ? static_cast<int>(rawSrcFB->desc.type) : -1,
                 rawSrcFB ? rawSrcFB->desc.width : 0u, rawSrcFB ? rawSrcFB->desc.height : 0u,
                 rawSrcFB ? rawSrcFB->gpuModified : false, g_fbManager.GetFBCount());
    if (srcFB && (srcFB->gpuModified || srcFB->needsRdramResync)) {
        MM_RDP_TRACE("CopyFromGPU LoadTile src=0x{:06X} fbAddr=0x{:06X} fbSize={}x{} stride={}", address & 0x7FFFFF,
                     srcFB->desc.address, srcFB->desc.width, srcFB->desc.height, srcFB->desc.stride);
        g_fbManager.CopyFromGPU(srcFB, m_rdram);
        // CopyFromGPU is a GL-only no-op (FramebufferManager.cpp: `if (...|| !m_isGL) return;`), so
        // needsRdramResync is cleared only when the call could have done something. Clearing it
        // unconditionally would tell the Vulkan sync block below that this address's RDRAM copy was
        // already handled when nothing happened. If gpuModified had also been consumed by an earlier
        // LoadTile/LoadBlock this frame (common: several reads compete for the same buffer's flags per
        // frame), that block's condition (gpuModified || needsRdramResync) would evaluate false and
        // the copy would never run, leaving RDRAM stuck with whatever pixels (often blank or
        // uninitialized) the buffer last synced, however much the GPU went on to render.
        if (g_fbManager.IsGL()) srcFB->needsRdramResync = false;
    }
    if (!g_fbManager.IsGL() && rawSrcFB && (rawSrcFB->gpuModified || rawSrcFB->needsRdramResync) && m_renderer) {
        const auto& d = rawSrcFB->desc;
        m_renderer->CopyFramebufferToRDRAM(d.address, d.width, d.height, d.format == FBFormat::RGBA16, m_rdram);
        g_fbManager.RecordRdramFingerprint(rawSrcFB, m_rdram, m_rdramSize);
        rawSrcFB->gpuModified = false;
        rawSrcFB->needsRdramResync = false;
    }

    uint32_t tmemWordAddr = tileDesc.tmemAddr; // 64-bit word address
    int totalBytes = 0;

    // RGBA32: split-bank storage per pixel (matching GLideN64's gDPLoadTile32b)
    if (tileDesc.size == static_cast<uint8_t>(ImageSize::Bpp32)) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(m_rdram);
        uint16_t* tmem16 = reinterpret_cast<uint16_t*>(m_state.rdp.tmem.data);
        uint32_t line32 = tileDesc.line << 2; // tile line in u16 units
        uint32_t tbase = tileDesc.tmemAddr << 2;
        uint32_t rdAddr32 = texImg.address >> 2;
        uint32_t maxIdx = m_rdramSize >> 2;

        for (int j = 0; j < height; ++j) {
            uint32_t tline = tbase + line32 * static_cast<uint32_t>(j);
            uint32_t s = (static_cast<uint32_t>(j + iult) * texImg.width) + static_cast<uint32_t>(iuls);
            uint32_t xorval = (j & 1) ? 3u : 1u;
            for (int i = 0; i < width; ++i) {
                uint32_t srcIdx = rdAddr32 + s + static_cast<uint32_t>(i);
                if (srcIdx >= maxIdx) continue;
                uint32_t c = src[srcIdx];
                uint32_t ptr = ((tline + static_cast<uint32_t>(i)) ^ xorval) & 0x3ff;
                tmem16[ptr] = static_cast<uint16_t>(c >> 16);
                tmem16[ptr | 0x400] = static_cast<uint16_t>(c & 0xffff);
            }
        }
        totalBytes = width * height * 4;
    } else {
        // Non-RGBA32: UnswapCopyWrap per row + DWordInterleave on odd rows (matches GLideN64
        // gDPLoadTile: qwpr = bpr >> 3, i.e. the row's own aligned byte count, not the tile's full
        // `line` stride)
        uint32_t tmAddr = tmemWordAddr;                  // in 64-bit word units
        uint32_t srcLineBpl = texImg.width * srcBpp / 8; // source bytes per line
        uint32_t curAddr = address;
        uint32_t qwpr = rowBytes >> 3;

        for (int y = 0; y < height && curAddr < m_rdramSize; y++) {
            uint32_t copyLen = rowBytes;
            if (curAddr + copyLen > m_rdramSize) copyLen = m_rdramSize - curAddr;

            UnswapCopyWrap(m_rdram, curAddr, m_state.rdp.tmem.data, tmAddr << 3, 0xFFF, copyLen);
            totalBytes += copyLen;

            if (y & 1) {
                DWordInterleaveWrap(reinterpret_cast<uint32_t*>(m_state.rdp.tmem.data), tmAddr << 1, 0x3FF, qwpr);
            }

            curAddr += srcLineBpl;
            tmAddr += tileDesc.line; // advance by tile line (64-bit word units)
        }
    }

    // CRC of the loaded data, for texture cache lookups
    uint32_t tmemByteAddr = tmemWordAddr << 3;
    if (totalBytes > 0) {
        int crcLen = std::min(totalBytes, static_cast<int>(TMEM_SIZE - tmemByteAddr));
        if (crcLen > 0) {
            uint32_t crc = CRC32_Fast(m_state.rdp.tmem.data + tmemByteAddr, crcLen);
            m_state.rdp.tmem.RecordLoad(address, tileDesc.tmemAddr, static_cast<uint16_t>(crcLen), crc);
        }
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

void RDPProcessor::ExecuteLoadTLUT(int tile, int count) {
    if (!m_rdram) return;
    FlushBatch();

    const auto& texImg = m_state.rdp.textureImage;
    const auto& tileDesc = m_state.rdp.tiles[tile];

    // GLideN64: tmem < 256 is wrong for TLUT (in 64-bit-word units, 256 = byte 2048)
    if (tileDesc.tmemAddr < 256) return;

    uint32_t address = texImg.address + (tileDesc.tl >> 2) * (texImg.width * GetBitsPerPixel(texImg.size) / 8) +
                       ((tileDesc.sl >> 2) << static_cast<int>(texImg.size) >> 1);

    MM_RDP_TRACE("ExecuteLoadTLUT tile={} count={} src=0x{:06X} baseTex=0x{:06X} tmem={} destU16={} texFmt={} "
                 "texSize={} texWidth={}",
                 tile, count, address & 0x7FFFFF, texImg.address & 0x7FFFFF, tileDesc.tmemAddr, tileDesc.tmemAddr << 2,
                 static_cast<int>(texImg.format), static_cast<int>(texImg.size), texImg.width);

    uint16_t* tmem16 = reinterpret_cast<uint16_t*>(m_state.rdp.tmem.data);

    // Byte-swap a 16-bit value (matches GLideN64's swapword)
    auto swapword = [](uint16_t v) -> uint16_t { return static_cast<uint16_t>((v << 8) | (v >> 8)); };

    // TMEM layout: palettes live in the upper 2KB of TMEM (starting at byte 0x800).
    // Each of the 256 palette entries occupies an 8-byte TMEM "word".
    // The N64 hardware replicates the 16-bit color across all 4 banks
    // so they are accessible regardless of the current CI format's bank-swizzling.
    for (int i = 0; i < count; i++) {
        uint32_t rdAddr = address & (m_rdramSize - 1);
        if (rdAddr + 1 < m_rdramSize) {
            uint32_t entryIdx = (tileDesc.tmemAddr + i) & 0x1FF;
            uint16_t val = *reinterpret_cast<const uint16_t*>(m_rdram + (rdAddr ^ 2));
            uint16_t color = swapword(val);
            tmem16[entryIdx * 4 + 0] = color;
            tmem16[entryIdx * 4 + 1] = color;
            tmem16[entryIdx * 4 + 2] = color;
            tmem16[entryIdx * 4 + 3] = color;
        }
        address += 2;
    }

    // CRC from RDRAM source
    uint32_t srcAddr = texImg.address & (m_rdramSize - 1);
    uint32_t crcBytes = count * 2;
    if (srcAddr + crcBytes <= m_rdramSize && crcBytes > 0) {
        uint32_t crc = CRC32_Fast(m_rdram + srcAddr, crcBytes);
        m_state.rdp.tmem.RecordLoad(srcAddr, static_cast<uint16_t>(tileDesc.tmemAddr), static_cast<uint16_t>(crcBytes),
                                    crc);
    }

    // Raw copy of the palette memory for texture-pack checksums: the bytes as RDRAM holds
    // them, at the palette's own position (entry = TMEM word - 256).
    {
        const uint32_t first = static_cast<uint32_t>(tileDesc.tmemAddr) - 256;
        if (first + static_cast<uint32_t>(count) <= 256 && srcAddr + crcBytes <= m_rdramSize)
            std::memcpy(m_state.rdp.texFilterPalette + 8 + first * 2, m_rdram + srcAddr, crcBytes);
    }

    m_stats.textureLoads++;
    m_stats.frameLoads++;
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
}

// --- Sync Handlers ---

void RDPProcessor::HandleSyncFull() {
    // Signal the DP_FULL interrupt immediately: this is what the N64 CPU waits for. The callback
    // fires before FlushBatch so the CPU can overlap with rendering, matching real hardware where
    // the RDP issues the interrupt at sync time.
    if (m_syncFullCallback) m_syncFullCallback();

    // Count scene draws for readback decision (snapshot is handled by FlushBatch).
    int sceneDraws = 0;
    for (const auto& c : m_batch.drawCalls) {
        if (c.type == DrawCall::Type::Triangle || c.type == DrawCall::Type::TextureRect) ++sceneDraws;
    }

    FlushBatch();

    // --- GPU to RDRAM readback ---
    // After the frame is complete, copy rendered pixels back to RDRAM so the
    // CPU sees up-to-date data if it reads the framebuffer memory directly.
    //
    // Throttle: glMapBufferRange(GL_MAP_READ_BIT) blocks ~10-50ms on Intel Iris Xe even with a
    // signaled fence (driver bug), and doing this on every SyncFull tanks FPS to ~47. Throttling to
    // every Nth call keeps the bulk of frames at native pace; mid-frame CPU reads of stale FB data
    // are uncommon and tolerated.
    // The throttle must stay GL-only. Its justification is a specific GL/Intel driver problem;
    // Vulkan's readback is a different mechanism (submit + fence, no glMapBufferRange) and the
    // backend's readback budget already dedupes it per address per frame, so it cannot run away.
    // Throttling it would be wrong: the CPU-side PreRender framebuffer capture that produces MM's
    // pause background happens on one specific frame (the moment the player opens the menu), so
    // 1-in-8 sampling misses the only frame that matters 7 times out of 8, and the capture reads
    // stale or zero RDRAM exactly as if no readback existed.
    static uint64_t s_rbCounter = 0;
    constexpr uint64_t kRdpReadbackInterval = 8;
    const bool readbackDue = !g_fbManager.IsGL() || ((s_rbCounter++ % kRdpReadbackInterval) == 0);
    if (sceneDraws >= 1 && m_rdram && readbackDue) {
        // Complete any previous async readback first
        if (g_fbManager.IsReadbackPending()) g_fbManager.FinishAsyncReadback(m_rdram);

        GPUFramebuffer* colorFB = g_fbManager.GetCurrentColor();
        if (colorFB && colorFB->desc.address != 0) {
            if (g_fbManager.IsGL()) {
                g_fbManager.BeginAsyncReadback(colorFB);
            } else if (m_renderer) {
                // --- Vulkan: the CFB to RDRAM readback ---
                // BeginAsyncReadback and CopyFromGPU both start with `if (!m_isGL) return;`, so on this backend
                // the "copy rendered pixels back so the CPU sees real data" feature would be a silent no-op
                // without this block. That matters: OoT/MM build the pause-menu background on the CPU (the
                // decomp's PreRender_* routines read the color framebuffer out of RDRAM, blur it, and write the
                // result to the prerender buffer). If RDRAM never receives the rendered frame, that code reads
                // zeros, blurs zeros and writes zeros, so the inventory panel is black however the later
                // read-back of the prerender buffer is handled. GLideN64 calls this framebuffer emulation
                // feature `copyToRDRAM`; it is why that option exists and is required for OoT/MM's pause screen.
                // It runs here (SyncFull) rather than at VI/present time because the frame is still recording
                // and CopyFramebufferToRDRAM needs a live command buffer. It is synchronous (submit + fence
                // wait) and the backend's readback budget dedupes it per address per frame.
                const auto& d = colorFB->desc;
                m_renderer->CopyFramebufferToRDRAM(d.address, d.width, d.height, d.format == FBFormat::RGBA16, m_rdram);
                g_fbManager.RecordRdramFingerprint(colorFB, m_rdram, m_rdramSize);
                // Also sync the display buffer when it differs from the current render target. MM
                // double-buffers (0x000500 / 0x7DA800) and the CPU capture reads the frame the player is
                // actually looking at, which is the VI-origin buffer, not necessarily the buffer the display
                // list ended on. If only the render target were written back, a capture taken while the other
                // buffer was on screen would read empty RDRAM. The backend dedupes per address per frame, so
                // this adds at most one extra copy and none at all for single-buffered scenes.
                GPUFramebuffer* displayFB = g_fbManager.GetDisplayFramebuffer();
                if (displayFB && displayFB != colorFB && displayFB->desc.address != 0 &&
                    displayFB->desc.address != d.address) {
                    const auto& dd = displayFB->desc;
                    m_renderer->CopyFramebufferToRDRAM(dd.address, dd.width, dd.height, dd.format == FBFormat::RGBA16,
                                                       m_rdram);
                    g_fbManager.RecordRdramFingerprint(displayFB, m_rdram, m_rdramSize);
                }
            }
        }

        // Depth readback is synchronous and expensive; only do it for
        // games that actually read depth from RDRAM (e.g., Perfect Dark).
        if (g_fbManager.m_fbHacks.pdDepthOncePerFrame) {
            if (m_state.rdp.zbufferImage.address != 0) {
                GPUFramebuffer* depthFB = g_fbManager.FindByAddress(m_state.rdp.zbufferImage.address, FBType::Depth);
                if (depthFB && depthFB->depthTex) g_fbManager.CopyDepthFromGPU(depthFB, m_rdram);
            }
        }
    }

    m_stats.syncFullCount++;
    m_state.rdp.syncCount++;

    // --- Clear coverage image for next frame ---
    // On real N64, a FILL-mode FillRect clears both the color framebuffer and the per-pixel
    // coverage. Our FillRect draws via normal GL (not MLE imageStore), so the coverage image keeps
    // stale values from the previous frame's geometry. With b1=1 (B = mem_coverage), stale coverage
    // makes edge pixels blend with the old frame's color, producing visible wireframe outlines on
    // every triangle. Clearing the coverage image at frame boundaries starts the next frame with
    // cvg=0 everywhere (matching N64's post-fill state).
    if (g_fbManager.IsGL()) {
        GPUFramebuffer* fb = g_fbManager.GetCurrentColor();
        if (fb && fb->coverageTex) {
            const GLuint zero = 0;
            glClearTexImage(fb->coverageTex, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, &zero);
        }
    }

    // --- Defensive depth clear ---
    // Games normally clear depth via a FillRect targeting the Z-buffer address, which
    // HandleFillRect intercepts. If a game skips that clear (or detection missed it), stale depth
    // from the previous frame can hide 2D HUD elements behind 3D geometry, so the depth buffer is
    // cleared at frame boundaries as a safety net. Mirrors the two branches of the detected clear
    // path in RDPProcessor_Draw.cpp (search "Vulkan: clear the active swapchain depth attachment");
    // without the Vulkan branch a missed detection would have no safety net there.
    if (g_fbManager.IsGL()) {
        GPUFramebuffer* fb = g_fbManager.GetCurrentColor();
        if (fb && fb->fbo) {
            glClearNamedFramebufferfi(fb->fbo, GL_DEPTH_STENCIL, 0, 1.0f, 0);
        }
    } else if (m_renderer) {
        m_renderer->ClearDepthBuffer(1.0f);
    }
}

void RDPProcessor::HandleSyncPipe() {
    // SyncPipe ensures all primitives have been written before a state change; the HLE batch
    // system already guarantees that.
    m_stats.syncPipeCount++;
}

void RDPProcessor::HandleSyncTile() { m_stats.syncTileCount++; }

void RDPProcessor::HandleSyncLoad() { m_stats.syncLoadCount++; }

} // namespace Nova64::Video
