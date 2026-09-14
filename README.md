# NovaStar64

A Nintendo 64 emulator written in C++20, built around a dynamic recompiler, a
high-level-emulation (HLE) graphics pipeline, and a Vulkan/MoltenVK renderer.

NovaStar64 began as a fork of [Project64](https://github.com/project64/project64).
The graphics plugin, launcher UI, and audio plugin have since been written from
scratch; the CPU core and RSP remain substantially Project64-derived. The project
is licensed under **GPL-2.0** in accordance with Project64's license — see
[LICENSE](LICENSE), and [What is and isn't Project64-derived](#what-is-and-isnt-project64-derived)
below for the specifics.

NovaStar64 is under active development. Expect bugs, missing features, and rough
edges.

---

## Features

- **Dynamic recompiler** for x86-64 and ARM64 (Apple Silicon), with a MIPS
  interpreter fallback for debugging.
- **HLE graphics pipeline** (`NovaGraphics`) implementing the RDP pixel pipeline —
  colour combiner, blender, TMEM and all N64 texture formats, TLUT, mipmapping,
  both hardware dither patterns, fog, chroma key, YUV convert, and N64-accurate
  decal depth compare in generated shader code.
- **Broad microcode coverage**: F3D, F3DEX, F3DEX2/F3DZEX, S2DEX, Perfect Dark,
  plus roughly a dozen per-game variants (Diddy Kong Racing, Turbo3D, the L3D
  family, ZSortBOSS and others).
- **Vulkan and OpenGL backends.** macOS runs through Vulkan via the bundled
  MoltenVK, since macOS caps OpenGL at 4.1 and the GL path needs 4.6.
- **Experimental compute ray tracing** layered over the rasterised output
  (`NOVA64_RT=1`) — shadows and ambient occlusion.
- **Launcher UI** with a ROM library, Lua scripting console, hex editor,
  RetroAchievements integration, and a hardware simulator view.
- **Save states and cartridge saves** (EEPROM, SRAM, FlashRAM), Controller Pak,
  Rumble Pak, Transfer Pak, GameShark-style enhancements, and partial N64DD
  support.
- **NTSC and PAL timing.** Video field rate and the audio DAC clock are both
  derived from the ROM's region.

### Known gaps

- No netplay.
- Compatibility is routinely verified against only a handful of titles; there is
  no automated regression suite yet.
- The ray tracer is a research feature configured through environment variables
  rather than the settings UI.
- Audio `SetVolume`/`SetMute` and the buffer-level/sync API are accepted but not
  yet applied to the output stream.

---

## Building

Requirements: CMake 3.10+, a C++20 compiler, and [Ninja](https://ninja-build.org/).

Most third-party dependencies (SDL2, Vulkan-Headers, volk, glslang, Dear ImGui,
stb, nlohmann/json, rcheevos, AsmJit, Lua) are fetched automatically by CMake's
`FetchContent` on first configure, so the first build needs an internet
connection. Every dependency is pinned to an exact tag or commit — see
[CONTRIBUTING.md](CONTRIBUTING.md) before changing one.

```bash
cd nova64
cmake -B build -G Ninja
cmake --build build
```

### Apple Silicon

`pip install cmake ninja` is a convenient way to get both tools. Note that pip
installs them to a directory that is not on `PATH` by default — if `cmake` is
not found, or worse, if a scripted build appears to succeed while silently
skipping the configure step, add it first:

```bash
export PATH="$HOME/Library/Python/3.9/bin:$PATH"
```

After **every** rebuild on macOS the binary must be re-signed, otherwise the
dynamic libraries fail to load and the process hangs with no error:

```bash
codesign --force --deep --sign - nova64/build/nova64
```

---

## Running

```bash
./nova64/build/nova64 path/to/rom.z64
```

Running with no ROM argument opens the launcher UI.

```
--help, -h       Show this help message
--debug          Enable debug logging
--log <file>     Write log to file
--config <file>  Use specified config file
--frames <N>     Run for N frames (0 = unlimited, default)
--fps <N>        Target FPS (0 = unlimited, 30, 60; default: 60)
--resolution <R> Window resolution: 480p, 720p, 960p, 1080p, 1440p, 4k
--scale <N>      Internal resolution scale, 1-8x (overrides NovaGraphics.ini)
--fullscreen     Exclusive fullscreen
--windowed       Windowed mode
--interpreter    Force interpreter mode (slower, better for debugging)
--recompiler     Force the dynamic recompiler (JIT)
--headless       Run without showing a window
--pipe <name>    Connect to a named pipe for IPC commands/framebuffer
```

Graphics settings live in `nova64/settings/NovaGraphics.ini`.

### Diagnostics

Many subsystems expose environment-variable-gated tracing, intended for
debugging rather than normal use. `NOVA64_RT=1` enables the ray tracer;
`NOVA64_TURBO=1` uncaps the frame rate; `NOVA64_SHOT_EVERY=N` writes a
screenshot every N frames; `NOVA64_LOG_LEVEL` controls log verbosity. Grep for
`NOVA64_` in the source for the full set.

---

## Project layout

| Path | Contents |
|---|---|
| `nova64/N64SYS/` | CPU core: MIPS interpreter, dynamic recompiler (x64/ARM64), memory and hardware-register handlers, plugin interfaces, save types |
| `nova64/NovaRSP/` | RSP: interpreter, recompiler, and HLE audio/graphics microcode dispatch |
| `nova64/NovaGraphics/` | RDP command processing, microcode handlers, texture cache, Vulkan/OpenGL backends, ray tracer |
| `nova64/NovaAudio/` | Audio plugin (AI DMA bridge, SDL2 output) |
| `nova64/NovaInput/` | Input plugin |
| `nova64/NovaLua/` | Embedded Lua scripting engine |
| `nova64/UI/` | Launcher UI and hex editor |
| `nova64/common/` | Shared utilities (logging, INI parsing, file I/O, platform macros) |

---

## What is and isn't Project64-derived

This matters if you plan to redistribute, relicense, or reimplement any part of
the project, so it is stated precisely rather than in general terms.

| Subsystem | Status |
|---|---|
| `NovaGraphics/` | **Original.** No Project64-derived files. |
| `UI/` | **Original.** |
| `NovaAudio/` | **Original.** |
| `NovaRSP/` | Substantially Project64-derived. |
| `N64SYS/` | Roughly 40% of files are Project64-derived, concentrated in the recompiler, interpreter, and plugin layers. |
| `common/` | One remaining Project64-derived file (`MemoryManagment.cpp`). |

Files carrying Project64 code state so in their file header. Those headers are
required by GPL-2.0 § 1 and must not be removed — removing them does not change
what the code is derived from, it just makes the project non-compliant.

GPL-2.0 permits commercial sale. What it requires is that recipients get the
source and the same rights; it does not permit shipping this closed-source. If
you want a proprietary derivative, the Project64-derived subsystems have to be
replaced by genuine clean-room reimplementation — see
[docs/REIMPLEMENTATION.md](docs/REIMPLEMENTATION.md).

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GPL-2.0. NovaStar64 is a derivative work of Project64 and is licensed under the
same terms; see [LICENSE](LICENSE).
