/**
 * @file Renderer.hpp
 * @brief Renderer Abstraction Layer.
 */
// SPDX-License-Identifier: MIT
// NovaGraphics - Renderer Abstraction Layer
// Backend-agnostic rendering interface that all backends implement

#pragma once

#include "../Core/N64Types.hpp"
#include "../Common/Memory.hpp"
#include "ShaderManager.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <array>

namespace Nova64 { namespace Video {

// ═══════════════════════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ═══════════════════════════════════════════════════════════════════════════════

class IRenderer;
class IRenderTarget;
class IVertexBuffer;
class IIndexBuffer;
class IUniformBuffer;
class ITexture;
class IShaderProgram;
class IPipeline;

// ═══════════════════════════════════════════════════════════════════════════════
//  ENUMERATIONS
// ═══════════════════════════════════════════════════════════════════════════════

enum class BackendType : uint8_t {
    None = 0,
    OpenGL46,       // OpenGL 4.6 Core Profile + DSA
    D3D11,          // Direct3D 11 (Windows)
    Software,       // Software fallback (debug only)
    Vulkan,         // Vulkan 1.2+ (stub — not yet implemented)
    Count
};

/// Internal format for image load/store binding (BindImageForRW).
enum class ImageBindFormat : uint8_t {
    R32F = 0,       // Z-image, deltaZ-image
    RGBA8,          // Color image
    R8UI            // Coverage image
};

enum class PrimitiveType : uint8_t {
    Triangles = 0,
    TriangleStrip,
    TriangleFan,
    Lines,
    LineStrip,
    Points
};

enum class BlendFactor : uint8_t {
    Zero = 0,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantColor,
    OneMinusConstantColor,
    ConstantAlpha,
    OneMinusConstantAlpha,
    SrcAlphaSaturate,
    Src1Color,
    OneMinusSrc1Color,
    Src1Alpha,
    OneMinusSrc1Alpha
};

enum class BlendOp : uint8_t {
    Add = 0,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

enum class CompareFunc : uint8_t {
    Never = 0,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class StencilOp : uint8_t {
    Keep = 0,
    Zero,
    Replace,
    IncrClamp,
    DecrClamp,
    Invert,
    IncrWrap,
    DecrWrap
};

enum class CullMode : uint8_t {
    None = 0,
    Front,
    Back,
    FrontAndBack
};

enum class FrontFace : uint8_t {
    CounterClockwise = 0,
    Clockwise
};

enum class PolygonMode : uint8_t {
    Fill = 0,
    Wireframe,
    Points
};

enum class TextureFormat : uint8_t {
    RGBA8 = 0,
    RGB8,
    RGBA4,
    RGB5A1,
    R8,
    RG8,
    RGBA16F,
    RGBA32F,
    Depth16,
    Depth24,
    Depth32F,
    Depth24Stencil8,
    IA16,     // N64 intensity+alpha
    I8,       // N64 intensity
    CI4,      // N64 color-indexed 4-bit
    CI8,      // N64 color-indexed 8-bit
    Count
};

enum class SamplerFilter : uint8_t {
    Nearest = 0,
    Linear,
    NearestMipNearest,
    LinearMipNearest,
    NearestMipLinear,
    LinearMipLinear,
    N64_3Point          // N64 hardware 3-point bilinear
};

enum class TextureWrap : uint8_t {
    Repeat = 0,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
    MirrorClampToEdge
};

enum class BufferUsage : uint8_t {
    Static = 0,     // Uploaded once
    Dynamic,        // Updated frequently
    Stream          // Updated every frame
};

enum class IndexType : uint8_t {
    UInt16 = 0,
    UInt32
};

// ═══════════════════════════════════════════════════════════════════════════════
//  VERTEX FORMAT
// ═══════════════════════════════════════════════════════════════════════════════

enum class VertexAttribType : uint8_t {
    Float1 = 0,
    Float2,
    Float3,
    Float4,
    UByte4,
    UByte4N,        // Normalized [0,1]
    Short2,
    Short4,
    Short2N,
    Short4N,
    UInt1,
    Int1
};

struct VertexAttrib {
    uint32_t location;      // Shader attribute location
    VertexAttribType type;  // Data type
    uint32_t offset;        // Byte offset in vertex struct
    bool normalized;        // Integer → float normalize
};

struct VertexLayout {
    std::vector<VertexAttrib> attribs;
    uint32_t stride;        // Bytes per vertex

    static VertexLayout CreateN64() {
        VertexLayout layout;
        // Must match GPUVertex in RDPProcessor.cpp (not RenderBatch::Vertex):
        //   float x, y, z, w;     // offset  0  (16 bytes) - position
        //   float r, g, b, a;     // offset 16  (16 bytes) - color
        //   float s0, t0;         // offset 32  ( 8 bytes) - texcoord0
        //   float s1, t1;         // offset 40  ( 8 bytes) - texcoord1
        //   float fog;            // offset 48  ( 4 bytes) - fog
        //   float perspW;         // offset 52  ( 4 bytes) - 1/clip_W
        //   float baryS, baryT;   // offset 56  ( 8 bytes) - barycentric for coverage AA
        //                         // total = 64 bytes
        layout.stride = 64;
        // position: location 0, float4
        layout.attribs.push_back({0, VertexAttribType::Float4, 0, false});
        // color: location 1, float4
        layout.attribs.push_back({1, VertexAttribType::Float4, 16, false});
        // texCoord0: location 2, float2
        layout.attribs.push_back({2, VertexAttribType::Float2, 32, false});
        // texCoord1: location 3, float2
        layout.attribs.push_back({3, VertexAttribType::Float2, 40, false});
        // fog: location 4, float1
        layout.attribs.push_back({4, VertexAttribType::Float1, 48, false});
        // perspW (1/clip_W): location 5, float1 — for perspective-correct texture interpolation
        layout.attribs.push_back({5, VertexAttribType::Float1, 52, false});
        // baryCoord: location 6, float2 — barycentric coords for sub-pixel coverage AA
        layout.attribs.push_back({6, VertexAttribType::Float2, 56, false});
        return layout;
    }

    static VertexLayout CreateUI() {
        VertexLayout layout;
        layout.stride = sizeof(float) * 2 + sizeof(float) * 2 + sizeof(float) * 4;
        layout.attribs.push_back({0, VertexAttribType::Float2, 0, false});
        layout.attribs.push_back({1, VertexAttribType::Float2, 8, false});
        layout.attribs.push_back({2, VertexAttribType::Float4, 16, false});
        return layout;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  RENDER STATE DESCRIPTORS
// ═══════════════════════════════════════════════════════════════════════════════

struct BlendState {
    bool enabled = false;
    BlendFactor srcColor = BlendFactor::SrcAlpha;
    BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::OneMinusSrcAlpha;
    BlendOp alphaOp = BlendOp::Add;
    float blendColor[4] = {0, 0, 0, 0};
    uint8_t colorWriteMask = 0x0F; // RGBA

    static BlendState Opaque() { return BlendState{}; }
    static BlendState AlphaBlend() {
        BlendState s;
        s.enabled = true;
        return s;
    }
    static BlendState Additive() {
        BlendState s;
        s.enabled = true;
        s.srcColor = BlendFactor::SrcAlpha;
        s.dstColor = BlendFactor::One;
        s.srcAlpha = BlendFactor::One;
        s.dstAlpha = BlendFactor::One;
        return s;
    }
    static BlendState Premultiplied() {
        BlendState s;
        s.enabled = true;
        s.srcColor = BlendFactor::One;
        s.dstColor = BlendFactor::OneMinusSrcAlpha;
        return s;
    }
};

struct DepthStencilState {
    bool depthTestEnabled = true;
    bool depthWriteEnabled = true;
    CompareFunc depthFunc = CompareFunc::LessEqual;

    bool stencilEnabled = false;
    uint8_t stencilReadMask = 0xFF;
    uint8_t stencilWriteMask = 0xFF;
    uint8_t stencilRef = 0;
    CompareFunc stencilFunc = CompareFunc::Always;
    StencilOp stencilFail = StencilOp::Keep;
    StencilOp depthFail = StencilOp::Keep;
    StencilOp stencilPass = StencilOp::Keep;

    static DepthStencilState Default() { return DepthStencilState{}; }
    static DepthStencilState NoDepth() {
        DepthStencilState s;
        s.depthTestEnabled = false;
        s.depthWriteEnabled = false;
        return s;
    }
    static DepthStencilState DepthReadOnly() {
        DepthStencilState s;
        s.depthWriteEnabled = false;
        return s;
    }
};

struct RasterizerState {
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::CounterClockwise;
    PolygonMode polygonMode = PolygonMode::Fill;
    bool scissorEnabled = true;
    bool depthClipEnabled = true;
    float depthBias = 0.0f;
    float depthBiasSlope = 0.0f;
    bool multisampleEnabled = false;

    static RasterizerState Default() { return RasterizerState{}; }
    static RasterizerState NoCull() {
        RasterizerState s;
        s.cullMode = CullMode::None;
        return s;
    }
    static RasterizerState Wireframe() {
        RasterizerState s;
        s.polygonMode = PolygonMode::Wireframe;
        s.cullMode = CullMode::None;
        return s;
    }
};

struct RenderViewport {
    float x = 0, y = 0;
    float width = 0, height = 0;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

struct RenderScissorRect {
    int32_t x = 0, y = 0;
    int32_t width = 0, height = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  SAMPLER DESCRIPTOR
// ═══════════════════════════════════════════════════════════════════════════════

struct SamplerDesc {
    SamplerFilter minFilter = SamplerFilter::Linear;
    SamplerFilter magFilter = SamplerFilter::Linear;
    TextureWrap wrapS = TextureWrap::Repeat;
    TextureWrap wrapT = TextureWrap::Repeat;
    float maxAnisotropy = 1.0f;
    float lodBias       = 0.0f;
    float borderColor[4] = {0, 0, 0, 0};
    bool useN64_3Point = false;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  TEXTURE DESCRIPTOR
// ═══════════════════════════════════════════════════════════════════════════════

struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    TextureFormat format = TextureFormat::RGBA8;
    BufferUsage usage = BufferUsage::Static;
    bool isRenderTarget = false;
    bool isDepthStencil = false;
    const char* debugName = nullptr;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  RENDER TARGET DESCRIPTOR
// ═══════════════════════════════════════════════════════════════════════════════

struct RenderTargetDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    TextureFormat colorFormat = TextureFormat::RGBA8;
    TextureFormat depthFormat = TextureFormat::Depth24Stencil8;
    uint32_t samples = 1; // MSAA sample count
    bool hasDepth = true;
    const char* debugName = nullptr;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  PIPELINE DESCRIPTOR
// ═══════════════════════════════════════════════════════════════════════════════

struct PipelineDesc {
    IShaderProgram* shader = nullptr;
    VertexLayout vertexLayout;
    BlendState blend;
    DepthStencilState depthStencil;
    RasterizerState rasterizer;
    PrimitiveType primitiveType = PrimitiveType::Triangles;
    TextureFormat colorFormat = TextureFormat::RGBA8;
    TextureFormat depthFormat = TextureFormat::Depth24Stencil8;
    const char* debugName = nullptr;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  ABSTRACT RESOURCE INTERFACES
// ═══════════════════════════════════════════════════════════════════════════════

class IVertexBuffer {
public:
    virtual ~IVertexBuffer() = default;
    virtual void Upload(const void* data, size_t sizeBytes) = 0;
    virtual void Update(const void* data, size_t offset, size_t sizeBytes) = 0;
    virtual size_t GetSize() const = 0;
    virtual BufferUsage GetUsage() const = 0;
};

class IIndexBuffer {
public:
    virtual ~IIndexBuffer() = default;
    virtual void Upload(const void* data, size_t sizeBytes, IndexType type) = 0;
    virtual void Update(const void* data, size_t offset, size_t sizeBytes) = 0;
    virtual size_t GetCount() const = 0;
    virtual IndexType GetIndexType() const = 0;
};

class IUniformBuffer {
public:
    virtual ~IUniformBuffer() = default;
    virtual void Upload(const void* data, size_t sizeBytes) = 0;
    virtual void Update(const void* data, size_t offset, size_t sizeBytes) = 0;
    virtual void BindSlot(uint32_t slot) = 0;
    virtual size_t GetSize() const = 0;
};

class ITexture {
public:
    virtual ~ITexture() = default;
    virtual void Upload(const void* data, size_t sizeBytes, uint32_t mipLevel = 0) = 0;
    virtual void UploadSubRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  const void* data, size_t sizeBytes, uint32_t mipLevel = 0) = 0;
    virtual void GenerateMipmaps() = 0;
    virtual void SetSampler(const SamplerDesc& desc) = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual TextureFormat GetFormat() const = 0;
    virtual uint64_t GetNativeHandle() const = 0;
};

class IShaderProgram {
public:
    virtual ~IShaderProgram() = default;
    virtual bool Compile(const std::string& vertexSrc, const std::string& fragmentSrc,
                          const std::string& geometrySrc = "") = 0;
    virtual bool CompileCompute(const std::string& computeSrc) = 0;
    virtual void Bind() = 0;
    virtual void Unbind() = 0;

    // Uniform setters
    virtual void SetInt(const char* name, int value) = 0;
    virtual void SetFloat(const char* name, float value) = 0;
    virtual void SetVec2(const char* name, float x, float y) = 0;
    virtual void SetVec3(const char* name, float x, float y, float z) = 0;
    virtual void SetVec4(const char* name, float x, float y, float z, float w) = 0;
    virtual void SetMat4(const char* name, const float* data) = 0;

    virtual int GetUniformLocation(const char* name) const = 0;
    virtual uint64_t GetNativeHandle() const = 0;
    virtual const std::string& GetInfoLog() const = 0;
};

class IRenderTarget {
public:
    virtual ~IRenderTarget() = default;
    virtual void Bind() = 0;
    virtual void Unbind() = 0;
    virtual void Clear(float r, float g, float b, float a, float depth = 1.0f, uint8_t stencil = 0) = 0;
    virtual void ClearColor(float r, float g, float b, float a) = 0;
    virtual void ClearDepthStencil(float depth, uint8_t stencil) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual ITexture* GetColorTexture() = 0;
    virtual ITexture* GetDepthTexture() = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual uint64_t GetNativeHandle() const = 0;
};

class IPipeline {
public:
    virtual ~IPipeline() = default;
    virtual void Bind() = 0;
    virtual uint64_t GetNativeHandle() const = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  DRAW COMMAND
// ═══════════════════════════════════════════════════════════════════════════════

struct DrawCommand {
    PrimitiveType primitive = PrimitiveType::Triangles;
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t instanceCount = 1;
    bool indexed = false;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  RENDERER STATISTICS
// ═══════════════════════════════════════════════════════════════════════════════

struct RendererStats {
    uint64_t drawCalls = 0;
    uint64_t triangles = 0;
    uint64_t stateChanges = 0;
    uint64_t textureUploads = 0;
    uint64_t shaderSwaps = 0;
    uint64_t bufferUploads = 0;
    uint64_t pipelineBinds = 0;
    double gpuTimeMs = 0.0;
    double cpuTimeMs = 0.0;
    uint64_t videoMemoryUsed = 0;      // bytes
    uint64_t videoMemoryAvailable = 0;  // bytes

    void Reset() {
        drawCalls = triangles = stateChanges = 0;
        textureUploads = shaderSwaps = bufferUploads = pipelineBinds = 0;
        gpuTimeMs = cpuTimeMs = 0.0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  RENDERER CAPABILITIES
// ═══════════════════════════════════════════════════════════════════════════════

struct RendererCaps {
    BackendType backend = BackendType::None;
    std::string deviceName;
    std::string driverVersion;
    std::string apiVersion;

    uint32_t maxTextureSize = 0;
    uint32_t maxTextureLayers = 0;
    uint32_t maxTextureUnits = 0;
    uint32_t maxUniformBlockSize = 0;
    uint32_t maxVertexAttribs = 0;
    uint32_t maxColorAttachments = 0;
    uint32_t maxComputeWorkGroupCount[3] = {};
    uint32_t maxComputeWorkGroupSize[3] = {};

    bool supportsCompute = false;
    bool supportsGeometryShaders = false;
    bool supportsTessellation = false;
    bool supportsAnisotropicFiltering = false;
    float maxAnisotropy = 1.0f;
    bool supportsMultisample = false;
    uint32_t maxSamples = 1;
    bool supportsIndirectDraw = false;
    bool supportsSSBO = false;
    bool supportsTimestampQueries = false;
    bool supportsDebugMarkers = false;
    bool supportsBindlessTextures = false;
    bool supportsDSA = false;
    bool supportsClipControl = false;
    bool supportsFragmentShaderInterlock = false;
    bool isIntelGPU = false;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  MAIN RENDERER INTERFACE
// ═══════════════════════════════════════════════════════════════════════════════

class IRenderer {
public:
    virtual ~IRenderer() = default;

    // ─── Lifecycle ─────────────────────────────────────────────
    virtual bool Initialize(void* windowHandle, uint32_t width, uint32_t height) = 0;
    virtual void Shutdown() = 0;
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Present() = 0;

    // ─── Resource Creation ─────────────────────────────────────
    virtual std::unique_ptr<IVertexBuffer> CreateVertexBuffer(size_t sizeBytes, BufferUsage usage) = 0;
    virtual std::unique_ptr<IIndexBuffer> CreateIndexBuffer(size_t sizeBytes, BufferUsage usage) = 0;
    virtual std::unique_ptr<IUniformBuffer> CreateUniformBuffer(size_t sizeBytes) = 0;
    virtual std::unique_ptr<ITexture> CreateTexture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<IShaderProgram> CreateShaderProgram() = 0;
    virtual std::unique_ptr<IRenderTarget> CreateRenderTarget(const RenderTargetDesc& desc) = 0;
    virtual std::unique_ptr<IPipeline> CreatePipeline(const PipelineDesc& desc) = 0;

    // ─── State Management ──────────────────────────────────────
    virtual void SetViewport(const RenderViewport& vp) = 0;
    virtual void SetScissor(const RenderScissorRect& rect) = 0;
    virtual void SetBlendState(const BlendState& state) = 0;
    virtual void SetDepthStencilState(const DepthStencilState& state) = 0;
    virtual void SetRasterizerState(const RasterizerState& state) = 0;
    virtual void SetBlendColor(float r, float g, float b, float a) = 0;

    // ─── Binding ───────────────────────────────────────────────
    virtual void BindVertexBuffer(IVertexBuffer* vb, const VertexLayout& layout) = 0;
    virtual void BindIndexBuffer(IIndexBuffer* ib) = 0;
    virtual void BindTexture(uint32_t unit, ITexture* tex) = 0;
    virtual void BindShader(IShaderProgram* shader) = 0;
    virtual void BindPipeline(IPipeline* pipeline) = 0;
    virtual void BindRenderTarget(IRenderTarget* rt) = 0; // nullptr = default backbuffer

    // ─── Image Load/Store (MLE blender, N64 Z-image) ──────────
    /// Bind a native texture for pixel-shader read-write (imageLoad/imageStore on GL,
    /// RasterizerOrderedTexture on D3D11.3).
    /// @param unit   Image unit (0=Z, 1=deltaZ, 2=colorImage, 3=coverage)
    /// @param handle Backend-native handle (GL: GLuint texture name, D3D: ID3D11UnorderedAccessView*)
    /// @param format Internal pixel format of the image
    virtual void BindImageForRW(uint32_t unit, uint64_t handle, ImageBindFormat format) = 0;
    /// Unbind all image units.
    virtual void UnbindAllImages() = 0;
    /// Control per-channel color write mask (equivalent to glColorMask / D3D blend write mask).
    virtual void SetColorWriteMask(bool r, bool g, bool b, bool a) = 0;
    /// Issue an image-access memory barrier (GL: glMemoryBarrier, D3D: no-op with ROV).
    virtual void ImageMemoryBarrier() = 0;

    // ─── Drawing ───────────────────────────────────────────────
    virtual void Draw(const DrawCommand& cmd) = 0;
    virtual void DrawArrays(PrimitiveType prim, uint32_t first, uint32_t count) = 0;
    virtual void DrawIndexed(PrimitiveType prim, uint32_t indexCount, IndexType type, uint32_t offset) = 0;

    // ─── Compute ───────────────────────────────────────────────
    virtual void DispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;

    // ─── Readback ──────────────────────────────────────────────
    virtual void ReadPixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             TextureFormat format, void* outData) = 0;

    // ─── Debug ─────────────────────────────────────────────────
    virtual void PushDebugGroup(const char* name) = 0;
    virtual void PopDebugGroup() = 0;
    virtual void InsertDebugMarker(const char* name) = 0;

    // ─── Queries ───────────────────────────────────────────────
    virtual BackendType GetBackendType() const = 0;
    virtual const RendererCaps& GetCapabilities() const = 0;
    virtual const RendererStats& GetStats() const = 0;
    virtual void ResetStats() = 0;

    // ─── Window ────────────────────────────────────────────────
    virtual void OnResize(uint32_t width, uint32_t height) = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════
//  RENDERER FACTORY
// ═══════════════════════════════════════════════════════════════════════════════

/// Create a renderer of the specified backend type
std::unique_ptr<IRenderer> CreateRenderer(BackendType type);

/// Auto-detect the best available backend
BackendType DetectBestBackend();

/// Get human-readable name for a backend
const char* GetBackendName(BackendType type);

}} // namespace Nova64::Video
