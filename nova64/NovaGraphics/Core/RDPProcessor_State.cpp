#include "RDPProcessor_Private.hpp"
void RDPProcessor::HandleSetOtherModes(const uint32_t* words) {
    m_decoder.DecodeSetOtherModes(words, m_state.rdp);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetCombine(const uint32_t* words) {
    m_decoder.DecodeSetCombine(words, m_state.rdp);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetTile(const uint32_t* words) {
    int tileIdx = (words[1] >> 24) & 0x07;
    m_decoder.DecodeSetTile(words, m_state.rdp.tiles[tileIdx]);
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
    m_stats.stateChanges++;
    MM_RDP_TRACE("SetTile tile={} fmt={} size={} line={} tmem={} pal={}", tileIdx,
                 static_cast<int>(m_state.rdp.tiles[tileIdx].format), static_cast<int>(m_state.rdp.tiles[tileIdx].size),
                 m_state.rdp.tiles[tileIdx].line, m_state.rdp.tiles[tileIdx].tmemAddr,
                 static_cast<int>(m_state.rdp.tiles[tileIdx].palette));
}

void RDPProcessor::HandleSetTileSize(const uint32_t* words) {
    int tileIdx = (words[1] >> 24) & 0x07;
    m_decoder.DecodeSetTileSize(words, m_state.rdp.tiles[tileIdx]);
    m_state.rdp.MarkDirty(RDPDirtyFlag::Tiles);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetScissor(const uint32_t* words) {
    // Flush existing batch before changing scissor � N64 scissor applies per-triangle
    // and we use a single global scissor in FlushBatch, so a mid-batch change would
    // apply the new scissor to all previously-batched triangles incorrectly.
    if (!m_batch.drawCalls.empty()) FlushBatch();
    m_decoder.DecodeSetScissor(words, m_state.rdp.scissor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::Scissor);
    m_stats.stateChanges++;
    // Keep FramebufferManager in sync so new FBOs get the right height
    uint32_t scissorX = m_state.rdp.scissor.xh >> 2;
    uint32_t scissorY = m_state.rdp.scissor.yh >> 2;
    uint32_t scissorW = m_state.rdp.scissor.xl >> 2;
    uint32_t scissorH = m_state.rdp.scissor.yl >> 2;
    g_fbManager.SetScissorHint(scissorX, scissorY, scissorW, scissorH);
}

void RDPProcessor::HandleSetColorImage(const uint32_t* words) {
    // Remember the previous color image address for mid-frame readback
    uint32_t prevAddr = m_state.rdp.colorImage.address;

    FlushBatch(); // Changing render target requires flush

    // Vulkan: capture the current swapchain content BEFORE the new FB's renders overwrite it.
    // N64 games render a background to FB-A, then switch to FB-B for the overlay (e.g. MM
    // inventory).  By the time a subsequent LoadTile reads from FB-A, the swapchain already
    // holds FB-B content.  Snapshotting here — at switch time — captures the correct content.
    //
    // Gate on needsInventorySnapshot, NOT gpuModified: LoadBlock/LoadTile's FB-as-texture
    // sync path (RDPProcessor_Texture.cpp) also clears gpuModified for its own purpose (e.g.
    // OoT/MM's every-frame CfB/AA-resolve copy-back reads the color image as a texture before
    // the player ever opens a menu). If that path runs first, gpuModified is already false by
    // the time this fires, so the inventory background was never captured -- a black image.
    if (!g_fbManager.IsGL() && m_renderer) {
        GPUFramebuffer* prevFB = g_fbManager.GetCurrentColor();
        if (prevFB && prevFB->needsInventorySnapshot) {
            const uint32_t snapW = m_renderer->GetWidth();
            const uint32_t snapH = m_renderer->GetHeight();
            if (snapW > 0 && snapH > 0) {
                // Ping-pong: flip slot FIRST so the previously-bound snapshot
                // (which may still be sampled by an in-flight GPU frame) is not
                // overwritten by this capture.
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
                    m_renderer->SnapshotToTexture(slot.get());
                    m_vkFBSnapshotAddrs[m_vkFBSnapshotIdx] = prevFB->desc.address & 0x7FFFFF;
                    prevFB->needsInventorySnapshot = false;
                }
            }
        }
    }

    m_decoder.DecodeSetColorImage(words, m_state.rdp.colorImage);
    m_state.rdp.colorImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.colorImage.address);

    MM_RDP_TRACE("SetColorImage prev=0x{:06X} addr=0x{:06X} fmt={} size={} width={} z=0x{:06X}", prevAddr & 0x7FFFFF,
                 m_state.rdp.colorImage.address & 0x7FFFFF, static_cast<int>(m_state.rdp.colorImage.format),
                 static_cast<int>(m_state.rdp.colorImage.size), m_state.rdp.colorImage.width,
                 m_state.rdp.zbufferImage.address & 0x7FFFFF);

    // NOTE: We intentionally do NOT do a proactive CopyFromGPU here.
    // The reactive copies in LoadBlock/LoadTile already handle reading
    // from GPU framebuffers when needed.  A proactive copy here would
    // overwrite unrelated RDRAM data (e.g. palettes) that overlaps
    // with the framebuffer address range.

    m_state.rdp.MarkDirty(RDPDirtyFlag::ColorImage);
    m_stats.stateChanges++;

    // Vulkan per-buffer canvas isolation: tell the backend which N64 color
    // buffer subsequent draws target so it can swap persistent canvases.
    // (GL default no-op — the FB manager binds a real FBO below instead.)
    if (m_renderer) m_renderer->SetActiveCanvas(m_state.rdp.colorImage.address & 0x7FFFFF);

    g_fbManager.OnSetColorImage(m_state.rdp.colorImage.address, m_state.rdp.colorImage.width,
                                static_cast<uint32_t>(m_state.rdp.colorImage.format),
                                static_cast<uint32_t>(m_state.rdp.colorImage.size));
    // Evict any stale TextureCache entries at this address � it is now a GPU render target
    if (m_textureCache) {
        uint32_t fbSizeBytes = m_state.rdp.colorImage.width * 240 * 4; // conservative upper bound
        m_textureCache->InvalidateRange(m_state.rdp.colorImage.address, fbSizeBytes);
    }
}

void RDPProcessor::HandleSetMaskImage(const uint32_t* words) {
    FlushBatch();
    m_decoder.DecodeSetMaskImage(words, m_state.rdp.zbufferImage);
    m_state.rdp.zbufferImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.zbufferImage.address);

    MM_RDP_TRACE("SetMaskImage z=0x{:06X} color=0x{:06X} colorFmt={} colorSize={} colorWidth={}",
                 m_state.rdp.zbufferImage.address & 0x7FFFFF, m_state.rdp.colorImage.address & 0x7FFFFF,
                 static_cast<int>(m_state.rdp.colorImage.format), static_cast<int>(m_state.rdp.colorImage.size),
                 m_state.rdp.colorImage.width);

    m_state.rdp.MarkDirty(RDPDirtyFlag::ZBufferImage);
    m_stats.stateChanges++;

    g_fbManager.OnSetDepthImage(m_state.rdp.zbufferImage.address);

    // Wire into the N64-accurate depth buffer manager
    g_depthManager.OnSetDepthImage(m_state.rdp.zbufferImage.address, m_state.rdp.colorImage.width);

    // Proactively initialize Z-image textures so SF_N64_DEPTH_COMPARE can be
    // set on the very first draw call after SetMaskImage.  Without this, the
    // first frame has glZImageTex == 0 and falls back to simple GL depth.
    // The Z-image is cleared to max depth (1.0) only when first created;
    // afterwards the game clears it via FillRect to the depth buffer address.
    auto* depthBuf = g_depthManager.GetCurrent();
    if (depthBuf) {
        bool isNew = (depthBuf->glZImageTex == 0);
        g_depthManager.InitDepthImageTextures(*depthBuf, g_fbManager.GetResolutionScale());
        if (isNew) {
            g_depthManager.ClearDepthImageTextures(*depthBuf);
        }
    }
}

void RDPProcessor::HandleSetTextureImage(const uint32_t* words) {
    m_decoder.DecodeSetTextureImage(words, m_state.rdp.textureImage);
    m_state.rdp.textureImage.address = m_state.rdp.geometry.ResolveAddress(m_state.rdp.textureImage.address);

    MM_RDP_TRACE("SetTextureImage addr=0x{:06X} fmt={} size={} width={} color=0x{:06X} z=0x{:06X}",
                 m_state.rdp.textureImage.address & 0x7FFFFF, static_cast<int>(m_state.rdp.textureImage.format),
                 static_cast<int>(m_state.rdp.textureImage.size), m_state.rdp.textureImage.width,
                 m_state.rdp.colorImage.address & 0x7FFFFF, m_state.rdp.zbufferImage.address & 0x7FFFFF);

    m_state.rdp.MarkDirty(RDPDirtyFlag::TextureImage);
    m_stats.stateChanges++;
}

void RDPProcessor::HandleSetFillColor(const uint32_t* words) {
    // Store the raw 32-bit fill color register so we can re-decode at draw time
    // (in case the color image size changes between SetFillColor and FillRect).
    uint32_t raw = words[1];
    m_state.rdp.fillColorRaw = raw;
    DecodeFillColor(raw, m_state.rdp.colorImage.size, m_state.rdp.fillColor.r, m_state.rdp.fillColor.g,
                    m_state.rdp.fillColor.b, m_state.rdp.fillColor.a);
    m_state.rdp.MarkDirty(RDPDirtyFlag::FillColor);
}

void RDPProcessor::HandleSetFogColor(const uint32_t* words) {
    m_decoder.DecodeSetFogColor(words, m_state.rdp.fogColor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::FogColor);
}

void RDPProcessor::HandleSetBlendColor(const uint32_t* words) {
    m_decoder.DecodeSetBlendColor(words, m_state.rdp.blendColor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::BlendColor);
}

void RDPProcessor::HandleSetPrimColor(const uint32_t* words) {
    m_decoder.DecodeSetPrimColor(words, m_state.rdp);
    m_state.rdp.MarkDirty(RDPDirtyFlag::PrimColor);
}

void RDPProcessor::HandleSetEnvColor(const uint32_t* words) {
    m_decoder.DecodeSetEnvColor(words, m_state.rdp.envColor);
    m_state.rdp.MarkDirty(RDPDirtyFlag::EnvColor);
    MM_RDP_TRACE("[ENV-COLOR] r={} g={} b={} a={}", m_state.rdp.envColor.r, m_state.rdp.envColor.g,
                 m_state.rdp.envColor.b, m_state.rdp.envColor.a);
}

void RDPProcessor::HandleSetPrimDepth(const uint32_t* words) {
    FlushBatch();
    m_decoder.DecodeSetPrimDepth(words, m_state.rdp.primDepth, m_state.rdp.primDeltaZ);
    m_state.rdp.MarkDirty(RDPDirtyFlag::PrimDepth);
}

void RDPProcessor::HandleSetConvert(const uint32_t* words) { m_decoder.DecodeSetConvert(words, m_state.rdp.convertK); }

void RDPProcessor::HandleSetKeyGB(const uint32_t* words) { m_decoder.DecodeSetKeyGB(words, m_state.rdp); }

void RDPProcessor::HandleSetKeyR(const uint32_t* words) { m_decoder.DecodeSetKeyR(words, m_state.rdp); }

// ═══════════════════════════════════════════════════════════════════════════
//  Texture Load Handlers
// ═══════════════════════════════════════════════════════════════════════════

} // namespace Nova64::Video
