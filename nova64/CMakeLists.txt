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

# Find AsmJit package (optional for recompiler support)
find_package(asmjit CONFIG QUIET)
if(asmjit_FOUND)
    message(STATUS "AsmJit found - recompiler support enabled")
    add_compile_definitions(NOVA64_ASMJIT_ENABLED)
else()
    message(STATUS "AsmJit not found - recompiler will be stubbed")
endif()

# Add all source files
set(SOURCES
    # Common utilities
    common/md5.cpp
    common/util.cpp
    
    # N64 System files
    N64SYS/SysGlobals.cpp
    N64SYS/N64DD.cpp
    N64SYS/N64Rom.cpp
    N64SYS/MainSYS.cpp
    N64SYS/MIPSY/R4300Instruction.cpp
    N64SYS/MIPSY/Register.cpp
    N64SYS/MIPSY/Vmem.cpp
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
    N64SYS/Recomp/Recompiler.cpp
    N64SYS/Recomp/RecompilerMemory.cpp
)

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

# Create main executable
add_executable(nova64 ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(nova64 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Link AsmJit if available
if(asmjit_FOUND)
    target_link_libraries(nova64 PRIVATE asmjit::asmjit)
    message(STATUS "Linking with AsmJit")
endif()

# Compiler flags
if(MSVC)
    target_compile_options(nova64 PRIVATE /W4)
else()
    target_compile_options(nova64 PRIVATE -Wall -Wextra)
endif()

