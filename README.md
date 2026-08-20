# NovaStar64

NovaStar64 is a Nintendo 64 emulator written in modern C++20, built around a dynamic
recompiler, an HLE graphics pipeline, and a Vulkan/MoltenVK-based renderer. It began as a
fork of [Project64](https://github.com/project64/project64) and has since had large parts
of the CPU core, RSP, and graphics stack rewritten; it remains licensed under GPL-2.0 in
accordance with Project64's own license. See [LICENSE](LICENSE) for the full text and
[CONTRIBUTING.md](CONTRIBUTING.md) for how to get involved.

NovaStar64 is under active development. Expect bugs, missing features, and rough edges.

## Features

- **Dynamic recompiler** for x86-64 and ARM64 (Apple Silicon), with an interpreter
  fallback for debugging or unsupported paths.
- **HLE graphics pipeline** (`NovaGraphics`) emulating the RDP/RSP microcode
  (F3D/F3DEX/F3DEX2, S2DEX) with Vulkan (via MoltenVK on macOS) and OpenGL backends.
- **Experimental compute-based ray tracing** mode layered on top of the standard
  rasterized output (`NOVA64_RT=1`).
- **Launcher UI** ("NovaStar64 Dash Center") for managing a ROM library, an in-app Lua
  scripting console, a hex editor, RetroAchievements integration, and a hardware
  simulator view — see `nova64/UI/`.
- **Save states and cartridge save types** (EEPROM, FlashRAM/FRAM), plus partial N64DD
  support.

## Building

Requirements: CMake 3.10+, a C++20 compiler, and [Ninja](https://ninja-build.org/).
Most third-party dependencies (SDL2, Vulkan-Headers, volk, glslang, Dear ImGui, stb,
nlohmann/json, rcheevos, AsmJit) are fetched automatically via CMake's `FetchContent` on
first configure — an internet connection is required for that first build.

```bash
cd nova64
cmake -B build -G Ninja
cmake --build build
```

On Apple Silicon, `pip install cmake ninja` is a convenient way to get both tools if you
don't already have them. macOS has no system Vulkan/GL 4.6 support, so the renderer
always runs through Vulkan via the bundled MoltenVK (`nova64/thirdparty/MoltenVK`); this
is wired up automatically and needs no configuration. After every rebuild on macOS the
binary must be re-signed for the dynamic libraries to load correctly:

```bash
codesign --force --deep --sign - nova64/build/nova64
```

## Running

```bash
./nova64/build/nova64 path/to/rom.z64
```

Running with no ROM argument opens the launcher UI instead. Common flags:

```
--help, -h       Show this help message
--debug          Enable debug logging
--log <file>     Write log to file
--config <file>  Use specified config file
--frames <N>     Run for N frames (0 = unlimited, default)
--fps <N>        Target FPS (0=unlimited, 30, 60; default: 60)
--resolution <R> Window resolution: 480p, 720p, 960p, 1080p, 1440p, 4k (default: 960p)
--scale <N>      Internal resolution scale, 1-8x (overrides NovaGraphics.ini)
--fullscreen     Run in exclusive fullscreen mode
--windowed       Run in windowed mode (default: borderless fullscreen)
--interpreter    Force interpreter mode (slower, but better for debugging)
--recompiler     Force the dynamic recompiler (JIT) instead of the interpreter
--headless       Run without opening a window (useful with --pipe)
--pipe <name>    Connect to a named pipe for IPC commands/framebuffer
```

Set `NOVA64_RT=1` in the environment to enable the experimental ray tracer.

## Project layout

- `nova64/N64SYS/` — CPU core: MIPS interpreter, dynamic recompiler (x64/ARM64), memory
  handlers, plugin interfaces, save types.
- `nova64/NovaRSP/` — RSP HLE: microcode dispatch, audio/graphics ucodes.
- `nova64/NovaGraphics/` — RDP command processing, texture cache, and the
  Vulkan/OpenGL renderer backends.
- `nova64/NovaAudio/`, `nova64/NovaInput/` — audio and input plugins.
- `nova64/NovaLua/` — embedded Lua scripting engine.
- `nova64/UI/` — the launcher UI and hex editor.
- `nova64/common/` — shared utilities (strings, ini parsing, logging, paths).

## License

GPL-2.0. NovaStar64 is a derivative work of Project64 and is licensed under the same
terms; see [LICENSE](LICENSE).
