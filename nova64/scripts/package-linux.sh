#!/usr/bin/env bash
# Build NovaStar64 as an AppImage: one file that runs on most Linux systems, with nothing to install.
# (Linux, x86-64 or ARM64.)
#
#   nova64/scripts/package-linux.sh
#
# Output, in dist/:
#   NovaStar64-<version>-Linux-<arch>.AppImage
#   NovaStar64-<version>-Linux-<arch>.AppImage.sha256
#
# The build goes in nova64/build-package-linux/, separate from the everyday nova64/build/, and is not tuned
# for this machine's CPU, so the file runs on other computers. The AppImage needs at least the C library of the
# machine that built it; the release workflow builds on Ubuntu 24.04 because the code needs GCC 13 (std::format).
#
# The AppImage does not contain the graphics driver, the Vulkan loader or the system's audio libraries; those
# are the computer's own. A copy in an AppImage keeps its settings, games and saves in
# $XDG_DATA_HOME/NovaStar64 (~/.local/share/NovaStar64), never inside the file.
#
# Needs: a C++20 compiler, CMake, Ninja or make, git, the development packages listed in README.md,
# and curl. appimagetool is downloaded once, from a fixed release, and checked against the checksum below.
#
# Environment:
#   CMAKE, NINJA    the tools to use if they aren't on PATH

set -eu

SRC="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(dirname "$SRC")"
BUILD="$SRC/build-package-linux"
DIST="$ROOT/dist"
VERSION="$(head -n1 "$ROOT/VERSION" | tr -d '[:space:]')"
ARCH="$(uname -m)"
OUT="$DIST/NovaStar64-$VERSION-Linux-$ARCH.AppImage"
APPDIR="$BUILD/AppDir"

# appimagetool 1.9.1, https://github.com/AppImage/appimagetool/releases/tag/1.9.1
TOOL_VERSION="1.9.1"
case "$ARCH" in
x86_64)  TOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0" ;;
aarch64) TOOL_SHA256="f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158" ;;
*) echo "package-linux.sh builds for x86-64 and ARM64; this machine is $ARCH" >&2; exit 1 ;;
esac

[ "$(uname)" = "Linux" ] || { echo "package-linux.sh runs on Linux" >&2; exit 1; }

CMAKE="${CMAKE:-$(command -v cmake || true)}"
NINJA="${NINJA:-$(command -v ninja || true)}"
[ -n "$CMAKE" ] || { echo "cmake not found; install it (sudo apt install cmake ninja-build)" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "curl not found; install it" >&2; exit 1; }

say() { printf '\n==> %s\n' "$*"; }

# ---------------------------------------------------------------- 1. build
say "Building for distribution (generic $ARCH)"
GEN=()
if [ ! -f "$BUILD/CMakeCache.txt" ] && [ -n "$NINJA" ]; then GEN=(-G Ninja); fi
"$CMAKE" -S "$SRC" -B "$BUILD" ${GEN[@]+"${GEN[@]}"} \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DNOVA64_NATIVE_ARCH=OFF >"$BUILD.configure.log" 2>&1 \
    || { tail -n 30 "$BUILD.configure.log" >&2; echo "configure failed; see $BUILD.configure.log" >&2; exit 1; }
"$CMAKE" --build "$BUILD" --parallel >"$BUILD.build.log" 2>&1 \
    || { grep -E "error|FAILED" "$BUILD.build.log" | head -n 20 >&2; echo "build failed; see $BUILD.build.log" >&2; exit 1; }
[ -x "$BUILD/nova64" ] || { echo "the build made no nova64 program" >&2; exit 1; }

# ---------------------------------------------------------------- 2. the folder the AppImage is made from
say "Laying out the AppImage"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/licenses/novastar64"
# No portable.txt beside the program: that is what makes it keep its data in the user's own folder.
install -m 755 "$BUILD/nova64" "$APPDIR/usr/bin/nova64"
install -m 644 "$SRC/installer/linux/novastar64.desktop" "$APPDIR/usr/share/applications/novastar64.desktop"
install -m 644 "$SRC/installer/linux/novastar64.desktop" "$APPDIR/novastar64.desktop"
install -m 644 "$SRC/assets/icon/Novastar-256.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/novastar64.png"
install -m 644 "$SRC/assets/icon/Novastar-256.png" "$APPDIR/novastar64.png"
install -m 644 "$ROOT/LICENSE" "$APPDIR/usr/share/licenses/novastar64/LICENSE"
install -m 644 "$ROOT/THIRD_PARTY_NOTICES.md" "$APPDIR/usr/share/licenses/novastar64/THIRD_PARTY_NOTICES.md"
install -m 644 "$SRC/assets/fonts/LICENSE-Inter-OFL.txt" "$APPDIR/usr/share/licenses/novastar64/Inter-OFL.txt"
install -m 644 "$SRC/assets/fonts/LICENSE-Phosphor-MIT.txt" "$APPDIR/usr/share/licenses/novastar64/Phosphor-MIT.txt"
cat >"$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/nova64" "$@"
APPRUN
chmod 755 "$APPDIR/AppRun"
ln -sf novastar64.png "$APPDIR/.DirIcon"

# ---------------------------------------------------------------- 3. appimagetool
TOOL="$BUILD/appimagetool-$TOOL_VERSION-$ARCH.AppImage"
if [ ! -x "$TOOL" ]; then
    say "Fetching appimagetool $TOOL_VERSION"
    curl -fsSL -o "$TOOL.part" \
        "https://github.com/AppImage/appimagetool/releases/download/$TOOL_VERSION/appimagetool-$ARCH.AppImage"
    if command -v sha256sum >/dev/null 2>&1; then got="$(sha256sum "$TOOL.part" | cut -d' ' -f1)"
    else got="$(shasum -a 256 "$TOOL.part" | cut -d' ' -f1)"; fi
    if [ "$got" != "$TOOL_SHA256" ]; then
        rm -f "$TOOL.part"
        echo "appimagetool did not match its checksum (got $got); not using it" >&2
        exit 1
    fi
    chmod 755 "$TOOL.part"
    mv "$TOOL.part" "$TOOL"
fi

# ---------------------------------------------------------------- 4. the AppImage
say "Making the AppImage"
mkdir -p "$DIST"
rm -f "$OUT"
# --appimage-extract-and-run: no FUSE needed, so this also works in a container or a CI runner.
ARCH="$ARCH" "$TOOL" --appimage-extract-and-run --no-appstream "$APPDIR" "$OUT" >"$BUILD.appimage.log" 2>&1 \
    || { tail -n 30 "$BUILD.appimage.log" >&2; echo "appimagetool failed; see $BUILD.appimage.log" >&2; exit 1; }
chmod 755 "$OUT"
( cd "$DIST" && if command -v sha256sum >/dev/null 2>&1; then sha256sum "$(basename "$OUT")"; \
    else shasum -a 256 "$(basename "$OUT")"; fi >"$(basename "$OUT").sha256" )

say "Done"
echo "    AppImage:  $OUT ($(du -h "$OUT" | cut -f1))"
echo "    checksum:  $(cut -d' ' -f1 "$OUT.sha256")"
