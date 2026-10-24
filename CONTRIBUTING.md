# Contributing to NovaStar64

Thanks for your interest. NovaStar64 is a hobby-scale N64 emulator under heavy
development. For anything larger than a small fix, please open an issue or
discussion first so we can agree on direction before you invest the time.

**On this page:**
[Getting set up](#getting-set-up) ·
[Pull requests](#pull-requests) ·
[Testing your change](#testing-your-change) ·
[Making an installer](#making-an-installer) ·
[Diagnostic variables](#diagnostic-variables) ·
[Things that have bitten us before](#things-that-have-bitten-us-before) ·
[Code style](#code-style) ·
[Project64-derived code](#project64-derived-code) ·
[Reporting bugs](#reporting-bugs)

## Getting set up

See [README.md](README.md#getting-started) for requirements. In short:

```bash
./build.sh            # checks the tools, fetches the dependencies, builds nova64/build/nova64
./build.sh --run      # ... and opens the launcher
```

Run it again after a change and only what changed is rebuilt. The manual equivalent is
`cmake -S nova64 -B nova64/build -G Ninja && cmake --build nova64/build`. A build puts a
`portable.txt` beside the executable, so it keeps its settings, games and saves in
`nova64/build/` (see [Configuration](README.md#configuration)).

On macOS the build signs the binary and sets its Finder icon (see
`scripts/set-macos-file-icon.sh`); there is no separate `codesign` step.

You need your own ROM images to test with. None are included, and `roms/` and
`saves/` are git-ignored — keep it that way.

The [project layout](README.md#how-it-works) in the README maps the source
directories to subsystems.

## Pull requests

- **Branches.** `main` is the release branch: stable, playable builds. `devbuild`
  is active development; target pull requests there unless asked otherwise.
- **Keep them focused.** Several small reviewable pull requests beat one large
  one. A dependency bump goes in its own commit, with the reason.
- **Say what you tested** in the description — which ROMs, which engine
  (`--interpreter`, `--recompiler` or both), which region. See
  [Testing your change](#testing-your-change) for what to run for each kind of
  change.
- **Say if code came from elsewhere.** If you port or adapt code from Project64
  or any other GPL codebase, say so in the description and add the attribution
  header described in [Project64-derived code](#project64-derived-code).
- **Explain the why.** A commit message or comment that records the root cause of
  a bug is worth far more than one that restates the diff.

---

## Testing your change

Emulator bugs regress silently: the build stays green, the ROM still boots, and
one effect somewhere renders wrong. Compiling is not evidence that a change
works.

At minimum, run a few commercial ROMs headless and confirm a clean exit:

```bash
SDL_AUDIODRIVER=dummy ./build/nova64 path/to/rom.z64 --headless --frames 120
```

`SDL_AUDIODRIVER=dummy` matters — without it, headless runs leave audio threads
producing sound. `--headless` opens no window and does not take keyboard focus.

Then, depending on what you touched:

- **CPU or RSP execution** — test with both `--interpreter` and `--recompiler`,
  and compare them with the state trace below.
- **Graphics** — test with at least one ROM that actually exercises the path you
  changed, and compare frames before and after (`NOVA64_SHOT_EVERY=N` writes
  periodic screenshots).
- **Timing, audio or video** — test a **PAL** ROM as well as an NTSC one. The two
  regions use different video field rates and a different audio DAC clock, and
  region handling has been a recurring source of bugs. Also run the
  [absolute timing check](#timing-count-is-the-unit-of-time).
- **Save states or rewind** — see [Save states and rewind](#save-states-and-rewind).

### The state-trace harness

For anything touching CPU, RSP or timing, use the state trace. It records a
digest of architectural state once per VI frame so two runs can be diffed:

```bash
NOVA64_TRACE_STATE=/tmp/before.trace ./build/nova64 rom.z64 --headless --frames 300
# ...make your change, rebuild...
NOVA64_TRACE_STATE=/tmp/after.trace  ./build/nova64 rom.z64 --headless --frames 300
scripts/compare-trace.py /tmp/before.trace /tmp/after.trace
```

It exits non-zero on divergence, so it works directly as a CI check. Emulation
is deterministic, so **two runs of the same build must produce identical
traces** — if they don't, something has reintroduced a nondeterminism source
(wall-clock time or a PRNG leaking into emulated state), which is a bug in its
own right.

The same harness compares the two execution engines (`--interpreter` against
`--recompiler`). Read the `ALIGNMENT` line first: frames sampled at different CP0
`Count` values are inconclusive. Only frames reported as `CONFIRMED` — same
`Count`, different state — are real disagreements.

Useful extras: `NOVA64_TRACE_RDRAM=1` also hashes RDRAM each frame (slow),
`NOVA64_TRACE_CP0=1` emits a per-register CP0 breakdown so you can see which
register diverged first, `NOVA64_TRACE_EVENTS=1` records timer and interrupt
events with their `Count`, and `NOVA64_TRACE_FRAMES=<n>` caps the trace length.

### Timing: `Count` is the unit of time

Everything in the emulated machine is scheduled in CP0 `Count` ticks: the VI,
AI, PI, SI and RSP events and the Compare interrupt all go through
`SystemTimer`, and `m_NextTimer` counts down toward the next one, decremented by
`CountPerOp` (2) per instruction. Four rules follow from that, each learned from
a real bug.

**1. Timer periods are already in `Count` ticks — don't scale them by
`CountPerOp`.** The VI timer was once scheduled as `781250 * CountPerOp()`. The
constant is already `Count` ticks per NTSC field (93.75 MHz ÷ 2 ÷ 60), so the
field came out twice as long in `Count` terms. Wall-clock frame pacing in
`RefreshScreen()` still held real output at 60 fps, so video, movement and
per-frame game logic all looked perfect — but every game clock that reads
`Count` (`osGetTime()`) ran twice as fast, which showed up as a minigame
stopwatch running at double speed. It was invisible to every cross-engine
comparison because both engines had the same wrong constant.

**2. Both engines must service the timer on the instruction it expires on.**
`R4300iInterpreter::ExecuteOps` checks `NextTimer <= 0` in the
`PIPELINE_STAGE_NORMAL` case, at every straight-line instruction;
`ArmRecompilerOps::EmitTimerDecrementAndCheck` folds a compare into the
per-instruction decrement and leaves the block with the PC pointing at the next
instruction. If they deliver an interrupt at different instructions, the game
reads a different `Count`, writes a different CP0 `Compare`, and the two engines
walk away from each other. Two corollaries:

- Never take a timer exit on a branch or its delay slot. The branch must still
  run its delay slot, and afterwards the branch callback owns the PC — writing
  `addr + 4` there would clobber the jump target.
- A mid-block exit must write the resume PC itself. `ExitCodeBlock()` only flushes
  registers and returns.

**3. An idle-loop fast-forward must finish with `TimerDone()` in both engines.**
Both engines skip a spin loop by forcing `NextTimer` negative and calling
`UpdateTimers()`. That call can fire a periodic timer (VI, AI DMA) whose handler
reschedules itself, and rescheduling rewrites `NextTimer` — leaving it positive
again. The interpreter's version (`InPermLoop`, serviced from
`PIPELINE_STAGE_PERMLOOP_DELAY_DONE`) always calls `TimerDone()` afterwards,
which is what re-syncs everything. The recompiler used to gate that call on
`NextTimer < 0`, so whenever a handler had rescheduled, the timer bookkeeping was
silently skipped and the two engines forked completely within twenty frames of
booting Super Mario 64.

**4. Both engines treat exactly one thing as a spin loop: a branch back to its
own address.** The interpreter tests `offset == 0` in
`CN64System::DelayedRelativeJump`; the recompiler sets `BlockFlags::HasLoop`
during analysis and the dispatcher queries `CodeBlock::is_spin_loop()`. Keep the
two in step, and if you ever refine the rule, change both engines in the same
commit.

**The absolute check.** Cross-engine agreement only proves the engines agree,
not that they are right (see rule 1). After a run of *N* video fields, `Count`
must equal *N* × 781,250 on NTSC (× 937,500 on PAL), to within rounding. The
last line of a state trace gives you `Count` at that frame:

```bash
NOVA64_TRACE_STATE=/tmp/t.trace ./build/nova64 rom.z64 --headless --frames 600
tail -1 /tmp/t.trace     # column 2 is Count in hex; expect ≈ 600 * 781250 = 0x1BF08EB0
```

### What still differs between the engines

Measured on 2026-09-18, headless, `NOVA64_TURBO=1`, current `devbuild`:

- **Super Mario 64**, 1,200 frames: 5 frames differ, all in the CP0 digest only —
  PC, GPRs and FPRs match — and each resolves by the next sample.
- **Majora's Mask**, 3,000 frames from boot: 2,666 frames differ, 2,506 of them
  at the same `Count`. (175 frames were sampled at different `Count` values,
  which makes any difference on them inconclusive.) The first confirmed
  difference is at frame 45. The typical pattern is the recompiler's PC one instruction
  ahead of the interpreter's at the same `Count`, with different GPRs.

It is a real, lasting difference, not a sampling artifact: rerunning Majora's
Mask for 600 frames with `NOVA64_TRACE_RDRAM=1` shows RDRAM itself differing from
frame 47 onward (555 of 600 frames), starting two frames after the first PC/GPR
difference. So the two engines' game memory diverges early in the boot. Which
engine is right is not yet known — the interpreter is the reference by
construction, but nothing has proven it. It has not been tied to a visible bug,
and a 30-minute Majora's Mask playthrough ran on the recompiler, but a
disagreement this early is a good first suspect for any effect that renders
differently between the two CPU cores. Candidates are a one-instruction
difference in where the engines stop relative to a timer exit, or in how a
branch-likely with a skipped delay slot is charged.

If you work on it, start from `NOVA64_TRACE_EVENTS=1` and `NOVA64_TRACE_CP0=1`
on the first differing frame, and remember rule 2 above.

### Unattended game runs

A run reads `settings/` and writes cartridge saves (`Saves/`), save states and the
shader cache in the data folder: `$NOVA64_HOME` if it is set, otherwise the folder
containing the executable for a portable copy (a build from source is one), otherwise
the user data folder. The shell's directory does not matter. **Always point
`NOVA64_HOME` at a scratch folder for a test run** -- without it a run of
`build/nova64` reads and writes your real settings and saves, and a game can change
its save during a test (Majora's Mask deletes an owl save when it loads one). Copy
the save in again before every run so runs stay comparable:

```bash
export NOVA64_HOME=/tmp/run
mkdir -p $NOVA64_HOME/Saves && cd $NOVA64_HOME
cp -R ~/NovaStar64/nova64/build/settings . && ln -s ~/NovaStar64/nova64/build/roms roms
cp "path/to/ZELDA MAJORA'S MASK.fla" Saves/
SDL_AUDIODRIVER=dummy NOVA64_AUTOINPUT_MM=1 NOVA64_SHOT_START=1800 NOVA64_SHOT_EVERY=600 \
NOVA64_SHOT_PATH=/tmp/run/shot ~/NovaStar64/nova64/build/nova64 roms/MM \
    --headless --resolution 480p --frames 2450 --fps 0
```

A copy of the binary run from a scratch folder keeps its data there only if
`portable.txt` or a `settings/` folder is beside it; otherwise it uses your real user
data folder, so set `NOVA64_HOME` regardless. Use absolute paths for output files such
as `NOVA64_SHOT_PATH`: a relative one lands in the data folder.

`NOVA64_AUTOINPUT_SCRIPT` plays a timeline instead of the built-in mashing. Each
line is `from to Buttons [pulse=period,on] [stickX stickY]`, counted in video
fields; buttons are joined with `+` (`A`, `B`, `Z`, `Start`, `L`, `R`, `DUp`,
`CUp`, ...). This one takes Ocarina of Time from boot into Link's house, opens the
pause menu, turns through its pages and closes it again:

```
300 700 Start pulse=100,6      # title screen, then file 1
760 900 A pulse=30,12          # type letters (the cursor starts on A)
920 930 Start                  # cursor to END
960 15099 A pulse=50,10        # confirm the name, then skip dialogue into the house
15100 15108 Start              # open the pause menu
15570 15578 R                  # page right, four times
15650 15658 R
15730 15738 R
15810 15818 R
16000 16008 Start              # close it
```

Running that to field 15000 with `NOVA64_STATE_SAVE_AT=15000:1` leaves a state
you can start later runs from. A state loaded at the very start of a headless run
begins with no framebuffers on the GPU side, and the picture can stay black
(Ocarina of Time's did until its pause menu opened); for anything that depends on
the display, play through with a script instead.

The scripted input is applied where the PIF reads the controller
(`NovaInput::ApplyScriptedInput`). Screenshot and trace frame numbers count
presented frames, and heavy tracing can shift when the game reaches a scene.
Screenshots from the Vulkan backend are boxed to 4:3 (16:9 with a widescreen
setting), so compare them only with screenshots taken since the letterbox was
added.

Game controller code (`NovaInput/GamepadInput.cpp`) can be tested without a
physical controller: SDL's virtual controllers
(`SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0)`)
appear as real ones, with axes and buttons numbered like SDL's controller enums.
Set the trigger axes to -32768 for "released". Virtual controllers can't rumble.
Attach one from a thread inside the running launcher rather than from an
isolated unit test — an isolated test with a single device cannot reproduce
player-slot problems that only appear when several controllers are present.

### Save states and rewind

A snapshot (`N64SYS/MachineState.cpp`) holds everything needed to carry on from
that instant: CPU, FPU and CP0 registers, every hardware block's registers and
private state, the timers, the TLB, RSP memory, PIF RAM, the FlashRAM command
state, the plugins' state and RDRAM. Snapshots are only taken and restored at an
instruction boundary, from a system event (`SysEvent_ServiceStates`): inside a
recompiled block, guest registers live in host registers, so `CRegisters` is not
the whole state there.

**If you add state to a component, add it to that component's
`SerializeState`.** Anything that influences emulated timing counts, even when it
looks like a statistic. The SI DMA jitter is derived from the PIF's
`dma_operations` counter; until that counter was saved, a replay after a rewind
drifted by thousands of CPU cycles within a second. The layout must stay
fixed-size (plain values and arrays), and changing it changes the save-state
file format.

To check a change, trace a run that rewinds, then compare the frames it played
twice:

```bash
NOVA64_TRACE_STATE=/tmp/rewind.trace NOVA64_TRACE_RDRAM=1 \
NOVA64_UI_ROM=roms/game.z64 NOVA64_UI_KEYS=1200:Backspace,1240:Left,1270:Left,1350:Return \
NOVA64_UI_SHOT=/tmp/ui NOVA64_UI_SHOT_AT=1900 ./build/nova64
scripts/compare-trace.py --replay /tmp/rewind.trace
```

The whole check runs unattended: `NOVA64_UI_KEYS` presses launcher keys on the
given launcher frames, and `NOVA64_UI_SHOT_AT` saves a screenshot on each listed
frame and exits after the last.

---

## Making an installer

`./build.sh --package` builds what you give to other people. On macOS it produces
`dist/NovaStar64.app` and `dist/NovaStar64-<version>-macOS.dmg` (a drag-to-Applications
image, with a checksum file) from `nova64/scripts/package-macos.sh`. It builds in its
own `nova64/build-package/` folder, differently from an everyday build in two ways that
matter for a copy that runs on other people's Macs:

- **Not `-march=native`.** A binary tuned for the CPU that built it can use instructions
  an older Mac or PC lacks and crash on the first one. `NOVA64_NATIVE_ARCH=OFF` (which
  the script sets) uses the oldest Apple Silicon (`-mcpu=apple-m1`) or, on x86-64,
  `-march=x86-64-v2`.
- **A deployment target.** Without one the binary requires the macOS version of the SDK
  that built it. The script targets macOS 13.3, the first version whose C++ library has
  floating-point `std::format`, which the code uses; a lower target fails to compile.

The app is Apple Silicon only (the MoltenVK library is thinned to arm64) and is signed
ad hoc, which runs everywhere but makes macOS ask the user to approve it once
(**System Settings → Privacy & Security → Open Anyway**; the text is in the disk
image's `READ ME FIRST.txt`). Signing with a paid Apple Developer ID and notarising
would remove that step: set `CODESIGN_IDENTITY="Developer ID Application: ..."` and the
script signs with the hardened runtime and `nova64/assets/macos/NovaStar64.entitlements`.
The entitlements are not optional: with the hardened runtime and without them the app
cannot load MoltenVK (a different team ID) and the renderer fails to start. Notarising
itself (`xcrun notarytool`) is not scripted.

**Windows.** `nova64/installer/NovaStar64.iss` is an [Inno Setup](https://jrsoftware.org/isinfo.php)
script: a standard setup wizard (licence, install folder, optional desktop shortcut,
launch at the end) that installs for the current user without administrator rights.
The release workflow builds the program with `-DNOVA64_STATIC_RUNTIME=ON` (so users don't
need the Visual C++ Redistributable), wraps it in the installer, and also zips a portable copy. None of
this has been run on Windows yet.

**Releases.** The version is the one line in `VERSION`. To publish:

```bash
# edit VERSION, commit it, then:
git tag v0.1.0 && git push origin v0.1.0
```

`.github/workflows/release.yml` builds both installers and creates a *draft* release
with them attached; nothing is public until you publish the draft on GitHub. The tag
must match `VERSION`. The workflow never runs on ordinary pushes; "Run workflow" on the
Actions tab builds the installers without a release. Pushing a file under
`.github/workflows/` needs a token with the `workflow` scope.

**Where an installed copy keeps its data.** It has no `portable.txt`, so it uses the
user data folder (see [Configuration](README.md#configuration)), not the install folder:
an app in `/Applications` or `Program Files` can't write beside itself, and reinstalling
must not delete saves. The build, the Windows portable zip and any folder that already
holds `settings/`, `roms/` or `saves/` are portable. Don't add a second place data can
live.

## Diagnostic variables

Diagnostics are environment-variable-gated so they cost nothing in normal use.
These are the ones you are most likely to want; `grep -r NOVA64_ nova64/` lists
the rest.

| Variable | Effect |
|---|---|
| `NOVA64_TRACE_STATE=<file>` | Write a per-VI-frame state digest (see above) |
| `NOVA64_TRACE_RDRAM=1` / `_CP0=1` / `_EVENTS=1` | Also hash RDRAM / per-register CP0 / timer and interrupt events |
| `NOVA64_TRACE_FRAMES=<n>` | Stop tracing after `n` frames |
| `NOVA64_TURBO=1` | Uncap the frame rate so headless runs reach late game state quickly |
| `NOVA64_SHOT_FRAME=a,b,c` | Save a screenshot on each listed presented frame |
| `NOVA64_SHOT_EVERY=N` / `NOVA64_SHOT_START=F` | Save one every `N` presented frames, beginning at `F` |
| `NOVA64_SHOT_PATH=<prefix>` | Screenshot filename prefix (`<prefix>_<frame>.png`) |
| `NOVA64_UI_SHOT=<path>` `NOVA64_UI_TAB=settings/controls` | Render one launcher tab to a PNG and exit |
| `NOVA64_UI_ROM=<rom>` | Start a game as soon as the launcher window is up |
| `NOVA64_UI_KEYS=600:Backspace,660:Return` | Press launcher keys on the given launcher frames |
| `NOVA64_UI_SHOT_AT=580,700` | With `NOVA64_UI_SHOT`, save `<path>_<frame>` on each frame and exit after the last (pass a path without an extension; the files are PNG) |
| `NOVA64_AUTOINPUT=1` | Play controller 1 from a script: menu mashing, then wandering |
| `NOVA64_AUTOINPUT_MM=1` (`_MM_BOOT=<reads>`) | Walk through Majora's Mask and open its pause menu, with a longer boot if needed |
| `NOVA64_AUTOINPUT_SCRIPT=<file>` | Play controller 1 from a timeline: one span per line, `from to Buttons [pulse=period,on] [stickX stickY]`, counted in video fields, `#` starts a comment (see [Unattended game runs](#unattended-game-runs)) |
| `NOVA64_STATE_SAVE_AT=<field>[:slot],...` / `NOVA64_STATE_LOAD_AT=` | Save or load a state when the video field count reaches the value, so a headless run can start from late in a game. A run that loads a state continues from the field count the state was saved at |
| `NOVA64_MM_RDP_TRACE=1` (`_FROM=`, `_TO=`) | Log RDP commands, limited to a range of presented frames — tracing a whole run is far too slow |
| `NOVA64_TRI_DUMP=min,max` | Dump every triangle drawn in a range of frames |
| `NOVA64_TEXDBG=1` / `NOVA64_FORENSIC=1` | Log texture decodes / texture-cache hits and misses |
| `NOVA64_NO_MIPMAPS=1` | Disable mipmapping, to isolate texture problems |
| `NOVA64_DUMP_TEXTURES=1` | Write every texture the game uses to `texture_dump/<game name>/` as a Rice-format picture (same as the setting) |
| `NOVA64_UI_OPEN=add` / `edit:<index>` | Open the launcher's Add Game or Edit Game dialog on start, for screenshots |
| `NOVA64_RT=1` | Enable the experimental ray tracer |
| `NOVA64_RT_STATS=1` | Log the ray tracer's scene-update time and GPU time every 120 frames |
| `NOVA64_RT_HISTORY=<n>` | Longest temporal history in frames (default 24; `1` turns accumulation off) |
| `NOVA64_RT_SHADOW_RAYS=<n>` / `NOVA64_RT_AO_RAYS=<n>` | Rays per pixel for a pixel with no history (defaults 2 and 4); a pixel with a long history uses one |
| `NOVA64_RT_DEBUG=1/2/3` | Ray tracer debug views: normals and grid, lighting term only, raster-depth guard |
| `NOVA64_RT_SUN=<x>` / `NOVA64_RT_BLUR=<px>` / `NOVA64_RT_SHADOW=<x>` / `NOVA64_RT_EXPOSURE=<x>` | Shadow softness, denoise radius, shadow strength and exposure |
| `NOVA64_RT_REFL=0` | Turn the ray tracer's reflections off |
| `NOVA64_LOG_LEVEL=<0-6>` | Override the log verbosity |

Gate any new tracing the same way, and remove it once the bug it was written for
is fixed.

---

## Things that have bitten us before

These are real failure modes from this codebase, not hypotheticals.

**Prove code is reachable before you fix it.** Several subsystems here are wired
up but never actually called. `TextureCache::BeginFrame()` — which drains the
queue of evicted GPU textures waiting to be freed — had no caller anywhere, so
every eviction leaked a Vulkan texture until MoltenVK ran out of Metal
residency-set allocations and the driver aborted the process mid-game. The cache
looked correct on paper: its own counters stayed within budget. Before investing
in a fix, confirm the code runs at all — add a counter or a log line and watch it
fire, or `grep` for a call site. Removing a source file from the build and
checking that it still links is a reliable deadness test.

**The same GBI command can mean different things in different microcodes.**
`G_POPMTX` carries `G_MTX_MODELVIEW` (0) in F3D and F3DEX, where it means "pop
one matrix", but a count times 64 in F3DEX2. The shared handler used the F3DEX2
meaning for everything, so in F3D games nothing was ever popped. It went
unnoticed for a long time because Super Mario 64 draws its 3D objects with
`G_MTX_LOAD` and never needs a pop — but its dialog and letter text pushes a
translation per line and pops back, so each line started from the previous
line's accumulated offset and drifted off-screen, and Peach's letter came out
blank while its draw calls all ran. When text or UI goes missing, check the
geometry first: `NOVA64_TRI_DUMP` shows the NDC positions, and anything outside
-1..1 is being placed wrong rather than skipped.

**A framebuffer the game stopped using can still claim its memory.** The RDP
image the game rendered into a region is only a stand-in for that RDRAM while the
game leaves the region alone. Ocarina of Time renders Link's equipment-page
preview into a 64x112 buffer inside the heap; when the menu closes, gameplay
textures are loaded over the same memory. The framebuffer table still listed the
buffer, so a texture load that landed inside its range was bound to the buffer's
canvas instead of being decoded from RDRAM: Link's cap came out black (the
preview's background), and the same range could put pieces of his portrait on
other textures. Each buffer now
keeps a hash of its RDRAM footprint from the last time the RDP drew into it or the
backend copied it back (`FramebufferManager::RecordRdramFingerprint`); a
range-only match whose memory no longer hashes the same is not treated as a
framebuffer. Exact-address matches keep the old behaviour on purpose: Majora's
Mask and Ocarina of Time both read their pause-menu backdrop that way after the
CPU has worked on the memory. To spot this class of bug in a trace, look for
`BindTex0` lines with `fbFound=true fbAlias=false` on textures that should be plain
RDRAM.

**The picture follows the VI, not the last draw.** A frame in which the RDP drew
nothing used to present the buffer the game had drawn into last. Ocarina of
Time's pause menu leaves the RDP idle for seconds while the CPU filters the
frozen frame, and the last task before that had rendered a coverage image into
the old framebuffer's memory, so the screen went white until the menu appeared.
`VulkanRenderer::EndFrame` now presents the display-ready buffer (the VI origin)
even when nothing was drawn.

**Two engines agreeing does not make them right.** The VI-period bug above
existed identically in the interpreter and the recompiler, so the
interpreter-vs-recompiler trace comparison could never see it. When a value has a
real-hardware definition, check it against that absolute number, not just against
the other implementation.

**Don't rely on the working directory.** `settings/`, `roms/`, `saves/`, `Saves/` and
`ShaderCache/` are plain relative paths, so they follow the process's working
directory. That used to be whichever folder the program was started in, and the same
game had different save states and cartridge saves depending on whether it was
opened from a terminal (project root, `nova64/` or `build/`) or from Finder. Since
`main.cpp` now moves to one data folder at startup (`$NOVA64_HOME`, else the
executable's folder for a portable copy, else the user data folder), relative paths are safe -- but a path typed on the command line
is relative to where the user typed it, so resolve it before that move
(`ResolveFromStartDir`), and never add a second place that data can live.

**macOS filesystems are case-insensitive.** `RspTypes.hpp` and `RSPTypes.hpp` are
the same file. A case-insensitive search may report a header as unused when it is
included everywhere under different casing. Check both spellings before deleting
anything. The same applies to `Saves/` and `saves/`, which are one folder on macOS
and two on Linux.

**Never leave a dependency on a moving tag.** Every `FetchContent_Declare` must
pin an exact tag or commit SHA. An unpinned `GIT_TAG master` silently broke the
ARM64 recompiler when AsmJit marked `a64::Assembler` as `final` upstream — the
project built fine until the next reconfigure. If you bump a dependency, do it
deliberately, in its own commit, and say why.

**A build that "succeeds" may not have configured.** If `cmake` is not on `PATH`,
a `cmake .. && ninja` chain short-circuits and Ninja happily relinks stale object
files — including sources you just deleted. Verify the configure step actually
ran.

**A rebuilt macOS binary must be signed, and signing must come before the icon.**
Skip `codesign` and the dynamic libraries fail to load and the process hangs
silently. The build now does both steps, in that order, because `codesign` refuses a
file that already carries a Finder icon ("resource fork, Finder information, or
similar detritus not allowed") and the icon is lost whenever the file is relinked.

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
- Where a behaviour is modelled on another emulator's (GLideN64,
  angrylion-rdp-plus, mupen64plus-rsp-hle), say so in a comment — the existing
  code does, and it is what lets a later reader check the two against each other.
- Gate diagnostic tracing behind an environment variable rather than a permanent
  code path, and remove it once the underlying bug is fixed.

---

## Project64-derived code

Large parts of NovaStar64 — the CPU core, the RSP, and some of `common/` —
originate from [Project64](https://github.com/project64/project64), which is
GPL-2.0 licensed. `NovaGraphics/`, `UI/`, `NovaAudio/`, `NovaInput/` and
`NovaLua/` are original work and should stay that way. The exact share is in the
[README](README.md#project64-and-licensing); regenerate it with
`python3 nova64/scripts/measure-derivation.py` if you change it materially.

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
`NovaAudio/`, `NovaInput/` or `NovaLua/`. Those subsystems are currently clean,
and keeping them that way preserves options. If you want to help remove the
existing dependency, read [docs/REIMPLEMENTATION.md](docs/REIMPLEMENTATION.md)
first — the rules about who may write replacement code are stricter than they
look.

All contributions are accepted under the project's GPL-2.0 license.

---

## Reporting bugs

Open a GitHub issue with:

- The ROM (name and **region**), and whether you used the interpreter or the
  recompiler. If a problem appears with one CPU core and not the other, say so —
  it narrows the search a great deal.
- Steps to reproduce.
- A screenshot or capture for graphical issues.
- A log, if you can: `--log <file>` writes one.
- Your OS, and on macOS whether you're on Apple Silicon or Intel.
