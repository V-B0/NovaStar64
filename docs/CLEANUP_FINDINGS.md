# Findings from the code review

What a file-by-file review of the whole tree turned up besides comments. Nothing here has been
changed yet, and **none of it has been confirmed**: each item is something a reader noticed and
should be checked before it is acted on. "Dead" means a search found no caller outside the
defining file; confirm with the linker (`-Wl,-dead_strip -Wl,-map`) and a build before deleting.

Deleting dead code is a code change, so each batch gets the checks in
[RELEASE_PLAN.md](RELEASE_PLAN.md): a clean build on every CPU, the RSP selftest, and
unchanged frames from the three test games.

## Suspected bugs

Worth a look first, because a real bug is worth more than tidy code.

| Where | What looks wrong |
|---|---|
| `NovaGraphics/Plugin/NovaGraphicsGFXPlugin.cpp:360` | `VI__X_SCALE_REG` and `VI__Y_SCALE_REG` index VI registers 10 and 12; X scale is register 12 and Y scale 13 (10 is V_START) |
| `NovaGraphics/Common/ClipMath.cpp:509` | `ShouldCullTriangle`: the winding convention for "front face" and which sign `cull back` culls look inconsistent |
| `NovaGraphics/Common/ClipMath.cpp:355` | `ComputeGuardBandFlags` hard-codes 320x240, so guard bands for other resolutions are wrong |
| `NovaGraphics/Core/DepthBuffer.cpp:31` | The comment's step table is the reverse of what the code builds (`1 << (shift + 2)` with shifts `{6,5,4,3,2,1,0,0}`) |
| `NovaGraphics/Core/RDPProcessor_Texture.cpp:94` | Described as 10.2 fixed point, but the code divides by 1024 |
| `NovaGraphics/Microcode/Microcode.hpp:105` | `GBI1::G_LINE3D = 0xB6` collides with `G_CLEARGEOMETRYMODE = 0xB6` |
| `NovaGraphics/Common/N64Types.hpp` | `TextureFilter` names (`Average = 2`, `Bilerp = 3`) look swapped against `gbi.h` |
| `NovaGraphics/Debug/ImGuiDebugUI.cpp:795` | The G_TEXTURE_ENABLE row tests `gm & 0x00000000`, always false |
| `NovaGraphics/Common/BitUtils.cpp:98` | `CRC32_Fast` gives CRC32C with SSE4.2 and IEEE CRC32 without, so results differ between x86 and ARM builds (matters if a hash is stored) |
| `NovaGraphics/Renderer/ShaderCache` | `SaveBinaryCache` writes a `CombinerKey` including uninitialised padding, so the cache file is not deterministic |
| `NovaGraphics/Renderer/VK/VKShaderCompiler.cpp:57` | A truncated 32-bit pointer is used as the shader handle, so two programs could collide |
| `NovaGraphics/Renderer/VK/VKShaderCompiler.cpp` | `CompileWithGeometry` drops the geometry stage on Vulkan |
| `NovaGraphics/Core/RDPCommand.cpp` | `RDPCommandBuffer::Push` can write past capacity after the overflow reset |
| `NovaRSP/CPU/RspSystem.cpp:175` | The 5-million-instruction runaway guard only advances while the LLE trace is on, so it does nothing otherwise |
| `NovaRSP/HLE/hle.cpp` | A task that fails fast audio dispatch is identified and counted twice |
| `NovaRSP/HLE/alist.cpp:999` | `alist_chorus` LFO arithmetic does not centre on zero as intended (only reachable if the function is used, see Dead code) |
| `N64SYS/Recomp/ExitInfo.cpp:237` | `predicted_taken = execution_count > execution_count / 2` is always true above 10 |
| `N64SYS/Recomp/x64-86/x64RegInfo.cpp:40` | The volatile-register mask treats RBX as volatile, against its own comment |
| `N64SYS/Recomp/CodeBlock.cpp:742` | When an instruction fails to compile it is silently skipped (an old comment claimed a NOP was emitted) |
| `N64SYS/Recomp/CodeSection.cpp:1122` | `result.has_loop = result.has_loop;` |
| `N64SYS/MIPSY/GBcart.cpp:196` | `last_latch_val` is a function-local static, so every MBC3 cartridge shares it; and `rtc_latched` is toggled then immediately overwritten |
| `N64SYS/MIPSY/Register.hpp` | `COP0EntryHi::Raw` disagrees with its accessors (bit positions of R and FILL) |
| `N64SYS/Memoryhandler/UnCachedSysMemory.cpp:35` | `AUTO_DETECT_THRESHOLD` equals `SRAM_MAX_OFFSET`, so the SRAM auto-detect path looks unreachable |
| `N64SYS/MIPSY/Mempak.cpp` | Note table and iNode table offsets do not match the real Controller Pak layout (module is unused today) |
| `N64SYS/MainSYS.cpp:1437` | A hard-coded Super Mario 64 workaround (a PC and RDRAM addresses) for an SI/PIF boot-timing race; the race itself is not fixed |
| `common/DynamicLibrary.hpp` | `RTLD_*` values use glibc's numbers but are passed straight to `dlopen`; wrong on macOS |
| `common/LockGuard.hpp` | `ScopedLock::operator=(ScopedLock&&)` assigns through a reference whose assignment is deleted (would not compile if used) |
| `common/Thread.cpp` | POSIX `Terminate()` can race with the thread deleting its own id; a cancelled thread leaks its handle |
| `common/trace.cpp` | `MemoryTraceSink::GetCount` reads the vector size without the lock |
| `NovaLua/LuaEngine.cpp:242` | `input.get_buttons()` always returns 0 (TODO), though documented as the button mask |

## Dead code

Candidates for deletion, in rough order of size.

**N64SYS**
- `MIPSY/DDisk.*`, `MIPSY/Mempak.*`, `MIPSY/TransferPak.*`, `MIPSY/GBcart.*`, `MIPSY/Rumblepak.*`:
  only included, never used, as far as a search shows. Check what the launcher's Simulator page relies on first.
- `MIPSY/TLB.*`: `TranslationLookasideBuffer`, `TlbManager`, `GlobalTlb`, `TlbUtils` (`CTLB` is the live class).
- `MIPSY/Systiming.hpp`: `CSystemTimer` is declared but never defined; `TimerManager` and friends unused.
- `Recomp/FunctionInfo.*`, `Recomp/JumpInfo.*`, most of `Recomp/FunctionMap`, `Recomp/RegBase.cpp` (three
  functions), `RecompilerOpsBase`/`CreatePlatformRecompilerOps`, `CodeSection::generate_native_code`,
  `find_existing_section`, and the `TranslateInstruction`/`Handle*` family in `x64RecompilerOps.cpp`
  (lines 1239-1330) are unreachable. `schedule_instructions` swaps instructions with no hazard
  checks, so it must never be reached.
- `Recomp/x64-86/x64RecompilerOps.cpp:63`: the `FORCE_CALLBACK_*` bisecting switches (all 0).
- `Recomp/CodeBlock.cpp`: a 32-bit x86 path that includes headers which do not exist.
- `Memoryhandler`: no-op persistence stubs (`SaveDiskSettings`, `SaveRomSettingID` and similar), and three
  different 64DD models (`CachedSysMemory`, `DDisk`, `N64DD`).

**NovaGraphics**
- `Common/Memory.*` (allocators, `LeakDetector`), most of `Common/ClipMath` (only `ClipTriangle` is used),
  `Common/VectorMath` helpers, the unused `CRC32_SIMD`, and the AVX-512 detection with no kernels.
- `Renderer/GL/GLShaderProgram.*`: `GLProgramPipeline`, `ShaderIncludeManager`, `ShaderPreprocessor`,
  `ShaderHotReloader`.
- `Renderer`: `IsOpenGL46Available`, `GenerateLODCode`, `GenerateDetailSharpenCode`, empty `ResetStats`/`OnFrameBegin`.
- `Core`: `RDPCommandBuffer` (only `Reset` is called), `NeedsBatchFlush`, `FORCE_RED_DEBUG`, an
  `if (false && ...)` block of about 20 lines in `RDPProcessor_Batch.cpp:1749`, unused decoder helpers.
- `Plugin/VideoPlugin.cpp`: `IsFramebufferValid`, `EnableWireframe`, `EnableTextureDebug`, `LockRender`, `DumpFrameInfo`.
- `Compat/GameDatabase.cpp`: the `GAME_CRC`/`GAME_HACK` macros, defined and undefined at once.

**NovaRSP**
- `HLE/alist.cpp` 918-1153 (about 230 lines): `alist_deinterleave`, `alist_compress`, `alist_chorus`,
  `alist_delay`, `alist_hilopass`, `alist_pan`, `alist_dc_remove`.
- `CPU/RspLog.*`, `CPU/RspClamp.hpp` (duplicates `HLE/arithmetics.hpp`), the profiling counters and sync-CPU
  group in `CPU/RspSystem.cpp`, `RSPInfo.hpp`'s `RspRomClosed`/`FreeRSP`, several `RspSettings` getters.
- `HLE`: `Crc32::AdditiveChecksum`, `IsAudioUcode`/`IsGraphicsUcode`, `HLETaskBooter`, two identical CRC helpers
  in `TaskDispatcher.cpp`, write-only counters.

**common**
- The trace system has no sinks registered anywhere, so `WriteTrace` and `TRACE_ERROR`/`TRACE_WARN` log
  nowhere; `FileTraceSink`, `ConsoleTraceSink`, `MemoryTraceSink`, `TRACE_LOG`, `TRACE_IF` are unused.
  `Logging::Logger` is an empty stub. `DynLibHandle` is unused.

## Debug output left in

Raw prints that run in normal use, or diagnostics nobody switches on any more. Route what stays through the
logger and delete the rest.

- Always on: `[RDP-HISTO]`, `[RDPTRI#]`, `[RAWZ]`, `[GPURANGE#]`, `[GPUV#]`, `[ORTHO]`, `[DEPTHTRACE]` in the RDP code;
  `VPLLE` and `RENDER[60f avg ms]` in `VideoPlugin.cpp`; `[AI]` prints in `AudioInterfaceHandler.cpp`;
  `PERF[...]`/`SPIKE` lines in `MainSYS.cpp`; `VKEndFrame[...]` in `VKBackend.cpp`; `[SHADER-FAIL]`; one
  WARN per compiled shader; `[DIV_D_DEBUG]` in the interpreter; `Log::infof` every 100000 blocks in `Recompiler.cpp`.
- Behind environment variables: `NOVA64_TRI_DUMP`, `NOVA64_F3D_VTRACE`, `NOVA64_FORENSIC`, `NOVA64_TEXDBG`,
  `NOVA64_WATCH_*`, `NOVA64_DUMP_RDRAM`, and the always-active diagnostic hotkeys F7, F8, F11, Ctrl+D in
  `WindowManager.cpp`.
- Hard-coded: `wantValidation = false` in `VKBackend.cpp`, so the validation-layer path never runs.

## Smaller things

- `RendererFactory.cpp:22` says "Vulkan 1.1"; the rest says 1.2.
- `main.cpp`: `--scale` is accepted but missing from `PrintUsage`.
- `MainSYS.cpp:1560`: `s_UseInterpreter` defaults to true off x86; revisit now that the ARM64 recompiler exists.
- `GL/GLBackend.cpp`: an incomplete framebuffer is silently ignored.
- `VKBackend.cpp` is 3,600 lines in one class and the only file with a UTF-8 byte-order mark.
- The MP3 and MusyX HLE are approximations and may not be accurate for real games.
