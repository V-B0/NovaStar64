-- Builds the launcher's pixel art as Aseprite files from the text art in assets/ui/.
--
--   aseprite -b --script-param dir=<repo>/nova64/assets/ui --script seed.lua
--
-- It writes, next to the text files:
--   ui_icons.aseprite   16x16 icons in an 8-column grid, one slice per icon (named)
--   ui_font.aseprite    5x7 glyphs in 8x8 cells, 16 columns, cell number = ASCII code - 32
--   ui_frames.aseprite  10x10 frames for nine-slicing (corner 4) and 4x4 dither tiles
-- and a PNG export of each, which is what the program embeds. After this the .aseprite files are
-- the sources: open them in Aseprite to change the art, then run export.sh. Running this script
-- again starts over from the text files and overwrites any edits.

local dir = assert(app.params["dir"], "pass --script-param dir=<assets/ui folder>")

local function read(name)
    local f = assert(io.open(dir .. "/" .. name, "r"))
    local text = f:read("a")
    f:close()
    return text
end

-- Blocks are separated by a blank line; the first line of a block is its name.
local function blocks(text)
    local out = {}
    text = text:gsub("\r", "")
    for block in (text .. "\n\n"):gmatch("(.-)\n\n+") do
        local lines = {}
        for line in (block .. "\n"):gmatch("(.-)\n") do lines[#lines + 1] = line end
        if #lines > 1 then
            out[#out + 1] = { name = table.remove(lines, 1), rows = lines }
        end
    end
    return out
end

-- The launcher's five colours, by the letters the text art uses: K ink, B violet, O orange, L cream, G teal.
local palette = {
    K = { 0x0B, 0x0B, 0x1E }, -- ink
    B = { 0x4B, 0x3B, 0xB5 }, -- violet
    O = { 0xFF, 0x7A, 0x1A }, -- orange
    L = { 0xF6, 0xF0, 0xDC }, -- cream
    G = { 0x3F, 0xC5, 0xB0 }, -- teal
    ["#"] = { 0xFF, 0xFF, 0xFF }, -- font pixels: white, tinted by the program
}
local function rgba(letter)
    local c = palette[letter]
    if not c then return nil end
    return app.pixelColor.rgba(c[1], c[2], c[3], 255)
end

local function newSprite(w, h)
    local spr = Sprite(w, h)
    local pal = Palette(5)
    local i = 0
    for _, letter in ipairs({ "K", "B", "O", "L", "G" }) do
        pal:setColor(i, Color { r = palette[letter][1], g = palette[letter][2], b = palette[letter][3] })
        i = i + 1
    end
    spr:setPalette(pal)
    return spr, Image(w, h)
end

local function finish(spr, img, name)
    spr:newCel(spr.layers[1], 1, img, Point(0, 0))
    spr:saveAs(dir .. "/" .. name .. ".aseprite")
    spr:saveCopyAs(dir .. "/" .. name .. ".png")
    spr:close()
end

-- --- Icons ---
do
    local icons = blocks(read("icons.txt"))
    local columns, cell = 8, 16
    local rows = math.ceil(#icons / columns)
    local spr, img = newSprite(columns * cell, rows * cell)
    local index = {}
    for n, icon in ipairs(icons) do
        local cx, cy = ((n - 1) % columns) * cell, ((n - 1) // columns) * cell
        assert(#icon.rows == 16, icon.name .. " must have 16 rows")
        for y, row in ipairs(icon.rows) do
            assert(#row == 16, icon.name .. " row " .. y .. " must have 16 columns")
            for x = 1, 16 do
                local color = rgba(row:sub(x, x))
                if color then img:drawPixel(cx + x - 1, cy + y - 1, color) end
            end
        end
        local slice = spr:newSlice(Rectangle(cx, cy, cell, cell))
        slice.name = icon.name
        index[#index + 1] = icon.name
    end
    finish(spr, img, "ui_icons")
    local f = assert(io.open(dir .. "/ui_icons.txt", "w"))
    f:write(table.concat(index, "\n"), "\n")
    f:close()
end

-- --- Font ---
do
    local glyphs = blocks(read("font.txt"))
    local columns, cell, count = 16, 8, 96
    local spr, img = newSprite(columns * cell, (count // columns) * cell)
    for _, glyph in ipairs(glyphs) do
        local code = glyph.name:byte(1) - 32
        assert(#glyph.name == 1 and code >= 1 and code < count, "not a printable ASCII glyph: " .. glyph.name)
        local cx, cy = (code % columns) * cell, (code // columns) * cell
        for y, row in ipairs(glyph.rows) do
            for x = 1, 5 do
                if row:sub(x, x) == "#" then img:drawPixel(cx + x - 1, cy + y - 1, rgba("#")) end
            end
        end
        local slice = spr:newSlice(Rectangle(cx, cy, 5, 7))
        slice.name = glyph.name
    end
    finish(spr, img, "ui_font")
end

-- --- Frames and dither tiles ---
do
    local spr, img = newSprite(40, 14)
    -- A frame is a 10x10 outline with its corners cut off in two steps. The inside is left empty:
    -- the program fills it. Nine-slice with a corner of 4 pixels.
    local function frame(ox, oy, letter)
        local c = rgba(letter)
        local function px(x, y) img:drawPixel(ox + x, oy + y, c) end
        for i = 2, 7 do
            px(i, 0); px(i, 9); px(0, i); px(9, i)
        end
        px(1, 1); px(8, 1); px(1, 8); px(8, 8)
    end
    local names = { "B", "O", "L", "G" }
    for i, letter in ipairs(names) do
        frame((i - 1) * 10, 0, letter)
        local slice = spr:newSlice(Rectangle((i - 1) * 10, 0, 10, 10))
        slice.name = "frame_" .. letter
    end
    -- Dither tiles, 4x4, from sparse to dense.
    local tiles = {
        { { 0, 0 } },
        { { 0, 0 }, { 2, 2 } },
        { { 0, 0 }, { 2, 0 }, { 0, 2 }, { 2, 2 } },
        { { 0, 0 }, { 2, 0 }, { 1, 1 }, { 3, 1 }, { 0, 2 }, { 2, 2 }, { 1, 3 }, { 3, 3 } },
    }
    for i, dots in ipairs(tiles) do
        local ox = (i - 1) * 4
        for _, d in ipairs(dots) do img:drawPixel(ox + d[1], 10 + d[2], rgba(i == 4 and "O" or "B")) end
        local slice = spr:newSlice(Rectangle(ox, 10, 4, 4))
        slice.name = "dither_" .. i
    end
    finish(spr, img, "ui_frames")
end

print("ui art written to " .. dir)
