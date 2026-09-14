# Contributing to NovaStar64

Thanks for your interest. NovaStar64 is a hobby-scale N64 emulator under heavy
development. Please open an issue or discussion before starting anything large so
we can agree on direction first.

## Getting set up

See [README.md](README.md) for full build instructions.

```bash
cd nova64
cmake -B build -G Ninja
cmake --build build
codesign --force --deep --sign - build/nova64   # macOS, after every rebuild
```

## Branches

- `main` — release branch; stable, playable builds.
- `devbuild` — active development. Target pull requests here unless asked otherwise.

---

## Testing your change

Emulator bugs regress silently: the build stays green, the ROM still boots, and
one effect somewhere renders wrong. Compiling is not evidence that a change
works.

At minimum, run a few commercial ROMs headless and confirm a clean exit:

```bash
SDL_AUDIODRIVER=dummy ./build/nova64 path/to/rom.z64 --frames 120
```

`SDL_AUDIODRIVER=dummy` matters — without it, headless runs leave audio
threads producing sound.

### The divergence harness

For anything touching CPU, RSP, or timing, use the state trace. It records a
digest of architectural state once per VI frame so two runs can be diffed:

```bash
NOVA64_TRACE_STATE=/tmp/before.trace ./build/nova64 rom.z64 --frames 300
# ...make your change, rebuild...
NOVA64_TRACE_STATE=/tmp/after.trace  ./build/nova64 rom.z64 --frames 300
scripts/compare-trace.py /tmp/before.trace /tmp/after.trace
```

It exits non-zero on divergence, so it works directly as a CI check. Emulation
is deterministic, so **two runs of the same build must produce identical
traces** — if they don't, something has reintroduced a nondeterminism source
(wall-clock time or a PRNG leaking into emulated state), which is a bug in its
own right.

The same harness compares the two execution engines
(`--interpreter` vs `--recompiler`). Read the `ALIGNMENT` line first: the engines
advance in different granularities, so frames sampled at different CP0 `Count`
values are inconclusive. Only frames the script reports as `CONFIRMED` — same
`Count`, different state — are real disagreements.

Useful extras: `NOVA64_TRACE_RDRAM=1` also hashes RDRAM each frame (slow),
`NOVA64_TRACE_CP0=1` emits a per-register CP0 breakdown for pinpointing which
register diverged, and `NOVA64_TRACE_FRAMES=<n>` caps the trace length.

### Interrupt timing: both engines service the timer at the same boundary

**Instruction cycle counting is exact** — both engines charge `CountPerOp` per
instruction, and `SystemTimer::TimerDone()` fires at byte-identical `Count` on
both. There has never been a miscount.

What used to diverge was *when the interrupt got delivered*. The interpreter
only checked for pending interrupts on backward branches; the recompiler only at
block exits. The timer fired at the same instant on both, but the CPU entered
the ISR tens of cycles apart, so the game read a different `Count` and wrote a
different CP0 `Compare`.

Both now service the timer on the instruction it actually expires on:

- `R4300iInterpreter::ExecuteOps` checks `NextTimer <= 0` in the
  `PIPELINE_STAGE_NORMAL` case, i.e. at every straight-line instruction.
- `ArmRecompilerOps::EmitTimerDecrementAndCheck` folds a compare into the
  per-instruction timer decrement and leaves the block with the PC pointed at
  the next instruction.

Two rules matter if you touch this:

- **Never take a timer exit on a branch or its delay slot.** The branch still
  has to run its delay slot, and after the delay slot the branch callback owns
  the PC — writing `addr + 4` there would clobber the jump target. `CodeBlock`'s
  ARM64 loop excludes both.
- **A mid-block exit must write the resume PC itself.** `ExitCodeBlock()` only
  flushes registers and returns; it does not set PC. The exception path gets
  away with it because the exception handler supplies the PC.

Measured effect (150 frames, interpreter vs recompiler, diverging frames):
SM64 35 → 22, OOT 103 → 38, MM 114 → 53. No measurable performance cost.

### Spin-loop detection

Both engines treat exactly one thing as a spin loop: a branch back to its own
address. The interpreter tests `offset == 0` in
`CN64System::DelayedRelativeJump`; the recompiler sets `BlockFlags::HasLoop`
during analysis and the dispatcher queries `CodeBlock::is_spin_loop()`.

Keep those two in step. The recompiler previously used a size heuristic
(`instrCount <= 2`), which would happily fast-forward a short loop that was
doing real work, and it separately required that the delay slot could not affect
the branch condition — a stricter-sounding rule that in practice rejected nearly
every real spin loop, since the usual pattern reloads its flag in the delay
slot. Skipping ahead is correct for those: the flag is written by an interrupt
handler or a DMA completion, and both are timer events.

If you ever refine this (for example to spot a self-terminating loop that
counts down in its delay slot), change both engines in the same commit.

### What still diverges

CP0 `Compare` on a handful of frames. `Compare` is written by the game from the
`Count` it just read, so it is the most sensitive indicator left. Aligning
spin-loop detection did **not** move this — measured identical before and after
— so the cause is something else still unidentified. Use
`NOVA64_TRACE_EVENTS=1` and compare `cmpw` records to see the `Count` at each
write.

Then, depending on what you touched:

- **CPU or RSP execution** — test with both `--interpreter` and `--recompiler`.
  They must agree.
- **Graphics** — test with at least one ROM that actually exercises the path you
  changed, and compare frames before and after. `NOVA64_SHOT_EVERY=N` writes
  periodic screenshots.
- **Timing, audio, or video** — test a **PAL** ROM as well as an NTSC one. The
  two regions use different video field rates and a different audio DAC clock,
  and region handling has been a recurring source of bugs.

Say what you tested in the pull request description.

---

## Things that have bitten us before

These are real failure modes from this codebase, not hypotheticals.

**Prove code is reachable before you fix it.** Several subsystems here are
wired up but never actually called. Before investing in a fix, confirm the code
runs at all — add a counter or a log line and watch it fire. Removing a source
file from the build and checking that it still links is a reliable deadness test.

**macOS filesystems are case-insensitive.** `RspTypes.hpp` and `RSPTypes.hpp` are
the same file. A case-insensitive search may report a header as unused when it is
included everywhere under different casing. Check both spellings before deleting
anything.

**Never leave a dependency on a moving tag.** Every `FetchContent_Declare` must
pin an exact tag or commit SHA. An unpinned `GIT_TAG master` silently broke the
ARM64 recompiler when AsmJit marked `a64::Assembler` as `final` upstream — the
project built fine until the next reconfigure. If you bump a dependency, do it
deliberately, in its own commit, and say why.

**A build that "succeeds" may not have configured.** If `cmake` is not on `PATH`,
a `cmake .. && ninja` chain short-circuits and Ninja happily relinks stale object
files — including sources you just deleted. Verify the configure step actually
ran.

**Region matters.** If you add a hardware constant, check whether it differs
between NTSC and PAL before hardcoding it.

**Config values may carry inline comments.** The project's own `.ini` files use
`key = 1  # explanation`. Parsers must tolerate that.

---

## Code style

- C++20. Match the style of the file you're editing.
- Comments explain **why**, not what. A comment that restates the code is noise;
  a comment recording a hardware quirk, a decompilation finding, or the root
  cause of a past bug is the most valuable thing in the file. Do not delete the
  latter while tidying.
- Don't leave thinking-out-loud comments ("does this start DMA? docs disagree?")
  in merged code. Resolve the question, then state the answer.
- Keep pull requests focused. Several small reviewable ones beat one large one.
- Gate diagnostic tracing behind an environment variable rather than a permanent
  code path, and remove it once the underlying bug is fixed.

---

## Project64-derived code

Large parts of NovaStar64 — the CPU core, the RSP, and some of `common/` —
originate from [Project64](https://github.com/project64/project64), which is
GPL-2.0 licensed. `NovaGraphics/`, `UI/`, and `NovaAudio/` are original work and
should stay that way.

If you port or adapt code from Project64 or any other GPL-2.0 codebase:

- Say so in the pull request description.
- Add a file-level attribution header:

```cpp
/**
 * @file MyFile.cpp
 * @brief One-line description
 *
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 */
```

**Do not remove existing attribution headers.** They are required by GPL-2.0 § 1.
Removing one does not make the code any less derived — it just makes the project
non-compliant.

Please don't introduce *new* Project64-derived code into `NovaGraphics/`, `UI/`,
or `NovaAudio/`. Those subsystems are currently clean, and keeping them that way
preserves options. If you want to help remove the existing dependency, read
[docs/REIMPLEMENTATION.md](docs/REIMPLEMENTATION.md) first — the rules about who
may write replacement code are stricter than they look.

All contributions are accepted under the project's GPL-2.0 license.

---

## Reporting bugs

Open a GitHub issue with:

- The ROM (name and **region**), and whether you used the interpreter or recompiler.
- Steps to reproduce.
- A screenshot or capture for graphical issues.
- Your OS, and on macOS whether you're on Apple Silicon or Intel.
