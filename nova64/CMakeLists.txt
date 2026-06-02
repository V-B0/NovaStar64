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
  GIT_TAG        master
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
    GIT_TAG        master
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
    common/path.cpp
    common/platform.cpp
    common/Random.cpp
    common/StdString.cpp
    common/SyncEvents.cpp
    common/Thread.cpp
    common/TimeStamp.cpp
    common/trace.cpp
    common/util.cpp
    
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
    NovaGraphics/Common/ColorSpace.cpp
    NovaGraphics/Common/Logger.cpp
    NovaGraphics/Common/Memory.cpp
    NovaGraphics/Common/Platform.cpp
    NovaGraphics/Common/SIMDMath.cpp
    NovaGraphics/Common/VectorMath.cpp
    NovaGraphics/Common/VideoConfig.cpp
    NovaGraphics/Common/CRC.cpp

    # NovaGraphics - Graphics plugin (Compat)
    NovaGraphics/Compat/GameDatabase.cpp
    NovaGraphics/Compat/GamePatches.cpp

    # NovaGraphics - Graphics plugin (Core)
    NovaGraphics/Core/DepthBuffer.cpp
    NovaGraphics/Core/FramebufferManager.cpp
    NovaGraphics/Core/N64Math.cpp
    NovaGraphics/Core/N64Memory.cpp
    NovaGraphics/Core/RDPCommand.cpp
    NovaGraphics/Core/RDPLogger.cpp
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
    NovaGraphics/Renderer/VK/VKShaderCompiler.cpp

    # NovaGraphics - Graphics plugin (Texture)
    NovaGraphics/Texture/stb_image_impl.cpp
    NovaGraphics/Texture/HiResTextures.cpp
    NovaGraphics/Texture/TextureCache.cpp
    NovaGraphics/Texture/TextureConverter.cpp
    NovaGraphics/Texture/TextureDecoder.cpp
    NovaGraphics/Texture/TextureFormat.cpp
    NovaGraphics/Texture/TexConv.cpp

    # NovaGraphics - Graphics plugin (Window)
    NovaGraphics/Window/DisplayConfig.cpp
    NovaGraphics/Window/InputManager.cpp
    NovaGraphics/Window/VideoInterface.cpp
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
    common/path.hpp
    common/trace.hpp
    common/util.hpp
    
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
    "${SOFTFLOAT_DIR}/source/extF80M_*.c"
    "${SOFTFLOAT_DIR}/source/f128M_*.c"
    "${SOFTFLOAT_DIR}/source/*_to_extF80M.c"
    "${SOFTFLOAT_DIR}/source/*_to_f128M.c"
    "${SOFTFLOAT_DIR}/source/s_addExtF80M.c"
    "${SOFTFLOAT_DIR}/source/s_addF128M.c"
    "${SOFTFLOAT_DIR}/source/s_invalidExtF80M.c"
    "${SOFTFLOAT_DIR}/source/s_invalidF128M.c"
    "${SOFTFLOAT_DIR}/source/s_isNaNF128M.c"
    "${SOFTFLOAT_DIR}/source/s_mulAddF128M.c"
    "${SOFTFLOAT_DIR}/source/s_normExtF80SigM.c"
    "${SOFTFLOAT_DIR}/source/s_normRoundPackMToExtF80M.c"
    "${SOFTFLOAT_DIR}/source/s_normRoundPackMToF128M.c"
    "${SOFTFLOAT_DIR}/source/s_normSubnormalF128SigM.c"
    "${SOFTFLOAT_DIR}/source/s_roundPackMToExtF80M.c"
    "${SOFTFLOAT_DIR}/source/s_roundPackMToF128M.c"
    "${SOFTFLOAT_DIR}/source/s_shiftNormSigF128M.c"
    "${SOFTFLOAT_DIR}/source/s_tryPropagateNaNExtF80M.c"
    "${SOFTFLOAT_DIR}/source/s_tryPropagateNaNF128M.c"
    "${SOFTFLOAT_DIR}/source/s_compareNonnormExtF80M.c"
    "${SOFTFLOAT_DIR}/source/s_compare128M.c"
    "${SOFTFLOAT_DIR}/source/s_compare96M.c"
)
list(REMOVE_ITEM SOFTFLOAT_SOURCES ${SOFTFLOAT_EXCLUDE_M})
add_library(softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(softfloat PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}/config"
    "${SOFTFLOAT_DIR}/source/include"
    "${SOFTFLOAT_DIR}/source/8086-SSE"
)

# Create main executable
add_executable(nova64 ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(nova64 PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/NovaRSP
    ${CMAKE_CURRENT_SOURCE_DIR}/NovaGraphics
    ${SDL2_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
)

# Link AsmJit if available
if(TARGET asmjit)
    target_link_libraries(nova64 PRIVATE asmjit)
    message(STATUS "Linking with AsmJit")
endif()

target_link_libraries(nova64 PRIVATE softfloat)

# NovaVideo dependencies: SDL2, glad, OpenGL
target_link_libraries(nova64 PRIVATE ${SDL2_LIBRARIES} glad_gl46 imgui)
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

