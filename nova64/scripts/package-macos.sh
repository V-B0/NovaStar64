#!/usr/bin/env bash
# Build NovaStar64.app and a drag-to-Applications .dmg installer (macOS, Apple Silicon).
#
#   ./build.sh --package          the usual way (checks the tools first)
#   nova64/scripts/package-macos.sh   the same, if cmake and ninja are already on PATH
#
# Output, in dist/:
#   NovaStar64.app
#   NovaStar64-<version>-macOS.dmg      the installer
#   NovaStar64-<version>-macOS.dmg.sha256
#
# The build goes in nova64/build-package/, separate from the everyday nova64/build/,
# with two differences from it that matter for a copy other people will run:
#   * not tuned for this Mac's CPU (an M3 build can use instructions an M1 lacks), and
#   * built to run on macOS 13.3 or later, not just the version of the SDK used here.
#
# Environment:
#   CODESIGN_IDENTITY   a "Developer ID Application: ..." identity to sign with and
#                       enable the hardened runtime; the default is an ad-hoc signature,
#                       which runs everywhere but needs the one-time "Open Anyway" step.
#   MACOSX_DEPLOYMENT_TARGET   oldest macOS to build for (default 13.3, the first with floating-point std::format)
#
# Notarising (so the app opens with no warning at all) needs a paid Apple Developer
# account and is not done here.

set -eu

SRC="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(dirname "$SRC")"
BUILD="$SRC/build-package"
DIST="$ROOT/dist"
VERSION="$(cat "$ROOT/VERSION")"
APP_NAME="NovaStar64"
APP="$DIST/$APP_NAME.app"
DMG="$DIST/$APP_NAME-$VERSION-macOS.dmg"
TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.3}"
IDENTITY="${CODESIGN_IDENTITY:--}"
BUNDLE_ID="io.github.v-b0.novastar64"

[ "$(uname)" = "Darwin" ] || { echo "package-macos.sh runs on macOS" >&2; exit 1; }
[ "$(uname -m)" = "arm64" ] || { echo "package-macos.sh builds for Apple Silicon and needs an Apple Silicon Mac" >&2; exit 1; }

CMAKE="${CMAKE:-$(command -v cmake || true)}"
NINJA="${NINJA:-$(command -v ninja || true)}"
[ -n "$CMAKE" ] || { echo "cmake not found; run ./build.sh --package instead, which finds or installs it" >&2; exit 1; }

say() { printf '\n==> %s\n' "$*"; }

# ---------------------------------------------------------------- 1. build
say "Building for distribution (macOS $TARGET or later, generic Apple Silicon)"
GEN=()
if [ ! -f "$BUILD/CMakeCache.txt" ] && [ -n "$NINJA" ]; then GEN=(-G Ninja); fi
"$CMAKE" -S "$SRC" -B "$BUILD" ${GEN[@]+"${GEN[@]}"} \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DNOVA64_NATIVE_ARCH=OFF \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" >"$BUILD.configure.log" 2>&1 \
    || { tail -n 30 "$BUILD.configure.log" >&2; echo "configure failed; see $BUILD.configure.log" >&2; exit 1; }
"$CMAKE" --build "$BUILD" --parallel >"$BUILD.build.log" 2>&1 \
    || { grep -E "error|FAILED" "$BUILD.build.log" | head -n 20 >&2; echo "build failed; see $BUILD.build.log" >&2; exit 1; }
EXE="$BUILD/nova64"
[ -f "$EXE" ] || { echo "no executable at $EXE" >&2; exit 1; }

# ---------------------------------------------------------------- 2. the app bundle
say "Assembling $APP_NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/Licenses"

# cp -X: leave out the Finder-icon attributes the build put on the executable;
# codesign refuses a file that has them.
cp -X "$EXE" "$APP/Contents/MacOS/$APP_NAME"

# MoltenVK ships as a universal library; the app is arm64 only, so keep half of it.
DYLIB_SRC="$SRC/thirdparty/MoltenVK/libMoltenVK.dylib"
if lipo -archs "$DYLIB_SRC" | grep -q ' '; then
    lipo "$DYLIB_SRC" -thin arm64 -output "$APP/Contents/Frameworks/libMoltenVK.dylib"
else
    cp -X "$DYLIB_SRC" "$APP/Contents/Frameworks/libMoltenVK.dylib"
fi

cp -X "$SRC/assets/icon/Novastar.icns" "$APP/Contents/Resources/$APP_NAME.icns"
cp -X "$ROOT/LICENSE" "$APP/Contents/Resources/Licenses/NovaStar64-LICENSE.txt"
cp -X "$SRC/thirdparty/MoltenVK/LICENSE" "$APP/Contents/Resources/Licenses/MoltenVK-LICENSE.txt"
cp -X "$ROOT/README.md" "$APP/Contents/Resources/Licenses/README.md"
cp -X "$ROOT/THIRD_PARTY_NOTICES.md" "$APP/Contents/Resources/Licenses/THIRD_PARTY_NOTICES.md"
cp -X "$SRC/assets/fonts/LICENSE-Inter-OFL.txt" "$APP/Contents/Resources/Licenses/Inter-OFL.txt"
cp -X "$SRC/assets/fonts/LICENSE-Phosphor-MIT.txt" "$APP/Contents/Resources/Licenses/Phosphor-MIT.txt"

cat >"$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>                 <string>$APP_NAME</string>
    <key>CFBundleDisplayName</key>          <string>$APP_NAME</string>
    <key>CFBundleIdentifier</key>           <string>$BUNDLE_ID</string>
    <key>CFBundleExecutable</key>           <string>$APP_NAME</string>
    <key>CFBundleIconFile</key>             <string>$APP_NAME</string>
    <key>CFBundlePackageType</key>          <string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleShortVersionString</key>   <string>$VERSION</string>
    <key>CFBundleVersion</key>              <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>       <string>$TARGET</string>
    <key>LSApplicationCategoryType</key>    <string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key>      <true/>
    <key>NSHumanReadableCopyright</key>     <string>Copyright © Varga Bálint. GPL-2.0.</string>
</dict>
</plist>
PLIST
plutil -lint "$APP/Contents/Info.plist" >/dev/null

# ---------------------------------------------------------------- 3. sign
say "Signing ($([ "$IDENTITY" = "-" ] && echo "ad hoc" || echo "$IDENTITY"))"
if [ "$IDENTITY" = "-" ]; then
    codesign --force --sign - "$APP/Contents/Frameworks/libMoltenVK.dylib"
    codesign --force --sign - "$APP"
else
    codesign --force --timestamp --options runtime --sign "$IDENTITY" "$APP/Contents/Frameworks/libMoltenVK.dylib"
    codesign --force --timestamp --options runtime \
        --entitlements "$SRC/assets/macos/NovaStar64.entitlements" --sign "$IDENTITY" "$APP"
fi
codesign --verify --strict --verbose=1 "$APP"

# ---------------------------------------------------------------- 4. smoke test
say "Checking the app starts"
# A throwaway HOME, so the check can't touch anybody's real data folder.
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
HOME="$SCRATCH" "$APP/Contents/MacOS/$APP_NAME" --help >/dev/null
echo "    runs"

# ---------------------------------------------------------------- 5. the .dmg
say "Making the installer image"
STAGE="$SCRATCH/dmg"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cat >"$STAGE/READ ME FIRST.txt" <<EOF
NovaStar64 $VERSION

INSTALL
  Drag NovaStar64 onto the Applications folder in this window.

THE FIRST TIME YOU OPEN IT
  macOS will say it can't check NovaStar64 for malicious software, because the app
  isn't signed with a paid Apple developer certificate. It is safe to allow:

    1. Try to open NovaStar64, then close the message.
    2. Open System Settings > Privacy & Security, scroll down to Security, and click
       "Open Anyway" next to NovaStar64. Confirm with your password.

  (On macOS 14 or earlier you can instead right-click the app and choose Open.)
  You only have to do this once. From Terminal, the same thing is:

    xattr -dr com.apple.quarantine /Applications/NovaStar64.app

YOUR GAMES
  NovaStar64 comes with no games. In the launcher, click Add Game and pick your own
  ROM files (.z64, .n64 or .v64).

  Settings, games, saves and texture packs are kept in
    ~/Library/Application Support/NovaStar64
  (Settings > Emulation > Open data folder shows it). Deleting the app does not
  delete them.

SOURCE CODE AND LICENCES
  NovaStar64 is free software under the GNU GPL, version 2.
  https://github.com/V-B0/NovaStar64
  The licences of the app and of MoltenVK are inside the app, in
  Contents/Resources/Licenses.
EOF
mkdir -p "$DIST"
rm -f "$DMG"
hdiutil create -quiet -volname "$APP_NAME $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
(cd "$DIST" && shasum -a 256 "$(basename "$DMG")" >"$(basename "$DMG").sha256")

say "Done"
echo "    app:        $APP"
echo "    installer:  $DMG ($(du -h "$DMG" | cut -f1))"
echo "    checksum:   $(cut -d' ' -f1 "$DMG.sha256")"
