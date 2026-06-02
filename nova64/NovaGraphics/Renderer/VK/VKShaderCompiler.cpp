/**
 * @file VKShaderCompiler.cpp
 * @brief Vulkan IShaderCompiler implementation.
 */
// SPDX-License-Identifier: MIT

#include "VKShaderCompiler.hpp"
#include "VKShader.hpp"
#include "../../Common/Logger.hpp"

#include <cstring>
#include <cstdio>

namespace Nova64 { namespace Video { namespace VK {

VKShaderCompiler::VKShaderCompiler(IRenderer* renderer)
    : m_renderer(renderer)
{}

VKShaderCompiler::~VKShaderCompiler() { Shutdown(); }

bool VKShaderCompiler::Initialize(Shader::BackendAPI api) {
    if (api != Shader::BackendAPI::Vulkan) {
        m_lastError = "VKShaderCompiler only supports Vulkan backend";
        return false;
    }
    GlslangInitProcess();
    return true;
}

void VKShaderCompiler::Shutdown() {
    m_programs.clear();
    GlslangFinalizeProcess();
}

// ─── Compile ─────────────────────────────────────────────────────────────────

Shader::CompiledShader VKShaderCompiler::Compile(
    const Shader::CombinerKey& key,
    const std::string& vertSrc,
    const std::string& fragSrc)
{
    Shader::CompiledShader result;
    result.key      = key;
    result.features = key.features;

    // CreateShaderProgram() returns VKShaderProgramImpl inside VKBackend.
    auto impl = m_renderer->CreateShaderProgram();
    if (!impl || !impl->Compile(vertSrc, fragSrc)) {
        m_lastError = impl ? impl->GetInfoLog() : "CreateShaderProgram failed";
        Logger::Warn("VKShaderCompiler: compile failed feat=0x{:llX}: {}",
                     (unsigned long long)key.features, m_lastError);
        result.valid = false;
        return result;
    }
    // Store features/mux for diagnostic logging in VKBackend.
    static_cast<VKShaderProgramImpl*>(impl.get())->debugFeatures = key.features;
    static_cast<VKShaderProgramImpl*>(impl.get())->debugMux      = key.mux;

    // Use the pointer value as the opaque handle.
    Shader::ShaderHandle h = reinterpret_cast<Shader::ShaderHandle>(impl.get());
    result.handle = h;
    result.valid  = true;
    m_programs[h] = std::move(impl);
    return result;
}

Shader::CompiledShader VKShaderCompiler::CompileWithGeometry(
    const Shader::CombinerKey& key,
    const std::string& vertSrc,
    const std::string& fragSrc,
    const std::string& /*geomSrc*/)
{
    // Geometry shaders on Vulkan require extra pipeline/extension support;
    // fall back to vertex+fragment only for now.
    return Compile(key, vertSrc, fragSrc);
}

void VKShaderCompiler::DeleteShader(Shader::CompiledShader& shader) {
    if (shader.handle) {
        m_programs.erase(shader.handle);
        shader.handle = 0;
        shader.valid  = false;
    }
}

// ─── BindShader ──────────────────────────────────────────────────────────────

void VKShaderCompiler::BindShader(const Shader::CompiledShader& shader) {
    if (!shader.valid || !shader.handle) return;
    auto it = m_programs.find(shader.handle);
    if (it == m_programs.end()) return;
    // Tell the VKBackend which shader (and by extension, which pipeline)
    // should be used for the next draw.
    m_renderer->BindShader(it->second.get());
}

// ─── GetUniformLocation ──────────────────────────────────────────────────────

int VKShaderCompiler::GetUniformLocation(Shader::ShaderHandle /*program*/,
                                         const char* name)
{
    // Return the byte offset inside the N64 UBO (std140 layout).
    // Texture-unit uniforms (uTexture0, uTexture1, …) return -1 because
    // Vulkan binds textures via descriptor sets, not glUniform1i.
    using namespace N64UBOOff;

    struct Entry { const char* name; int offset; };
    static const Entry kTable[] = {
        { "uMVPMatrix",           uMVPMatrix           },
        { "uModelView",           uModelView           },
        { "uProjection",          uProjection          },
        { "uPrimColor",           uPrimColor           },
        { "uEnvColor",            uEnvColor            },
        { "uFogColor",            uFogColor            },
        { "uBlendColor",          uBlendColor          },
        { "uFillColor",           uFillColor           },
        { "uCenterColor",         uCenterColor         },
        { "uScaleColor",          uScaleColor          },
        { "uConvertK",            uConvertK            },
        { "uTexClamp0",           uTexClamp0           },
        { "uTexClamp1",           uTexClamp1           },
        { "uTexCoordBounds0",     uTexCoordBounds0     },
        { "uTexCoordBounds1",     uTexCoordBounds1     },
        { "uKeyWidth",            uKeyWidth            },
        { "uMinLod",              uMinLod              },
        { "uTexScale0",           uTexScale0           },
        { "uTexScale1",           uTexScale1           },
        { "uTexOffset0",          uTexOffset0          },
        { "uTexOffset1",          uTexOffset1          },
        { "uTexMask0",            uTexMask0            },
        { "uTexMask1",            uTexMask1            },
        { "uTexSize0",            uTexSize0            },
        { "uTexSize1",            uTexSize1            },
        { "uGPUScale0",           uGPUScale0           },
        { "uGPUScale1",           uGPUScale1           },
        { "uScreenSize",          uScreenSize          },
        { "uInvScreenSize",       uInvScreenSize       },
        { "uScreenScale",         uScreenScale         },
        { "uResolutionScale",     uResolutionScale     },
        { "uPrimLOD",             uPrimLOD             },
        { "uPrimLODFrac",         uPrimLODFrac         },
        { "uK4",                  uK4                  },
        { "uK5",                  uK5                  },
        { "uAlphaRef",            uAlphaRef            },
        { "uFogMultiplier",       uFogMultiplier       },
        { "uFogOffset",           uFogOffset           },
        { "uPrimDepth",           uPrimDepth           },
        { "uDepthScale",          uDepthScale          },
        { "uDepthOffset",         uDepthOffset         },
        { "uPrimDeltaZ",          uPrimDeltaZ          },
        { "uFrameCount",          uFrameCount          },
        { "uZMode",               uZMode               },
        { "uUseTexCoordBounds",   uUseTexCoordBounds   },
        { "uTexturePersp",        uTexturePersp        },
        { "uFbMonochrome0",       uFbMonochrome0       },
        { "uFbMonochrome1",       uFbMonochrome1       },
        { "uFbFixedAlpha0",       uFbFixedAlpha0       },
        { "uFbFixedAlpha1",       uFbFixedAlpha1       },
        { "uFogUsage",            uFogUsage            },
        { "uScreenSpaceTriangle", uScreenSpaceTriangle },
        { "uTextureDetail",       uTextureDetail       },
        { "uMaxTile",             uMaxTile             },
        { "uDebugOutput",         uDebugOutput         },
        { "uTexCvgAlpha",         uTexCvgAlpha         },
    };

    for (const auto& e : kTable) {
        if (std::strcmp(e.name, name) == 0)
            return e.offset;
    }
    return -1; // unknown (includes texture unit uniforms)
}

// ─── UBOPtr helper ───────────────────────────────────────────────────────────

uint8_t* VKShaderCompiler::UBOPtr(int offset) {
    if (offset < 0 || static_cast<size_t>(offset) >= kN64UBOSize) return nullptr;
    void* base = m_renderer->GetMappedUBOPtr();
    if (!base) return nullptr;
    return static_cast<uint8_t*>(base) + offset;
}

// ─── Uniform setters ─────────────────────────────────────────────────────────

void VKShaderCompiler::SetUniform1i(int loc, int v) {
    uint8_t* p = UBOPtr(loc);
    if (p) std::memcpy(p, &v, sizeof(int));
}

void VKShaderCompiler::SetUniform1f(int loc, float v) {
    uint8_t* p = UBOPtr(loc);
    if (p) std::memcpy(p, &v, sizeof(float));
}

void VKShaderCompiler::SetUniform2f(int loc, float x, float y) {
    uint8_t* p = UBOPtr(loc);
    if (!p) return;
    float v[2] = {x, y};
    std::memcpy(p, v, 8);
}

void VKShaderCompiler::SetUniform3f(int loc, float x, float y, float z) {
    uint8_t* p = UBOPtr(loc);
    if (!p) return;
    float v[3] = {x, y, z};
    std::memcpy(p, v, 12);
    // w component (bytes +12) belongs to the same vec4 slot and was
    // initialised to 0 by memset — leave it untouched.
}

void VKShaderCompiler::SetUniform4f(int loc, float r, float g, float b, float a) {
    uint8_t* p = UBOPtr(loc);
    if (!p) return;
    float v[4] = {r, g, b, a};
    std::memcpy(p, v, 16);
}

void VKShaderCompiler::SetUniformMatrix4fv(int loc, const float* m) {
    uint8_t* p = UBOPtr(loc);
    if (p) std::memcpy(p, m, 64);
}

}}} // namespace Nova64::Video::VK
