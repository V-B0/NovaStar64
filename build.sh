#!/usr/bin/env bash
# Build NovaStar64 with one command:
#
#   ./build.sh               build it (the program ends up in nova64/build/)
#   ./build.sh --run         build it, then open the launcher
#   ./build.sh --package     build a copy to give to other people (macOS: an app and a .dmg in dist/)
#   ./build.sh --help        all the options
#
# It checks for the tools the build needs, offers to install any that are missing,
# fetches the dependencies, builds, and says where the result is. Run it again after
# changing the source and only what changed is rebuilt.

set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/nova64"
BUILD_TYPE="RelWithDebInfo"
BUILD_DIR="$SRC/build"
DO_RUN=0
DO_PACKAGE=0
DO_CLEAN=0
ASSUME_YES=0

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

  --run        start the launcher when the build finishes
  --package    build a copy to hand to other people instead: on macOS an app and a
               .dmg installer in dist/ (see CONTRIBUTING.md, "Making an installer")
  --clean      delete the previous build first and start from scratch
  --debug      a Debug build (slow to run, easier to debug) instead of the default
  -y, --yes    install missing tools without asking first
  -h, --help   this text
EOF
}

for arg in "$@"; do
    case "$arg" in
        --run)      DO_RUN=1 ;;
        --package)  DO_PACKAGE=1 ;;
        --clean)    DO_CLEAN=1 ;;
        --debug)    BUILD_TYPE="Debug"; BUILD_DIR="$SRC/build-debug" ;;
        -y|--yes)   ASSUME_YES=1 ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------- output helpers
if [ -t 1 ]; then BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'; RESET=$'\033[0m'
else BOLD=""; DIM=""; RED=""; GREEN=""; RESET=""; fi
step() { printf '\n%s==> %s%s\n' "$BOLD" "$*" "$RESET"; }
info() { printf '    %s\n' "$*"; }
fail() { printf '\n%sError:%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }

# Ask a yes/no question (default yes). Without a terminal to ask on, the answer is
# no unless --yes was given, so a script or CI run never hangs waiting.
confirm() {
    [ "$ASSUME_YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local reply
    read -r -p "    $1 [Y/n] " reply
    case "$reply" in ""|y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

OS="$(uname -s)"

# Tools installed by pip or Homebrew often live in folders that aren't on PATH
# (the usual reason a scripted build "succeeds" without configuring anything), so
# look there too.
for dir in "$HOME"/Library/Python/*/bin "$HOME/.local/bin" /opt/homebrew/bin /usr/local/bin; do
    [ -d "$dir" ] && case ":$PATH:" in *":$dir:"*) ;; *) PATH="$PATH:$dir" ;; esac
done
export PATH

version_ge() {  # version_ge 3.27.1 3.20  -> true when the first is at least the second
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n -k3,3n | head -n1)" = "$2" ]
}

# ---------------------------------------------------------------- 1. source
step "Checking the source"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
    fail "can't find nova64/CMakeLists.txt next to this script."
fi
if [ ! -f "$ROOT/softfloat/source/include/softfloat.h" ]; then
    if [ -e "$ROOT/.git" ] && command -v git >/dev/null 2>&1; then
        info "fetching the SoftFloat submodule"
        git -C "$ROOT" submodule update --init --recursive >/dev/null 2>&1 \
            || info "(couldn't; the build will download SoftFloat itself)"
    else
        info "SoftFloat isn't here (this looks like a downloaded archive); the build will download it."
    fi
fi
info "version $(cat "$ROOT/VERSION")"

# ---------------------------------------------------------------- 2. tools
step "Checking the build tools"
MISSING=""

case "$OS" in
Darwin)
    if ! xcode-select -p >/dev/null 2>&1; then
        info "The Xcode command line tools (the C++ compiler) aren't installed."
        if confirm "Open the installer for them now?"; then
            xcode-select --install || true
            fail "finish installing the command line tools in the window that opened, then run ./build.sh again."
        fi
        fail "install them with:  xcode-select --install   and run ./build.sh again."
    fi
    info "compiler: $(clang --version | head -n1)"
    ;;
*)
    if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
        MISSING="a C++ compiler"
    fi
    ;;
esac

CMAKE="$(command -v cmake || true)"
NINJA="$(command -v ninja || true)"

# 3.28 is where FetchContent_Declare gained the EXCLUDE_FROM_ALL option that
# nova64/CMakeLists.txt passes for several dependencies. The build has been run with
# CMake 4.3; nothing older has been tried.
MIN_CMAKE="3.28"
if [ -n "$CMAKE" ]; then
    CMAKE_VER="$("$CMAKE" --version | head -n1 | awk '{print $3}' | sed 's/-.*//')"
    if ! version_ge "$CMAKE_VER" "$MIN_CMAKE"; then
        info "CMake $CMAKE_VER is too old (need $MIN_CMAKE or newer)."
        CMAKE=""
    fi
fi

if [ -z "$CMAKE" ] || [ -z "$NINJA" ]; then
    [ -z "$CMAKE" ] && info "CMake $MIN_CMAKE or newer: not found"
    [ -z "$NINJA" ] && info "Ninja: not found"
    installed=0
    if [ "$OS" = Darwin ] && command -v brew >/dev/null 2>&1; then
        if confirm "Install CMake and Ninja with Homebrew?"; then
            brew install cmake ninja && installed=1
        fi
    elif command -v python3 >/dev/null 2>&1 && python3 -m pip --version >/dev/null 2>&1; then
        if confirm "Install CMake and Ninja with pip (into your user folder, no admin needed)?"; then
            python3 -m pip install --user --upgrade cmake ninja && installed=1
            # pip puts them in a folder that isn't on PATH; find it again.
            for dir in "$HOME"/Library/Python/*/bin "$HOME/.local/bin"; do
                [ -d "$dir" ] && case ":$PATH:" in *":$dir:"*) ;; *) PATH="$PATH:$dir" ;; esac
            done
            export PATH
        fi
    fi
    CMAKE="$(command -v cmake || true)"
    NINJA="$(command -v ninja || true)"
    if [ -z "$CMAKE" ]; then
        case "$OS" in
        Darwin) fail "install CMake and Ninja (for example  python3 -m pip install --user cmake ninja  or  brew install cmake ninja ) and run ./build.sh again." ;;
        *)      fail "install CMake $MIN_CMAKE+, Ninja and a C++20 compiler (Debian/Ubuntu:  sudo apt install build-essential cmake ninja-build git libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libasound2-dev libpulse-dev libgl1-mesa-dev  ) and run ./build.sh again." ;;
        esac
    fi
fi
[ -n "$MISSING" ] && fail "$MISSING is missing; install one and run ./build.sh again."
info "cmake: $("$CMAKE" --version | head -n1)"
if [ -n "$NINJA" ]; then info "ninja: $("$NINJA" --version)"; else info "ninja: not found, using Makefiles instead (slower)"; fi

# ---------------------------------------------------------------- 3. package?
if [ "$DO_PACKAGE" = 1 ]; then
    case "$OS" in
    Darwin)
        step "Building the macOS app and installer"
        export CMAKE NINJA
        exec "$SRC/scripts/package-macos.sh"
        ;;
    *)
        fail "--package builds the macOS installer. The Windows installer is built by the release workflow (.github/workflows/release.yml) or by hand with Inno Setup; see README.md."
        ;;
    esac
fi

# ---------------------------------------------------------------- 4. configure + build
if [ "$DO_CLEAN" = 1 ] && [ -d "$BUILD_DIR" ]; then
    step "Removing the previous build"
    # Only ever delete a folder that is clearly a CMake build folder.
    [ -f "$BUILD_DIR/CMakeCache.txt" ] || fail "$BUILD_DIR doesn't look like a build folder; not deleting it."
    rm -rf "$BUILD_DIR"
fi

step "Configuring (the first time this downloads about a dozen libraries)"
GENERATOR=()
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    if [ -n "$NINJA" ]; then GENERATOR=(-G Ninja); fi
fi
mkdir -p "$BUILD_DIR"
CONFIGURE_LOG="$BUILD_DIR/configure.log"
if ! "$CMAKE" -S "$SRC" -B "$BUILD_DIR" ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >"$CONFIGURE_LOG" 2>&1; then
    tail -n 30 "$CONFIGURE_LOG" >&2
    fail "configuring failed. The full output is in $CONFIGURE_LOG. If it mentions downloading, check your internet connection."
fi
info "done"

step "Building (the first build takes a few minutes; later ones only rebuild what changed)"
BUILD_LOG="$BUILD_DIR/build.log"
START=$(date +%s)
"$CMAKE" --build "$BUILD_DIR" --parallel >"$BUILD_LOG" 2>&1 &
BUILD_PID=$!
if [ -t 1 ]; then
    while kill -0 "$BUILD_PID" 2>/dev/null; do
        progress="$(grep -o '^\[ *[0-9]*\(/[0-9]*\|%\)\]' "$BUILD_LOG" 2>/dev/null | tail -n1 || true)"
        printf '\r    %s%s%s  ' "$DIM" "${progress:-starting}" "$RESET"
        sleep 1
    done
    printf '\r%*s\r' 40 ''
fi
if ! wait "$BUILD_PID"; then
    grep -E "error|Error|FAILED" "$BUILD_LOG" | head -n 20 >&2 || true
    fail "the build failed. The full output is in $BUILD_LOG."
fi
ELAPSED=$(( $(date +%s) - START ))

EXE="$BUILD_DIR/nova64"
[ -f "$EXE" ] || EXE="$BUILD_DIR/$BUILD_TYPE/nova64.exe"
[ -f "$EXE" ] || fail "the build finished but there is no program where I expected it ($BUILD_DIR)."

step "${GREEN}Done${RESET}${BOLD} in $((ELAPSED / 60))m $((ELAPSED % 60))s"
info "the program:  $EXE"
info "start it with ./build.sh --run, or run that file directly."
info "games, saves and settings are kept in $(dirname "$EXE")/ (ROMs are never part of the project)."
info "to give it to other people, run ./build.sh --package"

if [ "$DO_RUN" = 1 ]; then
    step "Starting the launcher"
    exec "$EXE"
fi
