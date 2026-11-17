# Release plan: 0.1.0

What stands between the current tree and a first public release, in the order it should be
done. `[x]` is finished, `[ ]` is open. Each step is small enough to be one commit and says how
it is verified.

How changes are checked in this plan:

- **Formatting and comment-only changes** are proven harmless by compiling every source file to
  assembly before and after and requiring identical output (apart from `__LINE__` constants).
- **Code changes** are checked with the RSP differential selftest (`nova64 --rsp-selftest`) and
  by comparing frames of Super Mario 64, Ocarina of Time and Majora's Mask against the previous
  build (`NOVA64_SHOT_*`, see CONTRIBUTING.md).
- **Anything that touches only one platform** (Windows, Linux) waits for CI, because only macOS
  has been built and run so far.

---

## 1. Code hygiene

Formatting and structure

- [x] One style for the whole tree: `.clang-format`, `.editorconfig`, every C++ file formatted
- [x] `.git-blame-ignore-revs` so the formatting commit does not hide authorship in `git blame`
- [x] Decorative banner comments (`// =====`) collapsed to one-line section headings
- [x] Comment review of every file: comments that restate code, stale status notes and
      commented-out code removed; hardware notes and every attribution kept; every file has a header.
      Checked three ways: identical compiled output on AArch64 and x86-64, identical code tokens in
      all 351 files, and no credit to another project lost (see the commit messages)
- [ ] Header and source file names agree in case (`MIPSYinterfacehandler.hpp`, `file.hpp`,
      `Timestamp.hpp` do not match their `.cpp`); rename with `git mv` and fix includes
- [ ] Unused includes removed and forward declarations used in headers (shorter rebuilds)
- [ ] CMake: formatting check target (`clang-format --dry-run -Werror`) and the same check in CI

Dead code and leftovers

- [ ] Diagnostic switches left from past bisecting removed (`FORCE_CALLBACK_*` in the x64
      recompiler, other `#if 0` and `#ifdef` debug blocks)
- [x] Dead code deleted in two rounds, about 25,000 lines: each confirmed with the linker map on both CPUs,
      a clean build with no warnings, the RSP selftest, unchanged frames from the three test games and
      unchanged saved states at a fixed field
- [ ] What is left of the dead code, listed in [CLEANUP_FINDINGS.md](CLEANUP_FINDINGS.md): the trace system,
      parts of the GL shader helper, the recompiler's section graph
- [ ] The suspected bugs in CLEANUP_FINDINGS.md triaged: reproduce, fix or close each one, with a test
      where a test is possible
- [ ] About 100 raw `printf`/`std::cout`/`fprintf(stderr)` calls routed through the logger
      (keep the command-line output of `main.cpp` and the selftest)
- [ ] The dozens of `NOVA64_*` environment variables audited: keep the documented ones, drop the
      one-off traces, document the rest in CONTRIBUTING.md
- [ ] Remaining `TODO`/`HACK` markers (6) resolved or written up as issues
- [ ] Magic numbers in the hottest and least clear places given names

Correctness and robustness

- [ ] `-ffast-math` in `Release` makes Clang reject the recompilers' `#pragma STDC FENV_ACCESS ON`,
      so a plain `-DCMAKE_BUILD_TYPE=Release` build fails; `/fp:fast` is on for MSVC. Decide the
      floating-point flags for emulation code and make every build type compile
- [ ] Compiler warnings kept at zero: `-Wall -Wextra -Wshadow` and `-Werror` in CI on all three
      platforms (currently zero on macOS ARM64 and x86-64)
- [ ] AddressSanitizer and UndefinedBehaviorSanitizer runs of the selftest and of the three
      games; fix what they find
- [ ] `clang-tidy` with a small, agreed set of checks (`bugprone-*`, `performance-*`, selected
      `modernize-*`); fix real findings, do not mass-rewrite
- [ ] Oversized files split along existing seams: `VKBackend.cpp` (3.7k lines),
      `InterpreterOps.cpp` (3.5k), `x64RecompilerOps.cpp` (3.3k), `ShaderGenerator.cpp` (2.6k),
      `RecompInterpCallbacks.inc` (2.5k), `CodeSection.cpp` (2.4k), `RDPProcessor_Batch.cpp` (2.1k)
- [ ] Naming conventions written down in CONTRIBUTING.md (the Project64-derived code keeps its
      own style; new code follows the stated one)

## 2. Tests and continuous integration

- [x] RSP differential selftest (random programs, interpreter versus recompiler)
- [x] CTest wired up: `ctest` runs the RSP selftest and the Simulator workbench unit tests (`tests/`)
- [ ] A few unit tests where they are cheap: byte-swapping and ROM format detection, CRC, save
      type detection, TLB translation, PIF/CIC handling, settings parsing
- [ ] Golden-frame check for the three games (local only: it needs ROMs, which are not in the
      repository), scripted and documented
- [ ] CI jobs for macOS (ARM64 and Intel), Windows, Linux; Debug and RelWithDebInfo; ccache
- [ ] First CI run read through and every Windows/Linux failure fixed
- [ ] Nightly build uploading artifacts
- [ ] Badges (build status, licence) in the README

## 3. Repository and documentation

- [ ] README rewritten to be short: what it is, status table, download, build, screenshots
- [ ] `docs/ARCHITECTURE.md`: modules (N64SYS, NovaRSP, NovaGraphics, UI), data flow of a frame,
      where Project64-derived code lives
- [ ] `CHANGELOG.md` (Keep a Changelog), starting with 0.1.0
- [ ] `SECURITY.md` (how to report a vulnerability) and `CODE_OF_CONDUCT.md`
- [ ] Issue templates (bug report with log and system info, game compatibility report) and a
      pull-request template
- [ ] `THIRD_PARTY_NOTICES.md`: SDL2, Dear ImGui, asmjit, Lua, nlohmann/json, stb, volk, glslang,
      rcheevos, MoltenVK, Berkeley SoftFloat, plus the Project64 and Mupen64Plus-derived
      code; the same list reachable from an About dialog in the launcher
- [ ] `docs/COMPATIBILITY.md` generated from the game database
- [ ] `docs/` links checked, every claim in the README checked against the code
- [ ] GitHub repository settings: description, topics, social preview image, branch protection,
      Discussions
- [ ] Decide whether the 10 MB MoltenVK library stays in the repository or is fetched

## 4. Launcher and in-game UI

- [x] A UI kit (`UI/UiKit.*`) holding the palette, fonts, icons and widgets, so pages only say what they show
- [x] A modern console-style look in the warm orange of the app icon: opaque cards on a neutral ramp with one hairline
      and one lightness step per layer, one accent colour, two corner radii, a 4-point spacing scale, Inter for text and Phosphor for icons (both
      compiled in, licences kept), set at the display's pixel density so it is sharp on a Retina or scaled display
- [x] Research behind the look (emulator front-ends, design-system settings rows, spacing scales, contrast, ImGui
      navigation): text and captions measured against WCAG 2.2 (muted text 7:1, dim text 4.5:1 on every surface)
- [x] Icons for every page and action, as a font, so they stay sharp at any size
- [x] Home with a large card for the game last played (box art, publisher, year, description, play and
      favourite, and a screenshot of the last session), a Recently Played row, and a hint bar
- [x] Library, Recent and Favorites pages with search; a game remembers when it was last played and how often,
      and can be made a favourite
- [x] Window controls drawn as proper minimise/maximise/close glyphs instead of `_ [] x`
- [x] Sidebar with an icon per page, a clear selected state and the version at the bottom
- [x] Button hierarchy: one primary action per screen, quiet secondary buttons, a distinct
      destructive style, with hover, pressed and disabled states
- [x] Settings as rows: title and help on the left, control on the right, grouped in cards, a Reset button on a
      row that differs from its default, an Experimental badge, Restore defaults per page, one table of options
      (`UI/GameSetup.*`) behind both the Settings page and the Simulator
- [x] Simulator as a workbench (Packet Tracer style): drag a console, TV, controllers, Rumble Paks and the cartridge
      onto a canvas and cable them; link lights; a check that says in words what is missing; Realtime and Simulation
      modes (the power-on sequence, event by event, with an envelope travelling the cables); Play is blocked while the
      wiring has an error. Each game has its own bench (`settings/games/<rom>.lab`) and its own setup
      (`settings/games/<rom>.ini`): speed, CPU core, RSP engine, rewind, driver, resolution, widescreen, textures,
      accuracy, the save chip and the Rumble Pak per player, applied when the game starts
- [ ] The workbench offers 4 MB or 8 MB RAM, a TV standard and a Controller Pak only if the core gains them (it has
      8 MB, takes the region from the ROM, and has no Controller Pak)
- [ ] Undo and redo, copy and paste, and a minimap on the workbench; keyboard-only wiring (it is mouse-driven now)
- [ ] Library: sort order, play time, and cover pictures fetched for the user (the code makes no network
      connections, so this needs a decision)
- [ ] Empty and error states with an actionable message: no games, Vulkan or MoltenVK missing,
      shader compile failure, unreadable ROM
- [x] Keyboard and controller navigation reaches every widget (custom buttons use `ImGuiButtonFlags_EnableNav`
      and cards are `NavFlattened`), with a visible 2 px focus ring
- [ ] Controller navigation checked on a real gamepad: sliders with the D-pad, B to go back, bumpers to change
      Settings tabs, glyphs that follow the device in use
- [ ] Notifications for saves, rewinds and errors
- [x] Pause menu and rewind strip restyled; save-state slots with thumbnails
- [ ] Check the interface on a Retina display and at Windows scale settings (so far it has only been
      rendered at 1x); add an interface-scale setting
- [ ] A high-contrast option
- [x] Interface size: 100%, 125%, 150%, 200%, applied at once; text, icons and spacing scale together because the
      whole interface is laid out in its own units. Only sizes whose window fits the screen are offered
- [x] A Rumble Pak shakes the window while a game rumbles, with a strength setting (and a per-game one); the game
      picture shakes instead when the window is maximised or fullscreen
- [x] Launcher code review (four reviewers, then fixes): the window never opens larger than the screen, the pointer is
      in the interface's units on every frame (also while a controller is moving the focus), the framebuffer scale is
      set rather than multiplied while minimised, the shake puts the window back before it is maximised or minimised
      and falls back to shaking the picture where a window cannot be moved, removed games stay removed, a game with the
      same file name as another no longer replaces its copy in `roms/`, pictures are size-checked and loaded a few per
      frame, and stat calls are cached
- [ ] Text is sharpest when one interface unit is a whole number of pixels (100% and 200%, and 150% on a Retina
      display); at 125% and at 150% on a 1x display it is slightly soft. A fix means laying the interface out in
      pixels instead of units
- [ ] First-run welcome: pick a game folder, test the controller
- [x] About page: version, licence, the keys used while playing, what it is made with
- [ ] A full list of third-party licences reachable from the About page
- [ ] Before/after screenshots of every page for the README and the video

## 5. Release engineering

- [ ] Versioning policy (SemVer), `VERSION` as the single source, tag-driven release workflow
- [ ] macOS: Developer ID signing, notarisation and stapling (needs an Apple Developer account);
      decide between a universal binary and separate ARM64 and Intel downloads
- [ ] macOS disk image artwork (background, icon placement)
- [x] Windows: built and run on a real machine, installer and portable zip tried; the README has the SmartScreen note
- [ ] Windows code signing (so SmartScreen stops warning)
- [ ] Linux: AppImage (or Flatpak), Vulkan loader and SDL requirements documented
- [x] Checksums published with every file (a `.sha256` for the dmg, the setup wizard and the portable zip)
- [ ] Release notes generated from the changelog
- [ ] Crash handling: a log and a "copy diagnostics" button that does not include personal paths
- [ ] Privacy statement in the README: no telemetry and no network connections (the code has no
      sockets or HTTP at all; the achievements tab is a placeholder). Re-check before each release
- [x] Fresh-home first run of the packaged macOS app (dmg verified, signature and minimum OS checked, game added,
      state saved, data kept outside the app, library there on the next start)
- [ ] Pre-release checklist for every release: clean checkout builds, upgrade keeps saves, uninstall keeps saves
- [ ] Dry run on a throwaway tag and a draft release before the real one

## 6. Video footage

- [x] Game audio can be recorded (`NOVA64_AUDIO_DUMP=<file.wav>`)
- [x] A generator for input that looks played by a person (smoothed stick, reaction lag, uneven taps)
- [ ] Capture setup: fixed window size and scale, 60 fps, a tidy data folder name so no
      personal or temporary path shows on screen
- [ ] Input that looks played by a person: smoothed stick curves, reaction delays, small
      corrections, pauses; the same seed gives the same take, several takes per scene
- [ ] Launcher footage with a natural cursor: eased paths, slight overshoot, click feedback
- [ ] Shot list: first launch, library, adding a game, settings, three games in play, pause menu
      and rewind, resolution and filtering options, high-level versus low-level graphics,
      the RSP recompiler (selftest in the terminal, speed-up), the developer tools (memory
      viewer, scripts, simulator), a one-command build, the download page
- [ ] Rough cut, then a polished edit with titles and callouts
- [ ] Decide on music and any voice-over (licences for music; a voice or text-only captions)

---

## Decisions that are yours

1. **Game footage.** Videos of commercial games are normal for emulator showcases but carry a
   takedown risk on some platforms. Your call whether to show all three games or keep to short
   clips.
2. **App icon.** The current icon stays unless you want it redrawn.
3. **macOS signing.** Needs your Apple Developer account; nothing can be notarised without it.
4. **Repository visibility and the release date** (public repository, tag `v0.1.0`).
