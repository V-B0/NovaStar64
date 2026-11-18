# 3.10 only fixes the policy defaults the build was written against. The build itself
# needs CMake 3.28 (FetchContent_Declare's EXCLUDE_FROM_ALL, used in
# cmake/Dependencies.cmake); build.sh checks.
cmake_minimum_required(VERSION 3.10)

# The release version lives in one place, the VERSION file at the top of the
# repository; the build scripts, the installers and the executable all read it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" NOVA64_VERSION LIMIT_COUNT 1)
project(nova64 VERSION ${NOVA64_VERSION})

# ── Options ──

# Windows builds that are handed to other people link the C++ runtime statically,
# so the program starts without the "Visual C++ Redistributable" being installed.
# The release workflow turns this on; everyday builds leave it off. (Set after
# project(), which is what defines MSVC.) The policy default makes the setting reach
# the downloaded libraries too (SDL, glslang, ...), whose own CMake files would
# otherwise ignore it.
option(NOVA64_STATIC_RUNTIME "Link the MSVC runtime statically (Windows builds you distribute)" OFF)
if(MSVC AND NOVA64_STATIC_RUNTIME)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# -march=native tunes the executable for the machine that builds it, which is what
# you want for a build you will run yourself and exactly wrong for one you give to
# other people: built on an M3 or a modern x86 CPU it can use instructions an
# M1 or an older PC doesn't have and crash on the first one it meets. The packaging
# scripts turn this off, which falls back to a baseline every supported CPU has.
option(NOVA64_NATIVE_ARCH "Tune the build for this machine's CPU (turn off for builds you distribute)" ON)

# A plain `cmake -B build` leaves the build type empty, which compiles without
# optimisation -- far too slow to play a game. Default to the configuration the
# project is developed and tested with.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type (Debug, Release, RelWithDebInfo)" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Windows toolchain ──
if(MSVC)
    add_compile_options(/FS)    # lets parallel compiles share one PDB
endif()
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()
if(WIN32)
    # A stray MSYS2 install on PATH makes find_package pick up its (incompatible) libraries.
    set(CMAKE_IGNORE_PATH "C:/msys64;C:/msys64/ucrt64" CACHE STRING "" FORCE)
    set(CMAKE_SYSTEM_IGNORE_PATH "C:/msys64;C:/msys64/ucrt64" CACHE STRING "" FORCE)
endif()

# ── Third-party libraries and our source list ──
include(cmake/Dependencies.cmake)    # downloads, pinned; sets up the library targets
include(cmake/Sources.cmake)         # NOVA64_SOURCES

# Files compiled into the binary as a list of bytes that a source file #includes, so the
# program needs nothing next to the executable. The output is rewritten only when the
# file changes, so an ordinary reconfigure doesn't recompile its users.
function(nova64_embed_file SOURCE_FILE OUTPUT_NAME)
    set(inc "${CMAKE_CURRENT_BINARY_DIR}/generated/${OUTPUT_NAME}")
    file(READ "${SOURCE_FILE}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    set(old "")
    if(EXISTS "${inc}")
        file(READ "${inc}" old)
    endif()
    if(NOT old STREQUAL "${bytes}\n")
        file(WRITE "${inc}" "${bytes}\n")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SOURCE_FILE}")
endfunction()

# The NovaStar icon (UI/AppIcon.cpp), and what the launcher draws with (UI/UiKit.cpp): the Inter and
# Phosphor fonts (cut down by scripts/ui-art/subset-fonts.py) and the background picture.
set(NOVA_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/Novastar.png")
nova64_embed_file("${NOVA_ICON_PNG}" NovastarIcon.inc)
nova64_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf" InterRegular.inc)
nova64_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-SemiBold.ttf" InterSemiBold.inc)
nova64_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-Bold.ttf" InterBold.inc)
nova64_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Phosphor.ttf" PhosphorRegular.inc)
nova64_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Phosphor-Fill.ttf" PhosphorFill.inc)
nova64_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/background.jpg" Background.inc)

# Windows: the executable's own icon comes from a resource script.
if(WIN32)
    enable_language(RC)
    list(APPEND NOVA64_SOURCES assets/nova64.rc)
endif()

# ── The executable ──
add_executable(nova64 ${NOVA64_SOURCES})
# Only main.cpp and the launcher read the version, so only they are recompiled when it changes.
set_source_files_properties(main.cpp UI/NovaLauncher.cpp PROPERTIES COMPILE_DEFINITIONS "NOVA64_VERSION=\"${NOVA64_VERSION}\"")

# The RSP recompiler's vector kernels use compiler vector types where there are any (GCC,
# Clang) and a portable stand-in elsewhere (MSVC). This switch forces the stand-in, so
# that code path can be tested with `nova64 --rsp-selftest` on a machine with Clang.
option(NOVA64_RSP_PORTABLE_KERNELS "Use the portable RSP vector kernels even where compiler vector types exist (testing)" OFF)
if(NOVA64_RSP_PORTABLE_KERNELS)
    set_source_files_properties(NovaRSP/Jit/RspJitKernels.cpp PROPERTIES COMPILE_DEFINITIONS NOVA_RSP_FORCE_LANES)
endif()

# A build you run from its own folder keeps its settings, games and saves right
# there, so the marker file below goes next to the executable. An installed copy
# has no such file and keeps them in the user's own data folder instead (see
# SelectDataDirectory in main.cpp).
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:nova64>/portable.txt" CONTENT
"This file makes NovaStar64 keep its settings, games, saves and caches in this
folder instead of in your user data folder. Delete it to use the user data folder.
")

target_include_directories(nova64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/NovaRSP
    ${CMAKE_CURRENT_SOURCE_DIR}/NovaGraphics
    ${SDL2_INCLUDE_DIRS}
    ${lua_SOURCE_DIR}/src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
# A third-party header: its own warnings are not ours to fix.
target_include_directories(nova64 SYSTEM PRIVATE ${stb_SOURCE_DIR})

# Tests: `ctest` after a build runs the RSP self-test and the unit tests (they need no ROM, window or GPU).
option(NOVA64_BUILD_TESTS "Build the unit tests and register them with ctest" ON)
if(NOVA64_BUILD_TESTS)
    enable_testing()
    add_executable(simlab_test tests/SimLabTest.cpp UI/SimLab.cpp UI/GameFiles.cpp)
    target_link_libraries(simlab_test PRIVATE imgui)
    add_test(NAME simlab COMMAND simlab_test)
    add_executable(audio_test tests/AudioOutputTest.cpp NovaRSP/HLE/AudioOutput.cpp)
    if(TARGET SDL2-static)
        target_link_libraries(audio_test PRIVATE SDL2-static)
    else()
        target_link_libraries(audio_test PRIVATE SDL2::SDL2)
    endif()
    add_test(NAME audio COMMAND audio_test)
    add_test(NAME rsp-selftest COMMAND nova64 --rsp-selftest --programs 200)
endif()

find_package(OpenGL REQUIRED)
target_link_libraries(nova64 PRIVATE
    asmjit softfloat
    ${SDL2_LIBRARIES} OpenGL::GL glad_gl46
    imgui
    nlohmann_json::nlohmann_json rcheevos lua54
    # Vulkan: volk loads the Vulkan library at run time, so there is nothing to link
    # against and no SDK to install. glslang compiles the shaders to SPIR-V at run time.
    nova_volk glslang SPIRV glslang-default-resource-limits
)
target_compile_definitions(nova64 PRIVATE VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(nova64 PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    # COM, and the common file dialog the launcher opens ROMs with
    target_link_libraries(nova64 PRIVATE ole32 uuid comdlg32)
endif()

# macOS: ship MoltenVK (Vulkan-on-Metal) next to the binary. macOS has no system
# Vulkan loader and its OpenGL caps at 4.1 (the renderer needs 4.6), so Vulkan via
# MoltenVK is the only working backend. main.cpp preloads this dylib at startup.
#
# The same step signs the binary (ad hoc; without it the dylib fails to load and
# the process hangs silently) and then gives it the NovaStar icon. The order
# matters: signing refuses a file that already carries a Finder icon.
if(APPLE)
    add_custom_command(TARGET nova64 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/MoltenVK/libMoltenVK.dylib"
            "$<TARGET_FILE_DIR:nova64>/libMoltenVK.dylib"
        COMMAND codesign --force --deep --sign - "$<TARGET_FILE:nova64>"
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-macos-file-icon.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/Novastar.png" "$<TARGET_FILE:nova64>"
        COMMENT "Copying libMoltenVK.dylib, signing nova64 and giving it the NovaStar icon")
endif()

# ── Compiler flags ──
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
    # Increase stack size to 8 MB: the synchronous RSP HLE call chain from
    # inside interpreter SW -> SP handler -> DoRspCycles -> HLE -> GFX plugin
    # can exceed the default 1 MB stack in Debug builds.
    target_link_options(nova64 PRIVATE /STACK:8388608)
else()
    # The Vulkan code starts each structure as `VkFoo x{VK_STRUCTURE_TYPE_FOO};` (the rest
    # is zeroed) and then sets the fields it needs, which -Wmissing-field-initializers
    # flags on every one of them.
    # -Wunused-parameter is off too: the plugin interfaces have many methods that a given
    # implementation doesn't need, and 120 warnings about them hide the ones that matter.
    target_compile_options(nova64 PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
    # The CPU being built FOR. On a Mac, CMAKE_SYSTEM_PROCESSOR is the CPU doing the
    # build, which differs when -DCMAKE_OSX_ARCHITECTURES=x86_64 builds an Intel copy on
    # Apple Silicon (handy for testing the x86-64 code under Rosetta).
    set(NOVA64_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES NOVA64_ARCH_COUNT)
        if(NOVA64_ARCH_COUNT EQUAL 1)
            set(NOVA64_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
        else()
            set(NOVA64_TARGET_ARCH "universal")   # one flag can't suit both slices
        endif()
    endif()
    if(NOVA64_NATIVE_ARCH AND NOVA64_TARGET_ARCH STREQUAL CMAKE_SYSTEM_PROCESSOR)
        set(NOVA64_ARCH_FLAG -march=native)
    elseif(NOVA64_TARGET_ARCH MATCHES "^(x86_64|AMD64|amd64)$")
        set(NOVA64_ARCH_FLAG -march=x86-64-v2)    # SSE4.2: every x86-64 CPU since about 2009
    elseif(APPLE AND NOVA64_TARGET_ARCH MATCHES "^(arm64|aarch64)$")
        set(NOVA64_ARCH_FLAG -mcpu=apple-m1)      # the oldest Apple Silicon; later ones run it too
    else()
        set(NOVA64_ARCH_FLAG "")
    endif()
    target_compile_options(nova64 PRIVATE
        $<$<CONFIG:Release>:-O3 ${NOVA64_ARCH_FLAG} -ffast-math>
        $<$<CONFIG:RelWithDebInfo>:-O2 ${NOVA64_ARCH_FLAG}>
    )
endif()
