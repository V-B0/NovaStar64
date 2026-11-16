/**
 * @file GLShaderProgram.hpp
 * @brief OpenGL Shader Program Management.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <memory>

namespace Nova64::Video::GL {

// --- Shader Stage ---

enum class ShaderStage : uint8_t { Vertex, Fragment, Geometry, TessControl, TessEval, Compute };

uint32_t ShaderStageToGL(ShaderStage stage);
const char* ShaderStageName(ShaderStage stage);

// --- Shader Object — individual compiled shader ---

class GLShader {
public:
    GLShader() = default;
    ~GLShader();

    GLShader(const GLShader&) = delete;
    GLShader& operator=(const GLShader&) = delete;
    GLShader(GLShader&& other) noexcept;
    GLShader& operator=(GLShader&& other) noexcept;

    bool CompileFromSource(ShaderStage stage, const std::string& source);
    bool CompileFromSPIRV(ShaderStage stage, const std::vector<uint32_t>& spirv, const char* entry = "main");
    void Destroy();

    uint32_t Handle() const { return m_handle; }
    ShaderStage Stage() const { return m_stage; }
    bool IsValid() const { return m_handle != 0; }
    const std::string& GetInfoLog() const { return m_infoLog; }
    const std::string& GetSource() const { return m_source; }

    void SetLabel(const std::string& label);

private:
    uint32_t m_handle = 0;
    ShaderStage m_stage = ShaderStage::Vertex;
    std::string m_infoLog;
    std::string m_source;
};

// --- Uniform Info ---

enum class UniformType : uint8_t {
    Float,
    Vec2,
    Vec3,
    Vec4,
    Int,
    IVec2,
    IVec3,
    IVec4,
    UInt,
    UVec2,
    UVec3,
    UVec4,
    Mat2,
    Mat3,
    Mat4,
    Mat2x3,
    Mat3x4,
    Mat4x3,
    Sampler1D,
    Sampler2D,
    Sampler3D,
    SamplerCube,
    Sampler2DArray,
    SamplerCubeShadow,
    Sampler2DShadow,
    Image2D,
    Image3D,
    Bool,
    AtomicCounter
};

struct UniformInfo {
    std::string name;
    int32_t location = -1;
    UniformType type = UniformType::Float;
    int32_t arraySize = 1;
    int32_t offset = -1;     // in UBO
    int32_t blockIndex = -1; // owning UBO index (-1 = default)
};

// --- Uniform Block Info ---

struct UniformBlockInfo {
    std::string name;
    uint32_t index = 0;
    uint32_t binding = 0;
    uint32_t size = 0;
    std::vector<UniformInfo> members;
};

// --- Attribute Info ---

struct AttributeInfo {
    std::string name;
    int32_t location = -1;
    uint32_t type = 0; // GL type
    int32_t size = 1;  // array size
};

// --- Shader Storage Block Info ---

struct SSBOInfo {
    std::string name;
    uint32_t index = 0;
    uint32_t binding = 0;
    uint32_t size = 0; // minimum buffer size
    bool isArray = false;
};

// --- Program Reflection ---

struct ProgramReflection {
    std::vector<UniformInfo> uniforms;
    std::vector<UniformBlockInfo> uniformBlocks;
    std::vector<AttributeInfo> attributes;
    std::vector<SSBOInfo> ssbos;
    uint32_t computeWorkGroupSize[3] = {0, 0, 0};
};

// --- GLProgram — linked shader program ---

class GLProgram {
public:
    GLProgram() = default;
    ~GLProgram();

    GLProgram(const GLProgram&) = delete;
    GLProgram& operator=(const GLProgram&) = delete;
    GLProgram(GLProgram&& other) noexcept;
    GLProgram& operator=(GLProgram&& other) noexcept;

    // Build from individual shaders
    bool Link(const GLShader& vertex, const GLShader& fragment);
    bool Link(const GLShader& vertex, const GLShader& geometry, const GLShader& fragment);
    bool LinkCompute(const GLShader& compute);
    bool LinkFromStages(const std::vector<const GLShader*>& shaders);

    // Build from source (compile + link in one call)
    bool BuildFromSource(const std::string& vertSrc, const std::string& fragSrc);
    bool BuildComputeFromSource(const std::string& src);

    // Build with #define injection
    bool BuildFromSourceWithDefines(const std::string& vertSrc, const std::string& fragSrc,
                                    const std::vector<std::pair<std::string, std::string>>& defines);

    void Destroy();

    void Use() const;
    static void UseNone();

    uint32_t Handle() const { return m_handle; }
    bool IsValid() const { return m_handle != 0; }
    const std::string& GetInfoLog() const { return m_infoLog; }

    const ProgramReflection& GetReflection() const { return m_reflection; }
    void Reflect();

    // Uniform queries
    int32_t GetUniformLocation(const std::string& name) const;
    int32_t GetUniformLocation(const char* name) const;
    int32_t GetUniformBlockIndex(const std::string& name) const;
    int32_t GetSSBOIndex(const std::string& name) const;
    int32_t GetAttribLocation(const std::string& name) const;

    // Uniform setters (by location — fast)
    void SetFloat(int32_t loc, float v) const;
    void SetVec2(int32_t loc, float x, float y) const;
    void SetVec3(int32_t loc, float x, float y, float z) const;
    void SetVec4(int32_t loc, float x, float y, float z, float w) const;
    void SetVec4(int32_t loc, const float* v) const;
    void SetInt(int32_t loc, int32_t v) const;
    void SetUInt(int32_t loc, uint32_t v) const;
    void SetIVec2(int32_t loc, int32_t x, int32_t y) const;
    void SetIVec3(int32_t loc, int32_t x, int32_t y, int32_t z) const;
    void SetIVec4(int32_t loc, int32_t x, int32_t y, int32_t z, int32_t w) const;
    void SetBool(int32_t loc, bool v) const;
    void SetMat3(int32_t loc, const float* m, bool transpose = false) const;
    void SetMat4(int32_t loc, const float* m, bool transpose = false) const;
    void SetMat4Array(int32_t loc, const float* m, int count, bool transpose = false) const;
    void SetFloatArray(int32_t loc, const float* v, int count) const;
    void SetIntArray(int32_t loc, const int32_t* v, int count) const;

    // Uniform setters (by name — slower, uses cache)
    void SetFloat(const std::string& name, float v) const;
    void SetVec2(const std::string& name, float x, float y) const;
    void SetVec3(const std::string& name, float x, float y, float z) const;
    void SetVec4(const std::string& name, float x, float y, float z, float w) const;
    void SetInt(const std::string& name, int32_t v) const;
    void SetBool(const std::string& name, bool v) const;
    void SetMat4(const std::string& name, const float* m, bool transpose = false) const;

    // Block binding
    void BindUniformBlock(uint32_t blockIndex, uint32_t bindingPoint) const;
    void BindUniformBlock(const std::string& name, uint32_t bindingPoint) const;
    void BindSSBO(uint32_t index, uint32_t bindingPoint) const;
    void BindSSBO(const std::string& name, uint32_t bindingPoint) const;

    // Compute dispatch helpers
    void Dispatch(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) const;
    void DispatchForSize(uint32_t sizeX, uint32_t sizeY = 1, uint32_t sizeZ = 1) const;
    void MemoryBarrier(uint32_t barriers) const;

    // Binary cache
    bool GetBinary(std::vector<uint8_t>& outData, uint32_t& outFormat) const;
    bool LoadBinary(const std::vector<uint8_t>& data, uint32_t format);

    void SetLabel(const std::string& label);

    std::string DumpUniforms() const;
    std::string DumpAttributes() const;
    std::string DumpBlocks() const;

private:
    bool CheckLinkStatus();
    void BuildUniformCache();

    uint32_t m_handle = 0;
    std::string m_infoLog;
    ProgramReflection m_reflection;

    mutable std::unordered_map<std::string, int32_t> m_uniformCache;
};

// --- Separable Pipeline (GL_ARB_separate_shader_objects) ---

class GLProgramPipeline {
public:
    GLProgramPipeline() = default;
    ~GLProgramPipeline();

    GLProgramPipeline(const GLProgramPipeline&) = delete;
    GLProgramPipeline& operator=(const GLProgramPipeline&) = delete;
    GLProgramPipeline(GLProgramPipeline&& other) noexcept;
    GLProgramPipeline& operator=(GLProgramPipeline&& other) noexcept;

    bool Create();
    void Destroy();

    void SetVertexStage(uint32_t program);
    void SetFragmentStage(uint32_t program);
    void SetGeometryStage(uint32_t program);
    void SetComputeStage(uint32_t program);

    void Bind() const;
    static void Unbind();
    bool Validate() const;

    uint32_t Handle() const { return m_handle; }
    bool IsValid() const { return m_handle != 0; }

    void SetLabel(const std::string& label);

private:
    uint32_t m_handle = 0;
};

// --- Shader Include Manager (GL_ARB_shading_language_include) ---

class ShaderIncludeManager {
public:
    void RegisterInclude(const std::string& path, const std::string& source);
    void UnregisterInclude(const std::string& path);
    void ClearAll();
    bool HasInclude(const std::string& path) const;
    const std::string& GetInclude(const std::string& path) const;

    // Expands #include directives from the registered sources in software, without GL_ARB_shading_language_include.
    std::string PreprocessIncludes(const std::string& source, int maxDepth = 16) const;

private:
    std::unordered_map<std::string, std::string> m_includes;
    std::string m_empty;
    std::string ResolveIncludes(const std::string& src, int depth, int maxDepth) const;
};

// --- Shader Preprocessor — #define injection & version management ---

class ShaderPreprocessor {
public:
    void SetVersion(int version, const std::string& profile = "core");
    void AddDefine(const std::string& name, const std::string& value = "");
    void AddDefine(const std::string& name, int value);
    void AddDefine(const std::string& name, float value);
    void RemoveDefine(const std::string& name);
    void ClearDefines();

    void SetIncludeManager(const ShaderIncludeManager* mgr) { m_includes = mgr; }

    std::string Process(const std::string& source) const;

    // Common define sets
    void SetN64Defines(bool twoPoint, bool fog, bool noise, bool lod);
    void SetBackendDefines(bool bindless, bool dsa, bool multiDraw);

private:
    int m_version = 460;
    std::string m_profile = "core";
    std::vector<std::pair<std::string, std::string>> m_defines;
    const ShaderIncludeManager* m_includes = nullptr;
};

// --- Hot-Reload Watcher ---

struct ShaderFileEntry {
    std::string path;
    uint64_t lastWriteTime = 0;
    ShaderStage stage = ShaderStage::Vertex;
};

class ShaderHotReloader {
public:
    void AddWatch(const std::string& path, ShaderStage stage);
    void RemoveWatch(const std::string& path);
    void ClearAll();
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    // Returns the paths that changed since the last check.
    std::vector<std::string> CheckForChanges();

    // Reloads the file and recompiles the shader.
    bool ReloadShader(GLShader& shader, const std::string& path);

private:
    std::vector<ShaderFileEntry> m_watches;
    bool m_enabled = false;
};

} // namespace Nova64::Video::GL
