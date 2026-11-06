# The RSP recompiler

The RSP is the N64's vector coprocessor. In HLE mode NovaStar64 skips it for the graphics and audio microcode it
recognises, but in **LLE mode** (`renderingMode = 1` in `NovaGraphics.ini`) the real microcode runs on it every
frame, and the interpreter is then the biggest single cost on the emulation thread. The recompiler translates that
microcode to native code and runs it instead.

| | |
|---|---|
| Engines | `Interpreter` (the reference) and `Recompiler` (the default) |
| Recompiler runs on | AArch64 (Apple Silicon) and x86-64 |
| Pick one | Launcher: Settings → Emulation → RSP engine. Or `--rsp-recompiler` / `--rsp-interpreter`, or `NOVA64_RSP_ENGINE=recompiler\|interpreter` |
| Results | Identical to the interpreter's, down to every register, memory byte and flag (see [Verification](#verification)) |
| Speed | The RSP itself runs about 5x faster (SM64 in LLE, M4 Pro: 5.2 s → 1.0 s per 2,400 frames; Majora's Mask 2.0 s → 0.5 s) |

The rest of the cost of LLE is the RDP drawing what the RSP hands it, which this does not change. `NOVA64_RSP_TIME=1`
prints the two separately. In HLE mode (the default rendering mode) the games tested never run the RSP engine at all,
because their graphics and audio microcode are handled at a higher level, so the setting only matters for LLE and
for microcode HLE doesn't recognise.

## How it works

```
 DoRspCycles ──► CRSPSystem::Run ──► RspJit::Run  ──► block ──► block ──► ...   (until the RSP halts)
                                       │   ▲
                         Discover(pc)  │   │ code cache: one table per IMEM image
                                       ▼   │
                                    Backend::Compile (RspJitA64.cpp / RspJitX64.cpp)
```

* **Blocks.** Code is compiled one basic block at a time, the first time execution reaches it. A block ends at a
  branch or jump (plus its delay slot), at `BREAK`, or after an `MTC0` (which can halt the RSP or start a DMA that
  rewrites IMEM).
* **Delay slots follow the interpreter exactly.** A branch's condition is decided *before* its delay slot runs. If the
  delay slot is itself a branch, the first branch is overruled: only its link-register write survives, and the second
  runs as an ordinary branch with its own delay slot.
* **Code cache.** Games switch between a handful of microcodes (and overlays of them) many times a frame, so compiled
  code is kept per *IMEM image*, found by hashing IMEM when a task starts and again after any SP DMA into IMEM. Going
  back to a microcode already seen costs a table lookup, not a recompile.
* **What is translated natively.** Scalar arithmetic, shifts, loads, stores, branches and jumps are emitted as machine
  code. An unaligned load or store calls the interpreter for that one instruction.
* **Vector instructions** (every computation, load and store) call a *kernel* (`RspJitKernels.cpp`): the whole
  8-lane operation written once with compiler vector types, which become NEON or SSE.
* **Everything else** (`MTC0`, `MFC0`, an unaligned scalar load or store) calls `CRSPSystem::ExecuteOne`, which runs
  that one instruction on the interpreter. So the recompiler is exact by construction, and any instruction a future
  change leaves untranslated is still correct. `NOVA64_RSP_JIT_STATS=1` lists what goes through the interpreter, most
  frequent first.
* **If a block can't be compiled** the interpreter finishes the task from that point.

The recompiler only handles a run with no cycle budget (the normal case) and never runs while the RSP debugger is
attached; the interpreter handles those.

## Verification

`nova64 --rsp-selftest` generates random but well-formed RSP programs and runs each one on two independent RSP
systems, one per engine, then compares the complete state: scalar and vector registers, accumulator, all five flag
registers, the reciprocal state, DMEM, IMEM, RDRAM and the SP and DPC registers. The programs cover every scalar
instruction, branches and jumps (forward, counted loops, and a branch in a delay slot), `MFC0`/`MTC0` with real DMA
(including DMA over the program itself), every vector operation with every element selector, and every vector load
and store. State is random, so flag and saturation corner cases come up constantly.

```bash
./nova64/build/nova64 --rsp-selftest                         # 2,000 programs
./nova64/build/nova64 --rsp-selftest --seed 7 --programs 20000
./nova64/build/nova64 --rsp-selftest --seed 7 --only 1234 --verbose   # reproduce and print one failing program
```

The test is checked the other way round too: planting a bug in an instruction makes it fail within a few dozen
programs. Beyond it, headless LLE runs of Super Mario 64, Ocarina of Time and Majora's Mask produce byte-identical
screenshots on both engines, on ARM64, and on x86-64 (built for Intel and run under Rosetta on an Apple Silicon Mac:
`cmake -S nova64 -B build-x64 -DCMAKE_OSX_ARCHITECTURES=x86_64`, then `arch -x86_64 build-x64/nova64 ...`). CI runs the
self-test on every push.

## Adding to it

* **A vector kernel.** Add the function to `RspJitKernels.cpp` and its entry to `Tables`. Write it with the `V8s` /
  `V8w` / `V8l` types and `Cvt`/`Bc`, not compiler builtins, so it works on both paths. Read the interpreter's
  version in `RSPInterpreterOps.cpp` first and match it exactly, quirks included; the interpreter stays as the
  reference. Run `--rsp-selftest` with a few seeds.
* **A code emitter for another CPU.** Implement `Backend` (`RspJitBackend.hpp`): turn one `Block` into a function that
  takes the `JitContext` and returns the next PC. Copy the structure of `RspJitA64.cpp` or `RspJitX64.cpp`; the
  front end, cache and test are shared.

## Limits

* The kernels use GCC/Clang vector types where they exist. Elsewhere (MSVC) they use `Lanes<>`
  (`RspSimdLanes.hpp`), the same operators written as plain loops, which is correct everywhere and about half as fast
  (SM64 in LLE: the RSP takes 1.9 s instead of 1.0 s, against 5.2 s on the interpreter). `-DNOVA64_RSP_PORTABLE_KERNELS=ON`
  forces that path on a Clang machine so it can be run through `--rsp-selftest`; CI does this on macOS.
* Platforms without an emitter (32-bit, RISC-V, ...) use the interpreter automatically.
