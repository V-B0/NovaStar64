# Third-party code: everything the build downloads (pinned to an exact version or
# commit, so a build next year is the same build) plus the two libraries that ship
# in the repository (glad, SoftFloat). Included from CMakeLists.txt; nothing here is
# NovaStar64's own code.
#
# To bump a dependency: change its GIT_TAG in its own commit and rebuild from scratch
# (./build.sh --clean), so a regression can be traced to the bump.

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

# The CPU recompiler is built on AsmJit, so it is always present.
add_compile_definitions(NOVA64_ASMJIT_ENABLED)

# ── SDL2 ──
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

# ── glad (OpenGL 4.6 loader - pre-generated) ──
set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/glad")
add_library(glad_gl46 STATIC "${GLAD_DIR}/src/gl.c")
target_include_directories(glad_gl46 PUBLIC "${GLAD_DIR}/include")
message(STATUS "glad OpenGL 4.6 loader integrated (pre-generated)")

# ── stb (stb_image for PNG hi-res texture loading) ──
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(stb)
message(STATUS "stb fetched for stb_image PNG decoding: ${stb_SOURCE_DIR}")

# ── Vulkan (headers + volk dynamic loader) ──
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
# Populate (download) but do NOT add volk's CMakeLists — we build volk.c ourselves.
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

# ── glslang (GLSL → SPIR-V compiler for the Vulkan backend) ──
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
message(STATUS "glslang integrated for runtime GLSL→SPIRV compilation")

# ── ImGui (immediate-mode GUI for debug panels) ──
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

# ── nlohmann/json ──
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)
message(STATUS "nlohmann/json integrated")

# ── rcheevos (RetroAchievements) ──
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

# ── Lua 5.4 ──
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

# SoftFloat Library
# Normally the git submodule in ../softfloat. A source archive downloaded from
# GitHub, or a clone made without --recurse-submodules, has an empty folder there;
# rather than failing with a missing-header error, fetch the same commit the
# submodule points at (keep the two in step: `git submodule status`).
set(SOFTFLOAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../softfloat")
if(NOT EXISTS "${SOFTFLOAT_DIR}/source/include/softfloat.h")
    message(STATUS "softfloat submodule is empty - fetching it instead")
    FetchContent_Declare(
        softfloat
        GIT_REPOSITORY https://github.com/ucb-bar/berkeley-softfloat-3
        GIT_TAG        a0c6494cdc11865811dec815d5c0049fba9d82a8
    )
    FetchContent_GetProperties(softfloat)
    if(NOT softfloat_POPULATED)
        FetchContent_Populate(softfloat)
    endif()
    set(SOFTFLOAT_DIR "${softfloat_SOURCE_DIR}")
endif()
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
