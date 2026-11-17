# Launcher fonts

| File | What | Licence |
|---|---|---|
| `Inter-Regular.ttf`, `Inter-SemiBold.ttf`, `Inter-Bold.ttf` | [Inter](https://rsms.me/inter/) 4.1 by The Inter Project Authors, cut down to Latin text | SIL Open Font License 1.1 (`LICENSE-Inter-OFL.txt`) |
| `Phosphor.ttf`, `Phosphor-Fill.ttf` | [Phosphor Icons](https://phosphoricons.com/) 2.1.1, regular and fill styles, cut down to the icons the launcher uses | MIT (`LICENSE-Phosphor-MIT.txt`) |
| `phosphor-codepoints.txt` | Icon name and code point for each icon kept | |

They are compiled into the program (see `nova64_embed_file` in `CMakeLists.txt`). `scripts/ui-art/subset-fonts.py`
rebuilds them from the full fonts, and explains how to add an icon.
