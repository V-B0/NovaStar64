/**
 * @file ShaderManager.cpp
 * @brief Shader Manager.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Shader Manager Implementation

#include "ShaderManager.hpp"
#include "../Common/Logger.hpp"
#include <glad/gl.h>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>

namespace Nova64 {
namespace Video {
namespace Shader {

// ═══════════════════════════════════════════════════════════════════════════════
// SHADER CACHE IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════════

ShaderCache::ShaderCache() : m_maxShaders(4096), m_currentFrame(0) {}

ShaderCache::~ShaderCache() { Shutdown(); }

void ShaderCache::Initialize(uint32_t maxShaders) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_maxShaders = maxShaders;
    m_currentFrame = 0;
    m_stats = ShaderCacheStats();
    m_cache.reserve(maxShaders / 2);
}

void ShaderCache::Shutdown() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (m_deleteCallback) {
        for (auto& [k, entry] : m_cache) m_deleteCallback(entry.shader);
    }
    m_cache.clear();
    m_stats = ShaderCacheStats();
}

void ShaderCache::Clear() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (m_deleteCallback) {
        for (auto& [k, entry] : m_cache) m_deleteCallback(entry.shader);
    }
    m_cache.clear();
    m_stats.currentCacheSize = 0;
}

const CompiledShader* ShaderCache::Find(const CombinerKey& key) const {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        const_cast<CacheEntry&>(it->second).lastAccessFrame = m_currentFrame;
        const_cast<CacheEntry&>(it->second).accessCount++;
        const_cast<ShaderCacheStats&>(m_stats).cacheHits++;
        return &it->second.shader;
    }
    const_cast<ShaderCacheStats&>(m_stats).cacheMisses++;
    return nullptr;
}

void ShaderCache::Insert(const CombinerKey& key, CompiledShader&& shader) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // Evict if at capacity
    if (m_cache.size() >= m_maxShaders) {
        // Find the entry with oldest access
        auto oldest = m_cache.begin();
        uint64_t oldestFrame = UINT64_MAX;
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (it->second.lastAccessFrame < oldestFrame) {
                oldestFrame = it->second.lastAccessFrame;
                oldest = it;
            }
        }
        if (m_deleteCallback) m_deleteCallback(oldest->second.shader);
        m_cache.erase(oldest);
        m_stats.evictions++;
    }

    CacheEntry entry;
    entry.shader = std::move(shader);
    entry.lastAccessFrame = m_currentFrame;
    entry.accessCount = 1;
    m_cache[key] = std::move(entry);

    m_stats.currentCacheSize = static_cast<uint32_t>(m_cache.size());
    m_stats.totalCompiled++;
}

bool ShaderCache::Remove(const CombinerKey& key) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        if (m_deleteCallback) m_deleteCallback(it->second.shader);
        m_cache.erase(it);
        m_stats.currentCacheSize = static_cast<uint32_t>(m_cache.size());
        return true;
    }
    return false;
}

void ShaderCache::EvictLRU(int count) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    for (int i = 0; i < count && !m_cache.empty(); ++i) {
        auto oldest = m_cache.begin();
        uint64_t oldestFrame = UINT64_MAX;
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (it->second.lastAccessFrame < oldestFrame) {
                oldestFrame = it->second.lastAccessFrame;
                oldest = it;
            }
        }
        if (m_deleteCallback) m_deleteCallback(oldest->second.shader);
        m_cache.erase(oldest);
        m_stats.evictions++;
    }
    m_stats.currentCacheSize = static_cast<uint32_t>(m_cache.size());
}

// Binary format: [MAGIC][VERSION][COUNT][ [KEY][VERT_LEN][VERT_SRC][FRAG_LEN][FRAG_SRC] ... ]
static const uint32_t CACHE_MAGIC = 0x4E565343; // 'NVSC'
static const uint32_t CACHE_VERSION = 19;

bool ShaderCache::SaveToDisk(const std::string& path) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.write(reinterpret_cast<const char*>(&CACHE_MAGIC), sizeof(CACHE_MAGIC));
    file.write(reinterpret_cast<const char*>(&CACHE_VERSION), sizeof(CACHE_VERSION));

    uint32_t count = static_cast<uint32_t>(m_cache.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (auto& [key, entry] : m_cache) {
        // Write key
        file.write(reinterpret_cast<const char*>(&key.mux), sizeof(key.mux));
        file.write(reinterpret_cast<const char*>(&key.blenderBits), sizeof(key.blenderBits));
        file.write(reinterpret_cast<const char*>(&key.otherModeH), sizeof(key.otherModeH));
        file.write(reinterpret_cast<const char*>(&key.otherModeL), sizeof(key.otherModeL));
        file.write(reinterpret_cast<const char*>(&key.features), sizeof(key.features));

        // Write vertex source
        uint32_t vertLen = static_cast<uint32_t>(entry.shader.vertexSource.size());
        file.write(reinterpret_cast<const char*>(&vertLen), sizeof(vertLen));
        file.write(entry.shader.vertexSource.c_str(), vertLen);

        // Write fragment source
        uint32_t fragLen = static_cast<uint32_t>(entry.shader.fragmentSource.size());
        file.write(reinterpret_cast<const char*>(&fragLen), sizeof(fragLen));
        file.write(entry.shader.fragmentSource.c_str(), fragLen);
    }

    return file.good();
}

bool ShaderCache::LoadFromDisk(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t magic, version;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));

    if (magic != CACHE_MAGIC || version != CACHE_VERSION) return false;

    uint32_t count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    for (uint32_t i = 0; i < count; ++i) {
        CombinerKey key;
        file.read(reinterpret_cast<char*>(&key.mux), sizeof(key.mux));
        file.read(reinterpret_cast<char*>(&key.blenderBits), sizeof(key.blenderBits));
        file.read(reinterpret_cast<char*>(&key.otherModeH), sizeof(key.otherModeH));
        file.read(reinterpret_cast<char*>(&key.otherModeL), sizeof(key.otherModeL));
        file.read(reinterpret_cast<char*>(&key.features), sizeof(key.features));

        std::string vertSource, fragSource;

        uint32_t vertLen;
        file.read(reinterpret_cast<char*>(&vertLen), sizeof(vertLen));
        vertSource.resize(vertLen);
        file.read(&vertSource[0], vertLen);

        uint32_t fragLen;
        file.read(reinterpret_cast<char*>(&fragLen), sizeof(fragLen));
        fragSource.resize(fragLen);
        file.read(&fragSource[0], fragLen);

        // Don't overwrite valid (binary-loaded) entries with invalid source
        if (m_cache.find(key) != m_cache.end()) continue;

        // Store source for later recompilation
        CacheEntry entry;
        entry.shader.key = key;
        entry.shader.features = key.features;
        entry.shader.vertexSource = std::move(vertSource);
        entry.shader.fragmentSource = std::move(fragSource);
        entry.shader.valid = false; // Needs recompilation
        entry.lastAccessFrame = 0;
        entry.accessCount = 0;
        m_cache[key] = std::move(entry);
    }

    m_stats.currentCacheSize = static_cast<uint32_t>(m_cache.size());
    return file.good();
}

bool ShaderCache::SaveBinaryCache(const std::string& path) const {
    // Binary cache saves GL program binaries for fast reload via GL_ARB_get_program_binary.
    // File format: magic(4) + version(4) + count(4) + entries[]
    // Each entry: CombinerKey(24) + features(8) + binaryFormat(4) + binaryLen(4) + data(len)
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    GLint numFormats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numFormats);
    if (numFormats == 0) return false; // Driver doesn't support binary retrieval

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    const uint32_t magic = 0x4E565342; // 'NVSB'
    const uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Placeholder for count — we'll seek back and fill it in
    const auto countPos = file.tellp();
    uint32_t count = 0;
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& [key, entry] : m_cache) {
        if (!entry.shader.valid || entry.shader.handle == INVALID_SHADER) continue;

        GLuint prog = static_cast<GLuint>(entry.shader.handle);

        GLint binaryLength = 0;
        glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
        if (binaryLength <= 0) continue;

        std::vector<uint8_t> binary(binaryLength);
        GLenum binaryFormat = 0;
        glGetProgramBinary(prog, binaryLength, nullptr, &binaryFormat, binary.data());

        // Write CombinerKey (POD, 24 bytes)
        file.write(reinterpret_cast<const char*>(&key), sizeof(CombinerKey));
        file.write(reinterpret_cast<const char*>(&entry.shader.features), sizeof(uint64_t));
        file.write(reinterpret_cast<const char*>(&binaryFormat), sizeof(binaryFormat));
        uint32_t len = static_cast<uint32_t>(binaryLength);
        file.write(reinterpret_cast<const char*>(&len), sizeof(len));
        file.write(reinterpret_cast<const char*>(binary.data()), binaryLength);
        ++count;
    }

    // Patch the count
    file.seekp(countPos);
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    return file.good();
}

bool ShaderCache::LoadBinaryCache(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t magic, version;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));

    if (magic != 0x4E565342 || version != 1) return false;

    uint32_t count = 0;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CombinerKey key{};
        uint64_t features = 0;
        GLenum binaryFormat = 0;
        uint32_t len = 0;

        file.read(reinterpret_cast<char*>(&key), sizeof(CombinerKey));
        file.read(reinterpret_cast<char*>(&features), sizeof(features));
        file.read(reinterpret_cast<char*>(&binaryFormat), sizeof(binaryFormat));
        file.read(reinterpret_cast<char*>(&len), sizeof(len));

        if (!file.good() || len == 0 || len > 16 * 1024 * 1024) break;

        std::vector<uint8_t> binary(len);
        file.read(reinterpret_cast<char*>(binary.data()), len);
        if (!file.good()) break;

        // If already in cache (source-compiled takes priority), skip
        if (m_cache.find(key) != m_cache.end()) continue;

        // Create program from binary
        GLuint prog = glCreateProgram();
        glProgramBinary(prog, binaryFormat, binary.data(), static_cast<GLsizei>(len));

        GLint linkStatus = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            // Binary is stale (driver update, GPU change, etc.)
            glDeleteProgram(prog);
            continue;
        }

        // Build a minimal CompiledShader — uniforms will be resolved by ShaderManager
        CompiledShader shader;
        shader.handle = static_cast<ShaderHandle>(prog);
        shader.key = key;
        shader.features = features;
        shader.valid = true;

        CacheEntry entry;
        entry.shader = std::move(shader);
        entry.lastAccessFrame = 0;
        entry.accessCount = 0;
        m_cache[key] = std::move(entry);
        ++loaded;
    }

    return loaded > 0;
}

void ShaderCache::ForEachEntry(std::function<void(const CombinerKey&, CompiledShader&)> callback) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    for (auto& [key, entry] : m_cache) {
        callback(key, entry.shader);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SHADER MANAGER IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════════

ShaderManager::ShaderManager() : m_currentShader(nullptr), m_api(BackendAPI::OpenGL), m_initialized(false) {}

ShaderManager::~ShaderManager() { Shutdown(); }

bool ShaderManager::Initialize(BackendAPI api) {
    m_api = api;
    m_cache.Initialize(4096);

    m_generator = std::make_unique<ShaderGenerator>();
    ShaderLanguage lang = ShaderLanguage::GLSL_460;
    switch (api) {
    case BackendAPI::OpenGL: lang = ShaderLanguage::GLSL_460; break;
    case BackendAPI::Vulkan: lang = ShaderLanguage::SPIRV; break;
    case BackendAPI::D3D11:  lang = ShaderLanguage::HLSL_SM5; break;
    default:                 break;
    }
    m_generator->Initialize(lang);

    // Compiler is created by the backend-specific factory
    // For now it remains null until SetCompiler() is called
    m_initialized = true;
    return true;
}

void ShaderManager::Shutdown() {
    m_depthOnlyShader.reset();
    m_uiShader.reset();
    m_fallbackShader.reset();
    m_postProcessShaders.clear();
    m_cache.Shutdown();
    m_generator.reset();
    m_compiler.reset();
    m_currentShader = nullptr;
    m_initialized = false;
}

const CompiledShader* ShaderManager::GetShader(const CombinerKey& key) {
    if (!m_initialized) return nullptr;

    // Try cache first
    const CompiledShader* cached = m_cache.Find(key);
    if (cached && cached->valid) {
        return cached;
    }

    // Compile and cache
    return CompileAndCache(key);
}

void ShaderManager::BindShader(const CompiledShader* shader) {
    if (!shader || !shader->valid || !m_compiler) return;
    // Always rebind — external code (BlitToScreen) may call glUseProgram
    // directly without updating m_currentShader.
    m_compiler->BindShader(*shader);
    m_currentShader = shader;
}

void ShaderManager::SetMatrixUniforms(const float* mvp, const float* modelView) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (mvp && u.mvpMatrix >= 0) m_compiler->SetUniformMatrix4fv(u.mvpMatrix, mvp);
    if (modelView && u.modelView >= 0) m_compiler->SetUniformMatrix4fv(u.modelView, modelView);
}

void ShaderManager::SetColorUniforms(const float* primColor, const float* envColor, const float* fogColor,
                                     const float* blendColor, float primLODFrac, float primLODMin, float centerR,
                                     float centerG, float centerB, float scaleR, float scaleG, float scaleB) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (primColor && u.primColor >= 0)
        m_compiler->SetUniform4f(u.primColor, primColor[0], primColor[1], primColor[2], primColor[3]);
    if (envColor && u.envColor >= 0)
        m_compiler->SetUniform4f(u.envColor, envColor[0], envColor[1], envColor[2], envColor[3]);
    if (fogColor && u.fogColor >= 0)
        m_compiler->SetUniform4f(u.fogColor, fogColor[0], fogColor[1], fogColor[2], fogColor[3]);
    if (blendColor && u.blendColor >= 0)
        m_compiler->SetUniform4f(u.blendColor, blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
    if (u.primLOD >= 0) m_compiler->SetUniform1f(u.primLOD, primLODMin);
    if (u.primLODFrac >= 0) m_compiler->SetUniform1f(u.primLODFrac, primLODFrac);
    // Chroma key center/scale — wired from RDP state
    if (u.centerColor >= 0) m_compiler->SetUniform3f(u.centerColor, centerR, centerG, centerB);
    if (u.scaleColor >= 0) m_compiler->SetUniform3f(u.scaleColor, scaleR, scaleG, scaleB);
}

void ShaderManager::SetFillColor(float r, float g, float b, float a) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.fillColor >= 0) m_compiler->SetUniform4f(u.fillColor, r, g, b, a);
}

void ShaderManager::SetTextureUniforms(int tex0Unit, int tex1Unit, const float* scale0, const float* scale1,
                                       const float* size0, const float* size1, const float* gpuScale0,
                                       const float* gpuScale1) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.texture0 >= 0) m_compiler->SetUniform1i(u.texture0, tex0Unit);
    if (u.texture1 >= 0) m_compiler->SetUniform1i(u.texture1, tex1Unit);
    if (scale0 && u.texScale0 >= 0) m_compiler->SetUniform2f(u.texScale0, scale0[0], scale0[1]);
    if (scale1 && u.texScale1 >= 0) m_compiler->SetUniform2f(u.texScale1, scale1[0], scale1[1]);
    if (size0 && u.texSize0 >= 0) m_compiler->SetUniform2f(u.texSize0, size0[0], size0[1]);
    if (size1 && u.texSize1 >= 0) m_compiler->SetUniform2f(u.texSize1, size1[0], size1[1]);
    if (gpuScale0 && u.gpuScale0 >= 0) m_compiler->SetUniform2f(u.gpuScale0, gpuScale0[0], gpuScale0[1]);
    if (gpuScale1 && u.gpuScale1 >= 0) m_compiler->SetUniform2f(u.gpuScale1, gpuScale1[0], gpuScale1[1]);
}

void ShaderManager::SetGPUScale(const float* gpuScale0, const float* gpuScale1) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (gpuScale0 && u.gpuScale0 >= 0) m_compiler->SetUniform2f(u.gpuScale0, gpuScale0[0], gpuScale0[1]);
    if (gpuScale1 && u.gpuScale1 >= 0) m_compiler->SetUniform2f(u.gpuScale1, gpuScale1[0], gpuScale1[1]);
}

void ShaderManager::SetTextureTileParams(const float* offset0, const float* offset1, const float* clamp0,
                                         const float* clamp1, const float* mask0, const float* mask1) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (offset0 && u.texOffset0 >= 0) m_compiler->SetUniform2f(u.texOffset0, offset0[0], offset0[1]);
    if (offset1 && u.texOffset1 >= 0) m_compiler->SetUniform2f(u.texOffset1, offset1[0], offset1[1]);
    if (clamp0 && u.texClamp0 >= 0) m_compiler->SetUniform4f(u.texClamp0, clamp0[0], clamp0[1], clamp0[2], clamp0[3]);
    if (clamp1 && u.texClamp1 >= 0) m_compiler->SetUniform4f(u.texClamp1, clamp1[0], clamp1[1], clamp1[2], clamp1[3]);
    if (mask0 && u.texMask0 >= 0) m_compiler->SetUniform2f(u.texMask0, mask0[0], mask0[1]);
    if (mask1 && u.texMask1 >= 0) m_compiler->SetUniform2f(u.texMask1, mask1[0], mask1[1]);
}

void ShaderManager::SetAlphaRef(float ref) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.alphaRef >= 0) m_compiler->SetUniform1f(m_currentShader->uniforms.alphaRef, ref);
}

void ShaderManager::SetFogParams(float multiplier, float offset) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.fogMultiplier >= 0) m_compiler->SetUniform1f(u.fogMultiplier, multiplier);
    if (u.fogOffset >= 0) m_compiler->SetUniform1f(u.fogOffset, offset);
}

void ShaderManager::SetTexCoordBounds(bool enabled, const float* bounds0, const float* bounds1) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.useTexCoordBounds >= 0) m_compiler->SetUniform1i(u.useTexCoordBounds, enabled ? 1 : 0);
    if (enabled && bounds0 && u.texCoordBounds0 >= 0)
        m_compiler->SetUniform4f(u.texCoordBounds0, bounds0[0], bounds0[1], bounds0[2], bounds0[3]);
    if (enabled && bounds1 && u.texCoordBounds1 >= 0)
        m_compiler->SetUniform4f(u.texCoordBounds1, bounds1[0], bounds1[1], bounds1[2], bounds1[3]);
}

void ShaderManager::SetDepthParams(float scale, float offset, float primDepth) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.depthScale >= 0) m_compiler->SetUniform1f(u.depthScale, scale);
    if (u.depthOffset >= 0) m_compiler->SetUniform1f(u.depthOffset, offset);
    if (u.primDepth >= 0) m_compiler->SetUniform1f(u.primDepth, primDepth);
}

void ShaderManager::SetN64DepthMode(int zMode, float primDeltaZ) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.zMode >= 0) m_compiler->SetUniform1i(u.zMode, zMode);
    if (u.primDeltaZ >= 0) m_compiler->SetUniform1f(u.primDeltaZ, primDeltaZ);
}

void ShaderManager::SetConvertK(const int16_t convertK[6]) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    // K0-K3 packed into vec4 for YUV→RGB conversion (9-bit signed → float)
    if (u.convertK >= 0)
        m_compiler->SetUniform4f(u.convertK, convertK[0] / 256.0f, convertK[1] / 256.0f, convertK[2] / 256.0f,
                                 convertK[3] / 256.0f);
    if (u.k4 >= 0) m_compiler->SetUniform1f(u.k4, convertK[4] / 255.0f);
    if (u.k5 >= 0) m_compiler->SetUniform1f(u.k5, convertK[5] / 255.0f);
}

void ShaderManager::SetScreenSize(float width, float height) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.screenSize >= 0) m_compiler->SetUniform2f(u.screenSize, width, height);
    if (u.invScreenSize >= 0) m_compiler->SetUniform2f(u.invScreenSize, 1.0f / width, 1.0f / height);
}

void ShaderManager::SetFrameCount(uint32_t frame) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.frameCount >= 0)
        m_compiler->SetUniform1i(m_currentShader->uniforms.frameCount, static_cast<int>(frame));
}

void ShaderManager::SetResolutionScale(float scaleX, float scaleY) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.resolutionScale >= 0)
        m_compiler->SetUniform2f(m_currentShader->uniforms.resolutionScale, scaleX, scaleY);
}

void ShaderManager::SetTexturePersp(int persp) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.texturePersp >= 0)
        m_compiler->SetUniform1i(m_currentShader->uniforms.texturePersp, persp);
}

void ShaderManager::SetFBTextureParams(int mono0, int mono1, int fixedAlpha0, int fixedAlpha1) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.fbMonochrome0 >= 0) m_compiler->SetUniform1i(u.fbMonochrome0, mono0);
    if (u.fbMonochrome1 >= 0) m_compiler->SetUniform1i(u.fbMonochrome1, mono1);
    if (u.fbFixedAlpha0 >= 0) m_compiler->SetUniform1i(u.fbFixedAlpha0, fixedAlpha0);
    if (u.fbFixedAlpha1 >= 0) m_compiler->SetUniform1i(u.fbFixedAlpha1, fixedAlpha1);
}

void ShaderManager::SetScreenScale(float scaleX, float scaleY) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.screenScale >= 0)
        m_compiler->SetUniform2f(m_currentShader->uniforms.screenScale, scaleX, scaleY);
}

void ShaderManager::SetKeyWidth(float r, float g, float b) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.keyWidth >= 0) m_compiler->SetUniform3f(m_currentShader->uniforms.keyWidth, r, g, b);
}

void ShaderManager::SetLODParams(float minLod, int maxTile, int textureDetail) {
    if (!m_currentShader || !m_compiler) return;
    const auto& u = m_currentShader->uniforms;
    if (u.minLod >= 0) m_compiler->SetUniform1f(u.minLod, minLod);
    if (u.maxTile >= 0) m_compiler->SetUniform1i(u.maxTile, maxTile);
    if (u.textureDetail >= 0) m_compiler->SetUniform1i(u.textureDetail, textureDetail);
}

void ShaderManager::SetFogUsage(int fogUsage) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.fogUsage >= 0) m_compiler->SetUniform1i(m_currentShader->uniforms.fogUsage, fogUsage);
}

void ShaderManager::SetScreenSpaceTriangle(int screenSpace) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.screenSpaceTriangle >= 0)
        m_compiler->SetUniform1i(m_currentShader->uniforms.screenSpaceTriangle, screenSpace);
}

void ShaderManager::SetDebugOutput(int mode) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.debugOutput >= 0)
        m_compiler->SetUniform1i(m_currentShader->uniforms.debugOutput, mode);
}

void ShaderManager::SetTexCvgAlpha(int v) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.texCvgAlpha >= 0) m_compiler->SetUniform1i(m_currentShader->uniforms.texCvgAlpha, v);
}

void ShaderManager::SetTexHiRes(int bits) {
    if (!m_currentShader || !m_compiler) return;
    if (m_currentShader->uniforms.texHiRes >= 0) m_compiler->SetUniform1i(m_currentShader->uniforms.texHiRes, bits);
}

const CompiledShader* ShaderManager::GetDepthOnlyShader() {
    if (m_depthOnlyShader && m_depthOnlyShader->valid) return m_depthOnlyShader.get();
    if (!m_compiler || !m_generator) return nullptr;

    std::string vertSrc = m_generator->GenerateDepthOnlyVertex();
    std::string fragSrc = m_generator->GenerateDepthOnlyFragment();

    CombinerKey key = {};
    key.features = SF_DEPTH_WRITE;
    auto compiled = m_compiler->Compile(key, vertSrc, fragSrc);
    if (compiled.valid) {
        m_depthOnlyShader = std::make_unique<CompiledShader>(std::move(compiled));
        return m_depthOnlyShader.get();
    }
    return nullptr;
}

const CompiledShader* ShaderManager::GetPostProcessShader(uint64_t features) {
    auto it = m_postProcessShaders.find(features);
    if (it != m_postProcessShaders.end() && it->second->valid) return it->second.get();
    if (!m_compiler || !m_generator) return nullptr;

    std::string vertSrc = m_generator->GeneratePostProcessVertex();
    std::string fragSrc = m_generator->GeneratePostProcessFragment(features);

    CombinerKey key = {};
    key.features = features;
    auto compiled = m_compiler->Compile(key, vertSrc, fragSrc);
    if (compiled.valid) {
        m_postProcessShaders[features] = std::make_unique<CompiledShader>(std::move(compiled));
        return m_postProcessShaders[features].get();
    }
    return nullptr;
}

const CompiledShader* ShaderManager::GetUIShader() {
    if (m_uiShader && m_uiShader->valid) return m_uiShader.get();
    if (!m_compiler || !m_generator) return nullptr;

    std::string vertSrc = m_generator->GenerateUIVertex();
    std::string fragSrc = m_generator->GenerateUIFragment();

    CombinerKey key = {};
    auto compiled = m_compiler->Compile(key, vertSrc, fragSrc);
    if (compiled.valid) {
        m_uiShader = std::make_unique<CompiledShader>(std::move(compiled));
        return m_uiShader.get();
    }
    return nullptr;
}

void ShaderManager::ClearCache() { m_cache.Clear(); }

bool ShaderManager::SaveCache(const std::string& path) {
    bool srcOk = m_cache.SaveToDisk(path);
    // Binary cache uses GL_ARB_get_program_binary — only valid on OpenGL path.
    // Calling glGetIntegerv without an active GL context (Vulkan) causes a crash.
    bool binOk = false;
    if (m_api == BackendAPI::OpenGL) {
        std::string binPath = path + ".bin";
        binOk = m_cache.SaveBinaryCache(binPath);
    }
    return srcOk || binOk;
}

bool ShaderManager::LoadCache(const std::string& path) {
    // 1. Try binary cache first — pre-compiled GPU programs load in microseconds
    // Binary cache uses glProgramBinary — only available on OpenGL.
    if (m_api == BackendAPI::OpenGL) {
        std::string binPath = path + ".bin";
        m_cache.LoadBinaryCache(binPath);
    }

    // 2. Load source cache — fills entries not covered by binary cache
    //    (LoadFromDisk will skip entries already loaded from binary)
    m_cache.LoadFromDisk(path);

    // 3. Resolve uniform locations for binary-loaded shaders
    if (m_compiler) {
        m_cache.ForEachEntry([this](const CombinerKey& /*key*/, CompiledShader& shader) {
            if (shader.valid && shader.handle != INVALID_SHADER) {
                QueryUniformLocations(shader);
            }
        });
    }

    return m_cache.GetStats().currentCacheSize > 0;
}

const ShaderCacheStats& ShaderManager::GetStats() const { return m_cache.GetStats(); }

void ShaderManager::ResetStats() {
    // Stats are part of cache; clearing them requires cache re-init
}

std::string ShaderManager::DumpShaderSource(const CombinerKey& key) const {
    if (!m_generator) return "";
    auto tmpl = m_generator->GenerateShader(key);
    std::string result;
    result += "=== VERTEX SHADER ===\n";
    for (auto& d : tmpl.defines) result += d + "\n";
    result += tmpl.vertexHeader + "\n";
    result += tmpl.vertexBody + "\n";
    result += "\n=== FRAGMENT SHADER ===\n";
    for (auto& d : tmpl.defines) result += d + "\n";
    result += tmpl.fragmentHeader + "\n";
    result += tmpl.fragmentBody + "\n";
    return result;
}

const CompiledShader* ShaderManager::GetFallbackShader() {
    if (m_fallbackShader && m_fallbackShader->valid) return m_fallbackShader.get();
    if (!m_compiler) return nullptr;

    static const char* fallbackVS = R"(
#version 460 core
layout(location = 0) in vec4 aPosition;
uniform mat4 uMVPMatrix;
void main() { gl_Position = uMVPMatrix * aPosition; }
)";
    static const char* fallbackFS = R"(
#version 460 core
out vec4 FragColor;
void main() { FragColor = vec4(1.0, 0.0, 1.0, 1.0); }
)";

    CombinerKey fallbackKey = {};
    auto compiled = m_compiler->Compile(fallbackKey, fallbackVS, fallbackFS);
    if (compiled.valid) {
        QueryUniformLocations(compiled);
        m_fallbackShader = std::make_unique<CompiledShader>(std::move(compiled));
        return m_fallbackShader.get();
    }
    return nullptr;
}

void ShaderManager::OnFrameBegin() {
    // Could precompile predicted shaders here
}

void ShaderManager::OnFrameEnd() {
    // Update cache frame counter for LRU
    m_cache.AdvanceFrame();
}

const CompiledShader* ShaderManager::CompileAndCache(const CombinerKey& key) {
    if (!m_compiler || !m_generator) return nullptr;

    auto tmpl = m_generator->GenerateShader(key);

    // Build full source — header first (contains #version), then defines, then body
    std::string vertSrc, fragSrc;

    vertSrc = tmpl.vertexHeader + "\n";
    fragSrc = tmpl.fragmentHeader + "\n";
    for (auto& d : tmpl.defines) {
        vertSrc += d + "\n";
        fragSrc += d + "\n";
    }
    vertSrc += tmpl.vertexBody;
    fragSrc += tmpl.fragmentBody;

    auto startTime = std::chrono::high_resolution_clock::now();

    CompiledShader compiled;
    if (!tmpl.geometryBody.empty()) {
        std::string geomSrc = tmpl.geometryHeader + "\n";
        for (auto& d : tmpl.defines) geomSrc += d + "\n";
        geomSrc += tmpl.geometryBody;
        compiled = m_compiler->CompileWithGeometry(key, vertSrc, fragSrc, geomSrc);
    } else {
        compiled = m_compiler->Compile(key, vertSrc, fragSrc);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    compiled.compileTimeMs =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

    // Diagnostic: log each new shader compilation (WARN so it shows in release logs).
    NOVA_WARN(::Nova64::Video::LogSubsystem::Core,
              "ShaderManager: compiled shader features={:#018x} mux={:#018x} "
              "fragLen={} cpuMs={}",
              key.features, key.mux, fragSrc.size(), compiled.compileTimeMs);

    if (compiled.valid) {
        compiled.key = key;
        compiled.features = key.features;
        compiled.vertexSource = std::move(vertSrc);
        compiled.fragmentSource = std::move(fragSrc);

        // Query uniform locations
        QueryUniformLocations(compiled);

        m_cache.Insert(key, std::move(compiled));
        return m_cache.Find(key);
    }

    // Compilation failed — return a flat-magenta fallback so broken geometry
    // is visible for debugging instead of silently invisible.
    return GetFallbackShader();
}

void ShaderManager::QueryUniformLocations(CompiledShader& shader) {
    if (!m_compiler) return;

    auto& u = shader.uniforms;
    auto Q = [&](const char* name) { return m_compiler->GetUniformLocation(shader.handle, name); };

    // Matrices
    u.mvpMatrix = Q("uMVPMatrix");
    u.modelView = Q("uModelView");
    u.projection = Q("uProjection");

    // Textures
    u.texture0 = Q("uTexture0");
    u.texture1 = Q("uTexture1");
    u.tlut0 = Q("uTLUT0");
    u.tlut1 = Q("uTLUT1");
    u.depthTexture = Q("uDepthTexture");
    u.noiseTexture = Q("uNoiseTexture");
    u.ditherTexture = Q("uDitherTexture");
    u.fbTexture = Q("uFBTexture");

    // Colors
    u.primColor = Q("uPrimColor");
    u.envColor = Q("uEnvColor");
    u.fogColor = Q("uFogColor");
    u.blendColor = Q("uBlendColor");
    u.fillColor = Q("uFillColor");
    u.centerColor = Q("uCenterColor");
    u.scaleColor = Q("uScaleColor");
    u.primLOD = Q("uPrimLOD");
    u.primLODFrac = Q("uPrimLODFrac");
    u.k4 = Q("uK4");
    u.k5 = Q("uK5");
    u.convertK = Q("uConvertK");

    // Texture state
    u.texScale0 = Q("uTexScale0");
    u.texScale1 = Q("uTexScale1");
    u.texOffset0 = Q("uTexOffset0");
    u.texOffset1 = Q("uTexOffset1");
    u.texClamp0 = Q("uTexClamp0");
    u.texClamp1 = Q("uTexClamp1");
    u.texMask0 = Q("uTexMask0");
    u.texMask1 = Q("uTexMask1");
    u.texSize0 = Q("uTexSize0");
    u.texSize1 = Q("uTexSize1");
    u.gpuScale0 = Q("uGPUScale0");
    u.gpuScale1 = Q("uGPUScale1");

    // Other
    u.alphaRef = Q("uAlphaRef");
    u.fogMultiplier = Q("uFogMultiplier");
    u.fogOffset = Q("uFogOffset");
    u.depthScale = Q("uDepthScale");
    u.depthOffset = Q("uDepthOffset");
    u.primDepth = Q("uPrimDepth");
    u.zImageTex = Q("uZImageTex");
    u.deltaZImageTex = Q("uDeltaZImageTex");
    u.zLutTex = Q("uZLUT");
    u.zMode = Q("uZMode");
    u.primDeltaZ = Q("uPrimDeltaZ");
    u.texCoordBounds0 = Q("uTexCoordBounds0");
    u.texCoordBounds1 = Q("uTexCoordBounds1");
    u.useTexCoordBounds = Q("uUseTexCoordBounds");
    u.texturePersp = Q("uTexturePersp");
    u.fbMonochrome0 = Q("uFbMonochrome0");
    u.fbMonochrome1 = Q("uFbMonochrome1");
    u.fbFixedAlpha0 = Q("uFbFixedAlpha0");
    u.fbFixedAlpha1 = Q("uFbFixedAlpha1");
    u.screenScale = Q("uScreenScale");
    u.keyWidth = Q("uKeyWidth");
    u.minLod = Q("uMinLod");
    u.maxTile = Q("uMaxTile");
    u.textureDetail = Q("uTextureDetail");
    u.fogUsage = Q("uFogUsage");
    u.screenSpaceTriangle = Q("uScreenSpaceTriangle");
    u.screenSize = Q("uScreenSize");
    u.invScreenSize = Q("uInvScreenSize");
    u.frameCount = Q("uFrameCount");
    u.resolutionScale = Q("uResolutionScale");
    u.debugOutput = Q("uDebugOutput");
    u.texCvgAlpha = Q("uTexCvgAlpha");
    u.texHiRes = Q("uTexHiRes");
}

} // namespace Shader
} // namespace Video
} // namespace Nova64
