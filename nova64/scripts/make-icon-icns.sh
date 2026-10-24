#!/bin/sh
# Build the macOS app icon (.icns) from the NovaStar PNG.
#
#   scripts/make-icon-icns.sh [source.png] [output.icns]
#
# Defaults to assets/icon/Novastar.png -> assets/icon/Novastar.icns. macOS asks for
# the icon at up to 1024 pixels; the source is far smaller than that, so the larger
# sizes are scaled up and look soft. Replace the PNG with a 1024x1024 picture and
# run this again for a sharp Dock icon. macOS only (sips and iconutil).

set -eu
here="$(cd "$(dirname "$0")/.." && pwd)"
src="${1:-$here/assets/icon/Novastar.png}"
dst="${2:-$here/assets/icon/Novastar.icns}"

[ "$(uname)" = "Darwin" ] || { echo "make-icon-icns.sh needs macOS (sips, iconutil)" >&2; exit 1; }
[ -f "$src" ] || { echo "no such file: $src" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
set_dir="$work/Novastar.iconset"
mkdir "$set_dir"

# name:pixels
for entry in icon_16x16:16 icon_16x16@2x:32 icon_32x32:32 icon_32x32@2x:64 \
             icon_128x128:128 icon_128x128@2x:256 icon_256x256:256 icon_256x256@2x:512 \
             icon_512x512:512 icon_512x512@2x:1024; do
    name="${entry%%:*}"; px="${entry##*:}"
    sips -z "$px" "$px" "$src" --out "$set_dir/$name.png" >/dev/null
done
iconutil -c icns "$set_dir" -o "$dst"
echo "wrote $dst"
