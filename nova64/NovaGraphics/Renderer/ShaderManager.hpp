/**
 * @file ShaderManager.hpp
 * @brief Shader management: combiner keys and feature flags, the compiled-shader cache, the backend compiler
 *        interface, the shader source generator and the ShaderManager that ties them together.
 */
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <array>

namespace Nova64 {
namespace Video {
namespace Shader {

// --- Shader Types ---
enum class ShaderType : uint8_t {
    Vertex = 0,
    Fragment = 1,
    Geometry = 2,
    Compute = 3,
    TessControl = 4,
    TessEval = 5,
    Count = 6
};

enum class ShaderLanguage : uint8_t {
    GLSL_460 = 0, // OpenGL 4.6 core
    GLSL_450 = 1, // OpenGL 4.5 fallback
    HLSL_SM5 = 2, // Direct3D 11 Shader Model 5
    SPIRV = 3,    // Vulkan / cross-compile
    Count = 4
};

enum class BackendAPI : uint8_t { OpenGL = 0, Vulkan = 1, D3D11 = 2, Count = 3 };

// --- Shader Feature Flags ---
// These control which code paths are compiled into the uber-shader
enum ShaderFeature : uint64_t {
    SF_NONE = 0,

    // Render modes
    SF_FILL_RECT = (1ULL << 0),
    SF_COPY_MODE = (1ULL << 1),
    SF_1CYCLE = (1ULL << 2),
    SF_2CYCLE = (1ULL << 3),

    // Texture
    SF_TEXTURE_0 = (1ULL << 4),
    SF_TEXTURE_1 = (1ULL << 5),
    SF_TEXTURE_DETAIL = (1ULL << 6),
    SF_TEXTURE_LOD = (1ULL << 7),
    SF_TEXTURE_SHARPEN = (1ULL << 8),
    SF_TEXTURE_3POINT = (1ULL << 9), // N64 3-point filtering
    SF_TEXTURE_BILINEAR = (1ULL << 10),
    SF_TEXTURE_YUV = (1ULL << 11),

    // Alpha
    SF_ALPHA_TEST = (1ULL << 12),
    SF_ALPHA_CVG = (1ULL << 13), // Coverage-based alpha
    SF_ALPHA_CVG_SEL = (1ULL << 14),
    SF_ALPHA_NOISE = (1ULL << 15),
    SF_ALPHA_DITHER = (1ULL << 16),

    // Blending
    SF_BLEND_COLOR = (1ULL << 17),
    SF_BLEND_FOG = (1ULL << 18),
    SF_BLEND_MULTIPLY = (1ULL << 19),
    SF_BLEND_ADD = (1ULL << 20),

    // Depth
    SF_DEPTH_TEST = (1ULL << 21),
    SF_DEPTH_WRITE = (1ULL << 22),
    SF_DEPTH_COMPARE = (1ULL << 23),
    SF_DEPTH_SOURCE_PRIM = (1ULL << 24), // Z from primitive instead of interpolated

    // Geometry
    SF_FOG = (1ULL << 25),
    SF_LIGHTING = (1ULL << 26),
    SF_SPECULAR = (1ULL << 27),
    SF_FLAT_SHADING = (1ULL << 28),
    SF_SMOOTH_SHADING = (1ULL << 29),

    // Special
    SF_NOISE_ENABLE = (1ULL << 30),
    SF_COVERAGE_ENABLE = (1ULL << 31),
    SF_FRAMEBUFFER_READ = (1ULL << 32), // For blend modes that read FB
    SF_COLOR_ON_COVERAGE = (1ULL << 33),

    // Post-processing
    SF_GAMMA_CORRECT = (1ULL << 34),
    SF_DITHER_ENABLE = (1ULL << 35),
    SF_AA_ENABLE = (1ULL << 36),
    SF_DIVOT_ENABLE = (1ULL << 37),
    SF_MONOCHROME = (1ULL << 38),         // Desaturate output (e.g. Zelda OoT cutscenes)
    SF_N64_DEPTH_COMPARE = (1ULL << 39),  // N64-accurate Z-image depth compare in shader
    SF_DEPTH_RENDER = (1ULL << 44),       // ColorImage == DepthImage: write color.r as depth
    SF_CHROMA_KEY = (1ULL << 45),         // N64 color key (chroma key alpha)
    SF_MLE_BLENDER = (1ULL << 46),        // MLE: full shader blender via imageLoad/imageStore
    SF_ALPHA_NOISE_DITHER = (1ULL << 47), // Alpha compare uses noise dither (mode 3)

    // Combiner complexity hints
    SF_COMBINER_SIMPLE = (1ULL << 40),   // Can use simplified combiner
    SF_COMBINER_COMPLEX = (1ULL << 41),  // Full 2-cycle combiner needed
    SF_FORCE_BLEND = (1ULL << 42),       // Explicitly blending
    SF_DUAL_SOURCE_BLEND = (1ULL << 43), // Dual-source output for HW blender compositing

    // Shadow map (depth-texture → TLUT → fog-color shadow overlay)
    SF_SHADOW_MAP = (1ULL << 49),

    // Resolution scale
    SF_HIRES = (1ULL << 48),

    // Debug
    SF_DEBUG_WIREFRAME = (1ULL << 60),
    SF_DEBUG_OVERDRAW = (1ULL << 61),
    SF_DEBUG_DEPTH_VIS = (1ULL << 62),
    SF_DEBUG_UV_VIS = (1ULL << 63),
};

// --- Combiner Key (uniquely identifies a combiner configuration) ---
struct CombinerKey {
    uint64_t mux;         // N64 combine mode (64-bit mux value)
    uint32_t blenderBits; // Blender mode bits
    uint32_t otherModeH;  // Relevant bits from OtherMode high
    uint32_t otherModeL;  // Relevant bits from OtherMode low
    uint64_t features;    // ShaderFeature flags

    bool operator==(const CombinerKey& o) const {
        return mux == o.mux && blenderBits == o.blenderBits && otherModeH == o.otherModeH &&
               otherModeL == o.otherModeL && features == o.features;
    }
};

struct CombinerKeyHasher {
    size_t operator()(const CombinerKey& key) const {
        size_t h = std::hash<uint64_t>{}(key.mux);
        h ^= std::hash<uint32_t>{}(key.blenderBits) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(key.otherModeH) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(key.otherModeL) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(key.features) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// --- Shader Uniform Locations ---
struct ShaderUniforms {
    // Matrices
    int mvpMatrix = -1;
    int modelView = -1;
    int projection = -1;

    // Textures
    int texture0 = -1;
    int texture1 = -1;
    int tlut0 = -1;
    int tlut1 = -1;
    int depthTexture = -1;
    int noiseTexture = -1;
    int ditherTexture = -1;
    int fbTexture = -1;

    // Color combiner inputs
    int primColor = -1;
    int envColor = -1;
    int fogColor = -1;
    int blendColor = -1;
    int fillColor = -1;
    int centerColor = -1; // vec3 chroma key center
    int scaleColor = -1;  // vec3 chroma key scale
    int primLOD = -1;
    int primLODFrac = -1;
    int k4 = -1;
    int k5 = -1;
    int convertK = -1; // vec4 for YUV conversion constants

    // Texture state
    int texScale0 = -1; // vec2
    int texScale1 = -1; // vec2
    int texOffset0 = -1;
    int texOffset1 = -1;
    int texClamp0 = -1; // vec4: clampS, clampT, mirrorS, mirrorT
    int texClamp1 = -1;
    int texMask0 = -1;  // vec2
    int texMask1 = -1;
    int texSize0 = -1;  // vec2: width, height
    int texSize1 = -1;
    int gpuScale0 = -1; // vec2
    int gpuScale1 = -1; // vec2

    // Alpha test
    int alphaRef = -1;

    // Fog
    int fogMultiplier = -1;
    int fogOffset = -1;

    // Depth
    int depthScale = -1;
    int depthOffset = -1;
    int primDepth = -1;

    // N64 Z-image depth compare
    int zImageTex = -1;
    int deltaZImageTex = -1;
    int zLutTex = -1;
    int zMode = -1;
    int primDeltaZ = -1;

    // Screen
    int screenSize = -1;    // vec2
    int invScreenSize = -1; // vec2
    int frameCount = -1;

    // Texture coordinate bounds
    int texCoordBounds0 = -1;   // vec4: minS, minT, maxS, maxT
    int texCoordBounds1 = -1;   // vec4: minS, minT, maxS, maxT
    int useTexCoordBounds = -1; // int

    // Texture perspective correction
    int texturePersp = -1; // int: 1 = perspective-correct, 0 = screen-space

    // Framebuffer-as-texture parameters
    int fbMonochrome0 = -1; // int: monochrome mode tile 0 (0=off,1=red,2=luma)
    int fbMonochrome1 = -1; // int: monochrome mode tile 1
    int fbFixedAlpha0 = -1; // int: fixed alpha tile 0 (0=off, 1=0.825)
    int fbFixedAlpha1 = -1; // int: fixed alpha tile 1

    // LOD screen scale
    int screenScale = -1; // vec2: resolution scale for LOD computation

    // Chroma key width
    int keyWidth = -1; // vec3: chroma key width per channel

    // LOD mipmap parameters
    int minLod = -1;        // float: primLODMin (minimum LOD level)
    int maxTile = -1;       // int: max mipmap tile index
    int textureDetail = -1; // int: 0=clamp, 1=sharpen, 2=detail

    // Fog usage mode
    int fogUsage = -1; // int: 0=none, 1=standard, 2=F3DAM RGB

    // Screen-space triangle (noperspective shade color)
    int screenSpaceTriangle = -1; // int: 0=perspective, 1=screen-space

    // Resolution scale
    int resolutionScale = -1; // vec2

    // Combiner debug output mode
    int debugOutput = -1; // int: 0=normal, 1=tex0, 2=shade, 3=prim, 4=env, 5=UV, 6=depth, 7=alpha

    // Texture-alpha coverage mask (RGBA16 sprites): multiply combined.a by texel0.a
    int texCvgAlpha = -1; // int: 0=off, 1=on

    // Texture-pack pictures: bit 0 = texture 0, bit 1 = texture 1 is sampled by n64ReadHiRes
    int texHiRes = -1; // int

    void Reset() { *this = ShaderUniforms(); }
};

// --- Shader Program Handle ---
using ShaderHandle = uint32_t;
static constexpr ShaderHandle INVALID_SHADER = 0;

// --- Compiled Shader Info ---
struct CompiledShader {
    ShaderHandle handle;
    CombinerKey key;
    ShaderUniforms uniforms;
    uint64_t features;
    uint32_t vertexShader; // Backend-specific sub-handles
    uint32_t fragmentShader;
    uint32_t geometryShader;
    std::string vertexSource;
    std::string fragmentSource;
    uint32_t compileTimeMs;
    bool valid;

    CompiledShader() :
        handle(INVALID_SHADER), features(0), vertexShader(0), fragmentShader(0), geometryShader(0), compileTimeMs(0),
        valid(false) {}
};

// --- Shader Source Code Templates ---
struct ShaderTemplate {
    std::string vertexHeader;
    std::string vertexBody;
    std::string fragmentHeader;
    std::string fragmentBody;
    std::string geometryHeader;
    std::string geometryBody;
    std::vector<std::string> defines;

    void AddDefine(const std::string& name, const std::string& value = "1") {
        defines.push_back("#define " + name + " " + value);
    }

    void AddDefineIf(bool condition, const std::string& name, const std::string& value = "1") {
        if (condition) defines.push_back("#define " + name + " " + value);
    }
};

// --- SHADER CACHE ---

struct ShaderCacheStats {
    uint32_t totalCompiled;
    uint32_t cacheHits;
    uint32_t cacheMisses;
    uint32_t compilationFailures;
    uint32_t totalCompileTimeMs;
    uint32_t currentCacheSize;
    uint32_t evictions;

    ShaderCacheStats() :
        totalCompiled(0), cacheHits(0), cacheMisses(0), compilationFailures(0), totalCompileTimeMs(0),
        currentCacheSize(0), evictions(0) {}
};

class ShaderCache {
public:
    using DeleteCallback = std::function<void(CompiledShader&)>;

    ShaderCache();
    ~ShaderCache();

    void Initialize(uint32_t maxShaders = 4096);
    void Shutdown();
    void Clear();

    /// Set a callback invoked before each shader is removed from the cache.
    /// The callback must delete the backend GPU program (e.g. glDeleteProgram).
    void SetDeleteCallback(DeleteCallback cb) { m_deleteCallback = std::move(cb); }

    // Look up a compiled shader by key. Returns nullptr if not found.
    const CompiledShader* Find(const CombinerKey& key) const;

    void Insert(const CombinerKey& key, CompiledShader&& shader);

    bool Remove(const CombinerKey& key);

    // Evicts the least recently used shaders.
    void EvictLRU(int count = 1);

    // Persistence: save/load shader cache to/from disk
    bool SaveToDisk(const std::string& path) const;
    bool LoadFromDisk(const std::string& path);

    // Save/load binary cache (pre-compiled shader binaries)
    bool SaveBinaryCache(const std::string& path) const;
    bool LoadBinaryCache(const std::string& path);

    const ShaderCacheStats& GetStats() const { return m_stats; }
    void AdvanceFrame() { m_currentFrame++; }

    /// Iterate all cache entries (under lock). Callback receives key + mutable shader.
    void ForEachEntry(std::function<void(const CombinerKey&, CompiledShader&)> callback);

private:
    struct CacheEntry {
        CompiledShader shader;
        uint64_t lastAccessFrame;
        uint32_t accessCount;
    };

    mutable std::shared_mutex m_mutex;
    std::unordered_map<CombinerKey, CacheEntry, CombinerKeyHasher> m_cache;
    uint32_t m_maxShaders;
    uint64_t m_currentFrame;
    ShaderCacheStats m_stats;
    DeleteCallback m_deleteCallback;
};

// --- SHADER COMPILER (Abstract Interface) ---

class IShaderCompiler {
public:
    virtual ~IShaderCompiler() = default;

    virtual bool Initialize(BackendAPI api) = 0;
    virtual void Shutdown() = 0;

    // On failure the returned shader has valid == false; see GetLastError().
    virtual CompiledShader Compile(const CombinerKey& key, const std::string& vertexSource,
                                   const std::string& fragmentSource) = 0;

    virtual CompiledShader CompileWithGeometry(const CombinerKey& key, const std::string& vertexSource,
                                               const std::string& fragmentSource,
                                               const std::string& geometrySource) = 0;

    virtual void DeleteShader(CompiledShader& shader) = 0;

    virtual void BindShader(const CompiledShader& shader) = 0;

    virtual std::string GetLastError() const = 0;

    virtual int GetUniformLocation(ShaderHandle program, const char* name) = 0;

    virtual void SetUniform1i(int location, int value) = 0;
    virtual void SetUniform1f(int location, float value) = 0;
    virtual void SetUniform2f(int location, float x, float y) = 0;
    virtual void SetUniform3f(int location, float x, float y, float z) = 0;
    virtual void SetUniform4f(int location, float r, float g, float b, float a) = 0;
    virtual void SetUniformMatrix4fv(int location, const float* matrix) = 0;

    virtual BackendAPI GetAPI() const = 0;

    virtual ShaderLanguage GetLanguage() const = 0;
};

// --- SHADER GENERATOR ---

class ShaderGenerator {
public:
    ShaderGenerator();
    ~ShaderGenerator();

    void Initialize(ShaderLanguage language);
    void Shutdown();

    ShaderTemplate GenerateShader(const CombinerKey& key) const;

    std::string GenerateVertexShader(uint64_t features) const;

    // The fragment shader carries the combiner logic.
    std::string GenerateFragmentShader(const CombinerKey& key) const;

    // For wireframe debug or point sprite expansion.
    std::string GenerateGeometryShader(uint64_t features) const;

    // For the shadow/depth pass.
    std::string GenerateDepthOnlyVertex() const;
    std::string GenerateDepthOnlyFragment() const;

    std::string GeneratePostProcessVertex() const;
    std::string GeneratePostProcessFragment(uint64_t features) const;

    // For debug rendering overlays.
    std::string GenerateUIVertex() const;
    std::string GenerateUIFragment() const;

    ShaderLanguage GetLanguage() const { return m_language; }

private:
    ShaderLanguage m_language;

    // Internal source generation helpers
    std::string GetVersionDirective() const;
    std::string GetCommonUniforms() const;
    std::string GetTextureUniforms() const;
    std::string GetTextureUniformsVK() const; // Vulkan: textures at binding 1+
    std::string GetVertexInputs() const;
    std::string GetVertexOutputs() const;
    std::string GetFragmentInputs() const;
    std::string GetFragmentOutputs(uint64_t features = 0) const;
    std::string GetHLSLCompatPreamble() const;

    // Combiner code generation
    std::string GenerateCombinerCode(uint64_t mux, uint64_t features) const;
    std::string GenerateBlenderCode(uint32_t blenderBits, uint32_t otherModeL, uint64_t features) const;
    std::string GenerateAlphaTestCode(uint64_t features) const;
    std::string GenerateFogCode(uint64_t features) const;
    std::string GenerateNoiseCode(uint64_t features) const;
    std::string GenerateTextureSampleCode(uint64_t features) const;
    std::string GenerateN64TexFilterCode() const;
    std::string GenerateN64TexelReadCode() const; // 3-point filter
    std::string GenerateDepthCompareCode(uint64_t features) const;
    std::string GenerateDepthRenderCode(uint64_t features) const;
    std::string GeneratePrimDepthCode(uint64_t features) const;
    std::string GenerateCoverageCode(uint64_t features) const;
    std::string GenerateDitherCode(uint64_t features, uint32_t otherModeH) const;
    std::string GenerateMonochromeCode(uint64_t features) const;
    std::string GenerateGammaCorrectionCode() const;
    std::string GenerateYUVConversionCode(uint64_t features) const;
    std::string GenerateLODCode(uint64_t features) const;
    std::string GenerateDetailSharpenCode(uint64_t features) const;
    std::string GenerateShadowMapShader() const;
};

// --- SHADER MANAGER (Top-level orchestrator) ---

class ShaderManager {
public:
    ShaderManager();
    ~ShaderManager();

    bool Initialize(BackendAPI api);
    void Shutdown();

    // Get or compile a shader for the given combiner state
    const CompiledShader* GetShader(const CombinerKey& key);

    // Bind a shader and set up its uniforms
    void BindShader(const CompiledShader* shader);

    // Update uniforms for the currently bound shader
    void SetMatrixUniforms(const float* mvp, const float* modelView = nullptr);
    void SetColorUniforms(const float* primColor, const float* envColor, const float* fogColor, const float* blendColor,
                          float primLODFrac, float primLODMin, float centerR = 0.0f, float centerG = 0.0f,
                          float centerB = 0.0f, float scaleR = 0.0f, float scaleG = 0.0f, float scaleB = 0.0f);
    void SetFillColor(float r, float g, float b, float a);
    void SetTextureUniforms(int tex0Unit, int tex1Unit, const float* scale0, const float* scale1, const float* size0,
                            const float* size1, const float* gpuScale0, const float* gpuScale1);
    void SetGPUScale(const float* gpuScale0, const float* gpuScale1);
    void SetTextureTileParams(const float* offset0, const float* offset1, const float* clamp0, const float* clamp1,
                              const float* mask0, const float* mask1);
    void SetAlphaRef(float ref);
    void SetFogParams(float multiplier, float offset);
    void SetTexCoordBounds(bool enabled, const float* bounds0, const float* bounds1);
    void SetDepthParams(float scale, float offset, float primDepth);
    void SetN64DepthMode(int zMode, float primDeltaZ);
    void SetConvertK(const int16_t convertK[6]);
    void SetScreenSize(float width, float height);
    void SetFrameCount(uint32_t frame);
    void SetResolutionScale(float scaleX, float scaleY);
    void SetTexturePersp(int persp);
    void SetFBTextureParams(int mono0, int mono1, int fixedAlpha0, int fixedAlpha1);
    void SetScreenScale(float scaleX, float scaleY);
    void SetKeyWidth(float r, float g, float b);
    void SetLODParams(float minLod, int maxTile, int textureDetail);
    void SetFogUsage(int fogUsage);
    void SetScreenSpaceTriangle(int screenSpace);
    void SetDebugOutput(int mode);
    void SetTexCvgAlpha(int v);
    void SetTexHiRes(int bits);

    // Built-in shader programs
    const CompiledShader* GetDepthOnlyShader();
    const CompiledShader* GetPostProcessShader(uint64_t features);
    const CompiledShader* GetUIShader();

    // Cache management
    void ClearCache();
    bool SaveCache(const std::string& path);
    bool LoadCache(const std::string& path);

    const ShaderCacheStats& GetStats() const;
    void ResetStats();

    std::string DumpShaderSource(const CombinerKey& key) const;

    void OnFrameBegin();
    void OnFrameEnd();

    // Set the backend shader compiler (must be called after Initialize)
    void SetCompiler(std::unique_ptr<IShaderCompiler> compiler) {
        m_compiler = std::move(compiler);
        m_cache.SetDeleteCallback([this](CompiledShader& shader) {
            if (m_compiler) m_compiler->DeleteShader(shader);
        });
    }

private:
    std::unique_ptr<IShaderCompiler> m_compiler;
    std::unique_ptr<ShaderGenerator> m_generator;
    ShaderCache m_cache;

    const CompiledShader* m_currentShader;
    BackendAPI m_api;
    bool m_initialized;

    // Built-in shaders
    std::unique_ptr<CompiledShader> m_depthOnlyShader;
    std::unique_ptr<CompiledShader> m_uiShader;
    std::unique_ptr<CompiledShader> m_fallbackShader;
    std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> m_postProcessShaders;

    const CompiledShader* CompileAndCache(const CombinerKey& key);
    const CompiledShader* GetFallbackShader();
    void QueryUniformLocations(CompiledShader& shader);
};

} // namespace Shader
} // namespace Video
} // namespace Nova64
