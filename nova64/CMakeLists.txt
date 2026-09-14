cmake_minimum_required(VERSION 3.10)
project(nova64)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add /FS flag for MSVC to allow parallel compilation
if(MSVC)
    add_compile_options(/FS)
endif()

# Set vcpkg toolchain file if using vcpkg
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Exclude MSYS2 paths from find_package to avoid conflicts
set(CMAKE_IGNORE_PATH "C:/msys64;C:/msys64/ucrt64" CACHE STRING "" FORCE)
set(CMAKE_SYSTEM_IGNORE_PATH "C:/msys64;C:/msys64/ucrt64" CACHE STRING "" FORCE)

# Integrate AsmJit using FetchContent
include(FetchContent)
set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  asmjit
  GIT_REPOSITORY https://github.com/asmjit/asmjit.git
  # Pinned: upstream b56f417 (Nov 2025) marks a64::Assembler 'final', which
  # breaks CArm64Ops. This is the last revision the ARM64 backend compiles against.
  GIT_TAG        5134d396bd00c1b63259387acdbb12dfdf009f9b
)
FetchContent_MakeAvailable(asmjit)

if(TARGET asmjit)
    message(STATUS "AsmJit integrated - recompiler support enabled")
    add_compile_definitions(NOVA64_ASMJIT_ENABLED)
else()
    message(STATUS "AsmJit failed to integrate - recompiler will be stubbed")
endif()

# â”€â”€ SDL2 â”€â”€
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found via find_package - trying FetchContent")
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.30.2
    )
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL2)
    set(SDL2_LIBRARIES SDL2-static SDL2main)
    set(SDL2_INCLUDE_DIRS "${sdl2_SOURCE_DIR}/include")
else()
    message(STATUS "SDL2 found via find_package")
    set(SDL2_LIBRARIES SDL2::SDL2 SDL2::SDL2main)
endif()

# â”€â”€ glad (OpenGL 4.6 loader - pre-generated) â”€â”€
set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/glad")
add_library(glad_gl46 STATIC "${GLAD_DIR}/src/gl.c")
target_include_directories(glad_gl46 PUBLIC "${GLAD_DIR}/include")
message(STATUS "glad OpenGL 4.6 loader integrated (pre-generated)")

# â”€â”€ stb (stb_image for PNG hi-res texture loading) â”€â”€
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(stb)
message(STATUS "stb fetched for stb_image PNG decoding: ${stb_SOURCE_DIR}")

# â”€â”€ Vulkan (headers + volk dynamic loader) â”€â”€
# volk dlopens vulkan-1.dll at runtime; no SDK install required.
# We fetch sources for both repos but build volk.c manually as a small
# static library so we can fully control include directories without
# fighting cross-directory target modification policies.
FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.3.275
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(VulkanHeaders)

FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        1.3.270
    EXCLUDE_FROM_ALL
)
# Populate (download) but do NOT add volk's CMakeLists â€” we build volk.c ourselves.
FetchContent_GetProperties(volk)
if(NOT volk_POPULATED)
    FetchContent_Populate(volk)
endif()

add_library(nova_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(nova_volk PUBLIC
    "${volk_SOURCE_DIR}"
    "${vulkanheaders_SOURCE_DIR}/include"
)
target_compile_definitions(nova_volk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(nova_volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
message(STATUS "Vulkan integrated via volk (no SDK install required)")

# â”€â”€ glslang (GLSL â†’ SPIR-V compiler for the Vulkan backend) â”€â”€
# Disable everything we don't need to keep the dep light.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)                 # SPIRV-Tools optimizer
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        14.2.0
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(glslang)
message(STATUS "glslang integrated for runtime GLSLâ†’SPIRV compilation")

# â”€â”€ ImGui (immediate-mode GUI for debug panels) â”€â”€
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_DIR "${imgui_SOURCE_DIR}")
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_include_directories(imgui PRIVATE ${SDL2_INCLUDE_DIRS})
if(TARGET SDL2-static)
    target_link_libraries(imgui PRIVATE SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(imgui PRIVATE SDL2::SDL2)
endif()
message(STATUS "ImGui integrated for debug UI panels")

# ─── ImGui Node Editor ───
FetchContent_Declare(
    imgui_node_editor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    GIT_TAG        021aa0ea4da13fed864bafb2a92d4c5205076866
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(imgui_node_editor)
set(IMGUI_NODE_EDITOR_DIR "${imgui_node_editor_SOURCE_DIR}")
add_library(imgui_node_editor STATIC
    "${IMGUI_NODE_EDITOR_DIR}/imgui_node_editor.cpp"
    "${IMGUI_NODE_EDITOR_DIR}/imgui_node_editor_api.cpp"
    "${IMGUI_NODE_EDITOR_DIR}/imgui_canvas.cpp"
    "${IMGUI_NODE_EDITOR_DIR}/crude_json.cpp"
)
target_include_directories(imgui_node_editor PUBLIC "${IMGUI_NODE_EDITOR_DIR}")
target_link_libraries(imgui_node_editor PUBLIC imgui)
message(STATUS "ImGui Node Editor integrated for Playground UI")

# ─── nlohmann/json ───
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)
message(STATUS "nlohmann/json integrated")

# ─── rcheevos (RetroAchievements) ───
FetchContent_Declare(
    rcheevos
    GIT_REPOSITORY https://github.com/RetroAchievements/rcheevos.git
    GIT_TAG        v11.5.0
)
FetchContent_GetProperties(rcheevos)
if(NOT rcheevos_POPULATED)
    FetchContent_Populate(rcheevos)
    file(GLOB_RECURSE RCHEEVOS_SOURCES "${rcheevos_SOURCE_DIR}/src/*.c")
    # Exclude libretro frontend integration which requires libretro.h
    list(FILTER RCHEEVOS_SOURCES EXCLUDE REGEX "rc_libretro\\.c$")
    add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
    target_include_directories(rcheevos PUBLIC "${rcheevos_SOURCE_DIR}/include")
    target_compile_definitions(rcheevos PUBLIC RC_DISABLE_LUA)
endif()
message(STATUS "rcheevos integrated")

# ─── Lua 5.4 ───
FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    EXCLUDE_FROM_ALL
)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()
file(GLOB LUA_SOURCES "${lua_SOURCE_DIR}/src/*.c")
# Exclude Lua interpreter and compiler executables
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac)\.c$")
add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC "${lua_SOURCE_DIR}/src")
if(WIN32)
    target_compile_definitions(lua54 PRIVATE LUA_USE_WINDOWS)
endif()
message(STATUS "Lua 5.4 integrated")

# Add all source files
set(SOURCES
    # Common utilities
    common/DateTime.cpp
    common/DynamicLibrary.cpp
    common/File.cpp
    common/IniFile.cpp
    common/LockGuard.cpp
    common/log.cpp
    common/md5.cpp
    common/MemoryManagment.cpp
    common/Thread.cpp
    common/TimeStamp.cpp
    common/trace.cpp
    
    # N64 System files
    N64SYS/SysGlobals.cpp
    N64SYS/N64DD.cpp
    N64SYS/N64Rom.cpp
    N64SYS/MainSYS.cpp
    N64SYS/Interpreter/InterpreterOps.cpp
    N64SYS/MIPSY/TLB.cpp
    N64SYS/MIPSY/Systiming.cpp
    N64SYS/MIPSY/DDisk.cpp
    N64SYS/MIPSY/GBcart.cpp
    N64SYS/MIPSY/Mempak.cpp
    N64SYS/MIPSY/Rumblepak.cpp
    N64SYS/MIPSY/TransferPak.cpp
    N64SYS/MIPSY/R4300Instruction.cpp
    N64SYS/MIPSY/Register.cpp
    N64SYS/MIPSY/Vmem.cpp
    
    # Plugins
    N64SYS/Plugins/PluginManager.cpp
    N64SYS/Plugins/GFXPlugin.cpp
    N64SYS/Plugins/AudioPlugin.cpp
    N64SYS/Plugins/InputPlugin.cpp
    N64SYS/Plugins/RSPPlugin.cpp

    # Memory Handlers
    N64SYS/Memoryhandler/AudioInterfaceHandler.cpp
    N64SYS/Memoryhandler/CachedRom.cpp
    N64SYS/Memoryhandler/CachedSysMemory.cpp
    N64SYS/Memoryhandler/DisplayCtrlReg.cpp
    N64SYS/Memoryhandler/MIPSYInterfaceHandler.cpp
    N64SYS/Memoryhandler/PifInterface.cpp
    N64SYS/Memoryhandler/PIInterFace.cpp
    N64SYS/Memoryhandler/RDRAMInterFaceHandler.cpp
    N64SYS/Memoryhandler/RDRAMRegHandler.cpp
    N64SYS/Memoryhandler/RomMemHandler.cpp
    N64SYS/Memoryhandler/SIInterfaceHandler.cpp
    N64SYS/Memoryhandler/SPregistersHandler.cpp
    N64SYS/Memoryhandler/UnCachedRom.cpp
    N64SYS/Memoryhandler/UnCachedSysMemory.cpp
    N64SYS/Memoryhandler/VideoInterfaceHandler.cpp
    N64SYS/Savetypes/Eeprom.cpp
    N64SYS/Savetypes/FRam.cpp
    N64SYS/Savetypes/SRam.cpp
    N64SYS/Recomp/CodeBlock.cpp
    N64SYS/Recomp/CodeSection.cpp
    N64SYS/Recomp/ExitInfo.cpp
    N64SYS/Recomp/FunctionInfo.cpp
    N64SYS/Recomp/FunctionMap.cpp
    N64SYS/Recomp/JumpInfo.cpp
    N64SYS/Recomp/LoopAnalysis.cpp
    N64SYS/Recomp/RegBase.cpp
    N64SYS/Recomp/Recompiler.cpp
    N64SYS/Recomp/RecompilerMemory.cpp
    N64SYS/Recomp/RegInfo.cpp
    N64SYS/Recomp/x64-86/x64RecompilerOps.cpp
    N64SYS/Recomp/x64-86/x64ops.cpp
    N64SYS/Recomp/x64-86/x64RegInfo.cpp
    # AArch64 (Apple Silicon / ARM64) recompiler backend.  Like the x64-86
    # sources above, these are listed unconditionally and self-gate with
    # `#if defined(__aarch64__) || defined(_M_ARM64)`, so they compile to empty
    # translation units on non-ARM64 hosts.
    N64SYS/Recomp/Arm64/arm64ops.cpp
    N64SYS/Recomp/Arm64/arm64RecompilerOps.cpp

    # NovaRSP - RSP plugin (CPU core)
    NovaRSP/CPU/RspClamp.cpp
    NovaRSP/CPU/RSPCpu.cpp
    NovaRSP/CPU/RSPInstruction.cpp
    NovaRSP/CPU/RSPInterpreterOps.cpp
    NovaRSP/CPU/RspLog.cpp
    NovaRSP/CPU/RspMemory.cpp
    NovaRSP/CPU/RSPRegisterHandler.cpp
    NovaRSP/CPU/RspSystem.cpp

    # NovaRSP - RSP plugin (HLE audio / task / graphics)
    NovaRSP/HLE/alist.cpp
    NovaRSP/HLE/arithmetics.cpp
    NovaRSP/HLE/audio.cpp
    NovaRSP/HLE/AudioOutput.cpp
    NovaRSP/HLE/hle.cpp
    NovaRSP/HLE/mem.cpp
    NovaRSP/HLE/TaskDispatcher.cpp
    NovaRSP/HLE/ucodes.cpp

    # NovaRSP - RSP plugin (settings / misc)
    NovaRSP/Settings/RspSettings.cpp
    NovaRSP/RSPDebugger.cpp
    NovaRSP/RSPInfo.cpp

    # NovaInput - Keyboard input mapping
    NovaInput/KeyboardInput.cpp

    # NovaAudio - Built-in audio plugin
    NovaAudio/NovaAudioPlugin.cpp

    # NovaGraphics - Graphics plugin (Common)
    NovaGraphics/Common/BitUtils.cpp
    NovaGraphics/Common/ClipMath.cpp
    NovaGraphics/Common/Logger.cpp
    NovaGraphics/Common/Memory.cpp
    NovaGraphics/Common/SIMDMath.cpp
    NovaGraphics/Common/VectorMath.cpp

    # NovaGraphics - Graphics plugin (Compat)
    NovaGraphics/Compat/GameDatabase.cpp

    # NovaGraphics - Graphics plugin (Core)
    NovaGraphics/Core/DepthBuffer.cpp
    NovaGraphics/Core/FramebufferManager.cpp
    NovaGraphics/Core/N64Math.cpp
    NovaGraphics/Core/RDPCommand.cpp
    NovaGraphics/Core/RDPProcessor.cpp
    NovaGraphics/Core/RDPProcessor_State.cpp
    NovaGraphics/Core/RDPProcessor_Texture.cpp
    NovaGraphics/Core/RDPProcessor_Draw.cpp
    NovaGraphics/Core/RDPProcessor_Batch.cpp

    # NovaGraphics - Graphics plugin (Debug â€” ImGuiDebugUI only; rest removed as dead)
    NovaGraphics/Debug/ImGuiDebugUI.cpp

    # NovaGraphics - Graphics plugin (Microcode)
    NovaGraphics/Microcode/GBIHandler.cpp
    NovaGraphics/Microcode/MicrocodeDispatcher.cpp
    NovaGraphics/Microcode/MicrocodeRegistry.cpp

    # NovaGraphics - Graphics plugin (Plugin)
    NovaGraphics/Plugin/NovaGraphicsGFXPlugin.cpp
    NovaGraphics/Plugin/PluginConfig.cpp
    NovaGraphics/Plugin/VideoPlugin.cpp

    # NovaGraphics - Graphics plugin (Renderer)
    NovaGraphics/Renderer/GL/GLShaderCompiler.cpp
    NovaGraphics/Renderer/GL/GLShaderProgram.cpp
    NovaGraphics/Renderer/GLBackend.cpp
    NovaGraphics/Renderer/GLRenderer.cpp
    NovaGraphics/Renderer/PostProcessing.cpp
    NovaGraphics/Renderer/RendererFactory.cpp
    NovaGraphics/Renderer/ShaderGenerator.cpp
    NovaGraphics/Renderer/ShaderManager.cpp

    # NovaGraphics - Graphics plugin (Renderer/VK - real impl via volk loader)
    NovaGraphics/Renderer/VK/VKBackend.cpp
    NovaGraphics/Renderer/VK/VKInstance.cpp
    NovaGraphics/Renderer/VK/VKDevice.cpp
    NovaGraphics/Renderer/VK/VKSwapchain.cpp
    NovaGraphics/Renderer/VK/VKMemory.cpp
    NovaGraphics/Renderer/VK/VKBuffer.cpp
    NovaGraphics/Renderer/VK/VKTexture.cpp
    NovaGraphics/Renderer/VK/VKShader.cpp
    NovaGraphics/Renderer/VK/VKPipeline.cpp
    NovaGraphics/Renderer/VK/VKRenderTarget.cpp
    NovaGraphics/Renderer/VK/VKAccelStruct.cpp
    NovaGraphics/Renderer/VK/VKRTPipeline.cpp
    NovaGraphics/Renderer/VK/VKComputeRaytracer.cpp
    NovaGraphics/RT/RTBVH.cpp
    NovaGraphics/Renderer/VK/VKShaderCompiler.cpp

    # NovaGraphics - Graphics plugin (Texture)
    NovaGraphics/Texture/stb_image_impl.cpp
    NovaGraphics/Texture/HiResTextures.cpp
    NovaGraphics/Texture/TextureCache.cpp

    # NovaGraphics - Graphics plugin (Window)
    NovaGraphics/Window/WindowManager.cpp

    main.cpp
)

# RSP JIT recompiler (Phase 2 â€” unfinished for F3D microcode; OFF by default)
option(NOVA_RSP_RECOMPILER "Enable RSP JIT recompiler (Phase 2 work)" OFF)
if(NOVA_RSP_RECOMPILER)
    list(APPEND SOURCES
        NovaRSP/Recompiler/RspAssembler.cpp
        NovaRSP/Recompiler/RspCodeBlock.cpp
        NovaRSP/Recompiler/RspProfiling.cpp
        NovaRSP/Recompiler/RspRecompilerCPU.cpp
        NovaRSP/Recompiler/RspRecompilerOps.cpp
        NovaRSP/Recompiler/RspRegState.cpp
        NovaRSP/Recompiler/x64/x64RspOps.cpp
        NovaRSP/Recompiler/x64/x64RspRegInfo.cpp
        NovaRSP/Recompiler/x64/x64RspVectorGen.cpp
    )
endif()

# Add header files
set(HEADERS   
    # Common utilities
    common/file.hpp
    common/log.hpp
    common/md5.hpp
    common/MemoryManagment.hpp
    common/platform.hpp
    common/trace.hpp
    
    # N64 System headers
    N64SYS/MainSYS.hpp
    N64SYS/N64DD.hpp
    N64SYS/N64Rom.hpp
    N64SYS/SysGlobals.hpp
    N64SYS/Types.hpp
    N64SYS/MIPSY/Register.hpp
    N64SYS/MIPSY/Vmem.hpp
    N64SYS/MIPSY/R4300Instruction.hpp
    N64SYS/Savetypes/Eeprom.hpp
    N64SYS/Savetypes/FRam.hpp
    N64SYS/Savetypes/SRam.hpp
    N64SYS/Recomp/asmjit.hpp
    N64SYS/Recomp/CodeBlock.hpp
)

# SoftFloat Library
set(SOFTFLOAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../softfloat")
file(GLOB SOFTFLOAT_SOURCES 
    "${SOFTFLOAT_DIR}/source/*.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/*.c"
)
# Exclude multi-word (M-variant) files for x64 with SOFTFLOAT_FAST_INT64.
# These files implement 128-bit float/extF80 ops using arrays of 32-bit words,
# which conflicts with the FAST_INT64 path that uses native 64-bit structs.
# The *M variant internal functions (softfloat_roundPackMToF128M, etc.) are
# not defined in FAST_INT64 builds, causing compile errors.
file(GLOB SOFTFLOAT_EXCLUDE_M
    # All multi-word (M-variant) primitives and 128-bit/extF80 ops live in files
    # ending in "M.c".  Under SOFTFLOAT_FAST_INT64 these use the native-64-bit
    # struct path instead, and the *M internal primitives are not even declared,
    # so every "*M.c" must be excluded.  (The N64 FPU only needs f32/f64.)
    "${SOFTFLOAT_DIR}/source/*M.c"
    # The 8086-SSE specialization also ships M-variant (multi-word) NaN helpers
    # that reference internal *M primitives which are not declared under
    # SOFTFLOAT_FAST_INT64.  Exclude them too (f128/extF80 are unused by the
    # N64 FPU, which only needs f32/f64).
    "${SOFTFLOAT_DIR}/source/8086-SSE/extF80M_isSignalingNaN.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/f128M_isSignalingNaN.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/s_commonNaNToExtF80M.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/s_commonNaNToF128M.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/s_extF80MToCommonNaN.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/s_f128MToCommonNaN.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/s_propagateNaNExtF80M.c"
    "${SOFTFLOAT_DIR}/source/8086-SSE/s_propagateNaNF128M.c"
)
list(REMOVE_ITEM SOFTFLOAT_SOURCES ${SOFTFLOAT_EXCLUDE_M})
add_library(softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(softfloat PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}/config"
    "${SOFTFLOAT_DIR}/source/include"
    "${SOFTFLOAT_DIR}/source/8086-SSE"
)

  # Append UI Launcher
  list(APPEND SOURCES UI/NovaLauncher.cpp)
  list(APPEND SOURCES UI/HexEditor.cpp)

  # FrameCallback
  list(APPEND SOURCES NovaGraphics/Plugin/FrameCallback.cpp)

  # SaveState
  list(APPEND SOURCES N64SYS/SaveState.cpp)

  # Cross-engine divergence tracing
  list(APPEND SOURCES N64SYS/StateTrace.cpp)

  # NovaLua scripting
  list(APPEND SOURCES NovaLua/LuaEngine.cpp)
# Create main executable
add_executable(nova64 ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(nova64 PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/NovaRSP
    ${CMAKE_CURRENT_SOURCE_DIR}/NovaGraphics
    ${SDL2_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
    ${lua_SOURCE_DIR}/src
)

# Link AsmJit if available
if(TARGET asmjit)
    target_link_libraries(nova64 PRIVATE asmjit)
    message(STATUS "Linking with AsmJit")
endif()

target_link_libraries(nova64 PRIVATE softfloat)

# NovaVideo dependencies: SDL2, glad, OpenGL
target_link_libraries(nova64 PRIVATE ${SDL2_LIBRARIES} glad_gl46 imgui imgui_node_editor nlohmann_json::nlohmann_json rcheevos lua54)
find_package(OpenGL REQUIRED)
target_link_libraries(nova64 PRIVATE OpenGL::GL)

# Vulkan backend dependencies (volk dynamically loads vulkan-1.dll at runtime;
# Vulkan::Headers provides the headers; we never link vulkan-1.lib directly).
target_link_libraries(nova64 PRIVATE nova_volk)
target_compile_definitions(nova64 PRIVATE VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(nova64 PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

# glslang (runtime GLSLâ†’SPIR-V compiler for the Vulkan backend)
target_link_libraries(nova64 PRIVATE glslang SPIRV glslang-default-resource-limits)

if(NOVA_RSP_RECOMPILER)
    target_compile_definitions(nova64 PRIVATE NOVA_RSP_RECOMPILER)
endif()

# WASAPI audio output (Windows)
if(WIN32)
    target_link_libraries(nova64 PRIVATE ole32 uuid)
endif()

# macOS: ship MoltenVK (Vulkan-on-Metal) next to the binary. macOS has no system
# Vulkan loader and its OpenGL caps at 4.1 (the renderer needs 4.6), so Vulkan via
# MoltenVK is the only working backend. main.cpp preloads this dylib at startup.
if(APPLE)
    add_custom_command(TARGET nova64 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/MoltenVK/libMoltenVK.dylib"
            "$<TARGET_FILE_DIR:nova64>/libMoltenVK.dylib"
        COMMENT "Copying libMoltenVK.dylib next to nova64")
endif()

# Compiler flags
if(MSVC)
    target_compile_options(nova64 PRIVATE /W4)
    # Release/RelWithDebInfo: max optimizations + whole-program optimization
    # /Zi generates PDB debug info even in Release for debuggable optimized builds
    target_compile_options(nova64 PRIVATE 
        $<$<CONFIG:Release>:/O2 /Oi /Ot /GL /GS- /fp:fast /Zi>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Ot /fp:fast>
    )
    target_link_options(nova64 PRIVATE 
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /DEBUG /OPT:REF>
    )
    # Increase stack size to 8 MB â€” the synchronous RSP HLE call chain from
    # inside interpreter SW â†’ SP handler â†’ DoRspCycles â†’ HLE â†’ GFX plugin
    # can exceed the default 1 MB stack in Debug builds.
    target_link_options(nova64 PRIVATE /STACK:8388608)
else()
    target_compile_options(nova64 PRIVATE -Wall -Wextra)
    target_compile_options(nova64 PRIVATE 
        $<$<CONFIG:Release>:-O3 -march=native -ffast-math>
        $<$<CONFIG:RelWithDebInfo>:-O2 -march=native>
    )
endif()

