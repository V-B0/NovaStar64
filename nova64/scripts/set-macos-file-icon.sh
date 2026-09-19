#!/bin/sh
# Give a file a custom Finder icon (macOS only).
#
#   scripts/set-macos-file-icon.sh assets/icon/Novastar.png build/nova64
#
# A bare executable has no bundle to hold an icon, so Finder shows the generic
# "exec" one. This attaches the picture to the file itself (as resource-fork and
# Finder-info attributes), which is what "Get Info -> paste an icon" does. The
# attributes are lost whenever the file is replaced, so the build runs this after
# every link; if it is copied elsewhere the copy has the plain icon.
#
# Failing to set the icon never fails the build: it needs a logged-in desktop
# session, which a remote or CI build does not have.

[ "$(uname)" = "Darwin" ] || exit 0
if [ $# -ne 2 ] || [ ! -f "$1" ] || [ ! -f "$2" ]; then
    echo "usage: $0 <icon.png> <file>" >&2
    exit 0
fi

osascript -l JavaScript - "$1" "$2" >/dev/null 2>&1 <<'JXA' || echo "note: could not set the Finder icon on $2" >&2
ObjC.import('AppKit');
function run(argv) {
    const image = $.NSImage.alloc.initWithContentsOfFile(argv[0]);
    if (image.isNil()) throw new Error('cannot read icon');
    if (!$.NSWorkspace.sharedWorkspace.setIconForFileOptions(image, argv[1], 0))
        throw new Error('setIcon failed');
}
JXA
exit 0
