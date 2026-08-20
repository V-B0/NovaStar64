# Contributing to NovaStar64

Thanks for your interest in contributing. NovaStar64 is a hobby-scale N64 emulator and
still under heavy development, so please open an issue or discussion before starting on
anything large so we can agree on direction first.

## Getting set up

See [README.md](README.md) for build instructions. In short:

```bash
cd nova64
cmake -B build -G Ninja
cmake --build build
```

On macOS, re-sign the binary after every rebuild:

```bash
codesign --force --deep --sign - nova64/build/nova64
```

## Branches

- `main` is the release branch — stable, playable builds.
- `devbuild` is the active development branch. Target pull requests at `devbuild` unless
  you're specifically asked to target `main`.

## Making changes

- Keep changes focused. Prefer several small, reviewable pull requests over one large one.
- Match the existing code style in the file/module you're editing (C++20, minimal
  comments — comments should explain *why*, not restate what the code does).
- Test your change with a real ROM where possible, both `--interpreter` and
  `--recompiler` if it touches CPU/RSP execution, and note what you tested in the PR
  description.
- If you're touching graphics code, verify with at least one commercial ROM known to
  exercise the path you changed (the RDP/RSP emulation is easy to regress silently).

## Attribution for Project64-derived code

Large parts of NovaStar64 (the CPU core, RSP HLE, and portions of the graphics
pipeline) originate from [Project64](https://github.com/project64/project64), which is
GPL-2.0 licensed. If you're porting or adapting code from Project64 or another GPL-2.0
codebase:

- Say so in the PR description.
- Add a short attribution comment near the code you ported, e.g.:
  ```cpp
  // Ported from Project64's <ClassOrFunction>, adapted to ...
  ```
- File-level headers use:
  ```cpp
  /**
   * @file MyFile.cpp
   * @brief One-line description
   *
   * Derived from Project64 (https://github.com/project64/project64),
   * licensed under the GNU General Public License v2.0 -- see LICENSE.
   */
  ```

All contributions are accepted under the project's GPL-2.0 license.

## Reporting bugs

Open a GitHub issue with:

- The ROM you were running (name/region) and whether you used the interpreter or
  recompiler.
- Steps to reproduce.
- A screenshot or capture if it's a graphical issue.
- Your OS and, on macOS, whether you're on Apple Silicon or Intel.

## Debug/diagnostic code

Several subsystems (graphics, RSP) support env-var-gated diagnostic tracing for
debugging hard-to-reproduce issues (frame dumps, canvas/texture tracing, etc.). If you
add a diagnostic flag, gate it behind an environment variable rather than a permanent
code path, and remove it once the underlying bug is fixed rather than leaving it in
place indefinitely.
