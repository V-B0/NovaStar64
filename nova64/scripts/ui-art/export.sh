#!/bin/bash
# Exports the launcher's pixel art from the .aseprite files to the PNG sheets the program embeds.
# Run it after editing a file in Aseprite. Needs the Aseprite command line (any recent 1.3).
#
#   ASEPRITE=/path/to/aseprite ./export.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
art="$here/../../assets/ui"

find_aseprite() {
    if [ -n "${ASEPRITE:-}" ]; then echo "$ASEPRITE"; return; fi
    for c in \
        "$(command -v aseprite || true)" \
        "$HOME/Library/Application Support/Steam/steamapps/common/Aseprite/Aseprite.app/Contents/MacOS/aseprite" \
        "/Applications/Aseprite.app/Contents/MacOS/aseprite" \
        "$HOME/.local/share/Steam/steamapps/common/Aseprite/aseprite" \
        "/c/Program Files (x86)/Steam/steamapps/common/Aseprite/Aseprite.exe"; do
        if [ -n "$c" ] && [ -x "$c" ]; then echo "$c"; return; fi
    done
    echo "Aseprite not found: set ASEPRITE to its path" >&2
    exit 1
}

aseprite="$(find_aseprite)"
for name in ui_icons ui_font ui_frames; do
    "$aseprite" -b "$art/$name.aseprite" --save-as "$art/$name.png"
    echo "exported $name.png"
done
