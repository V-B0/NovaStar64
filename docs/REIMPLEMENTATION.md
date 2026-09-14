# Replacing the Project64-derived subsystems

This document scopes what it would take to remove NovaStar64's dependency on
Project64-derived code. It is a roadmap, not a task list — parts of it are
deliberately slow, and one part cannot be done by someone who has read the
existing code.

Nothing here is required to keep developing NovaStar64. GPL-2.0 is a perfectly
good license to ship under, including commercially. This only matters if the
goal is a proprietary, closed-source derivative.

---

## Read this before writing any replacement code

**Rewriting derived code by reading it does not remove the derivation.** A
rewrite based on the original is still a derivative work, no matter how much the
structure, names, or style change. Removing the attribution header is worse than
doing nothing: it doesn't change what the code is, and it turns a compliant
project into an infringing one.

Genuine replacement requires a **clean-room** process:

1. Someone who has read the Project64-derived code writes a specification of
   *what the module does* — interfaces, observable behaviour, hardware
   semantics. It must describe behaviour, not implementation: no code, no
   pseudocode transcribed from the original, no "and then it does X the way the
   original does."
2. Someone who has **never read** the derived code implements from that
   specification plus public hardware documentation.
3. Keep records of who did which role and when.

This is why the work is slow, and why it cannot be done by a single person who
has already read the codebase — including, for the avoidance of doubt, by
prompting an AI assistant to "rewrite" a file it has just been shown.

The person writing the specification may freely consult public hardware
documentation: n64brew.dev, the N64 Programming Manual, the VR4300 manual, and
publicly documented RSP/RDP behaviour. Those are not Project64.

---

## Current scope

Measured by file-header attribution across the tree:

| Area | Files | Lines | Notes |
|---|---:|---:|---|
| `N64SYS/Recomp/` | 35 | 21,883 | Dynamic recompiler: code blocks, sections, register allocation, x64 and ARM64 backends |
| `NovaRSP/Recompiler/` | 18 | 10,511 | RSP recompiler incl. x64 vector codegen |
| `NovaRSP/CPU/` | 18 | 5,984 | RSP interpreter, registers, memory |
| `NovaRSP/HLE/` | 15 | 4,508 | HLE audio lists and microcode dispatch |
| `common/` | 1 | 468 | Executable-memory allocation for the recompiler |
| `N64SYS/MIPSY/` | 4 | 1,921 | Registers, timing, virtual memory |
| `N64SYS/Plugins/` | 4 | 1,882 | Zilmar-spec plugin interfaces |
| `N64SYS/Enhancement/` | 4 | 998 | GameShark-style cheat engine |
| `N64SYS/Savetypes/` | 2 | 888 | EEPROM, FlashRAM |
| `N64SYS/Memoryhandler/` | 1 | 710 | |
| `N64SYS/Interpreter/` | 1 | 503 | MIPS interpreter |
| `NovaRSP/` (misc) | 7 | 726 | Debugger, settings, info |
| **Total** | **110** | **50,960** | |

`NovaGraphics/` (~60K lines), `UI/`, and `NovaAudio/` contain no derived code.
Keep it that way — see [CONTRIBUTING.md](../CONTRIBUTING.md).

---

## Suggested ordering

Ordered by value per unit of risk, not by size.

### 1. `common/` — utilities — **mostly complete**

The easiest tier, and the only one that doesn't strictly need clean-room
handling, because the replacements are standard-library calls rather than
reimplementations of Project64's logic.

Done:

| File | Outcome |
|---|---|
| `StdString.cpp/hpp` | Deleted. One consumer used one printf helper; replaced with `std::format`. |
| `Random.cpp/hpp` | Deleted — entirely unused. |
| `IniFile.cpp/hpp` | Replaced with an independent parser (1,525 → 230 lines). |
| `path.cpp/hpp` | Deleted. Six `CPath` uses replaced with `std::filesystem`. |
| `platform.cpp` | Deleted — POSIX shims for MSVC `_s` functions that nothing called. The macro header was kept and trimmed. |
| `log.cpp` | Reimplemented against the existing (non-derived) `log.hpp` contract. No call-site changes. |

Remaining: **`MemoryManagment.cpp`** (~470 lines, 27 consumers). It wraps
executable-memory allocation for the recompiler — `mmap`/`VirtualAlloc` with W^X
and, on Apple Silicon, `MAP_JIT` handling. It is coupled tightly enough to the
JIT that it belongs with the recompiler tier rather than here.

### 2. `N64SYS/Plugins/` — plugin interfaces (~1,900 lines)

These implement the **Zilmar plugin specification**, which is a published ABI.
That makes it an unusually clean target: the specification *is* the public
document, so a clean-room implementer has everything they need without touching
the existing code.

### 3. `N64SYS/Savetypes/` and `Enhancement/` (~1,900 lines)

Self-contained and well documented publicly. EEPROM/FlashRAM command protocols
are documented on n64brew.dev; the GameShark code format is widely published.
Low coupling to the rest of the system, so mistakes stay local.

### 4. `NovaRSP/CPU/` and `NovaRSP/HLE/` (~10,500 lines)

The RSP is a documented processor: a MIPS-derived scalar core plus a 32-register
vector unit. Instruction semantics — including the awkward parts like `VRCP`,
`VRSQ`, and the accumulator behaviour — are publicly documented, and there are
independent test ROMs that exercise them.

HLE audio is the riskier half: it is not emulating documented hardware but
reimplementing what specific Nintendo audio microcodes do, which is closer to
behavioural reverse engineering. Budget accordingly.

### 5. `N64SYS/Recomp/` and `NovaRSP/Recompiler/` (~32,400 lines)

The largest and hardest tier — nearly two-thirds of the derived code. A dynamic
recompiler with register allocation, block linking, and two backends is a
substantial project in its own right.

Two observations that may change the calculus:

- A correct **interpreter** is far simpler than a recompiler, and the project
  already needs one. Replacing the recompiler with a clean interpreter first
  gets you a working, non-derived system that is slow but correct, and gives the
  replacement recompiler a reference to validate against.
- AsmJit already handles instruction encoding for both backends, so the
  replacement work is the compiler logic, not assembler plumbing.

---

## Verification

Any replacement must be validated against the implementation it replaces, not
just "does the ROM boot."

- Run the interpreter and the recompiler against each other and diff CPU state
  per instruction. A divergence harness is worth building before the rewrite,
  not after.
- Use public N64 test ROMs, which are specifically designed to catch the edge
  cases (rounding, overflow, timing) that casual testing misses.
- Keep a set of known-good frame captures from commercial ROMs and compare.
- Test PAL as well as NTSC.

---

## Honest assessment

The plugin-interface and save-type tiers are realistic: a few thousand lines of
well-documented, loosely coupled code. Combined with the `common/` work already
done, they would take the derived footprint to roughly 46,000 lines.

The RSP is a serious but tractable project against public documentation.

The recompilers are the real cost, and the clean-room constraint is what makes
them expensive — not the code volume, but the requirement that the person
writing them hasn't read what's there. Anyone planning this should be honest
that it is a multi-month effort for an experienced emulator developer, and that
the intermediate states will be slower and buggier than what exists today.

If the goal is simply to ship and sell NovaStar64, none of this is necessary.
