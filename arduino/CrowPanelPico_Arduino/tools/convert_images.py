"""
Convert the shared ../../../buttons.png sprite sheet into a single C header
of RGB565 pixel arrays for the Arduino port.

buttons.png (repository root) is a 320x480 grid of 80x80 tiles: 4 columns
(button states) x 6 rows (animals). Row order top-to-bottom:

    pig, panda, deer, tiger, elephant, fox

Column order left-to-right (button state, same layout used by the
CircuitPython TileGrid and the Arduino Button::Icon enum):

    tile 0: normal / unpressed
    tile 1: pressed
    tile 2: unpressed + indicator   (latching buttons only)
    tile 3: pressed + indicator     (latching buttons only)

Non-latching animals only use tiles 0-1; their sheet columns 2-3 are unused
filler and are not emitted. This mirrors the CircuitPython side, which
slices the same buttons.png into per-animal image/*.bmp files for
displayio.OnDiskBitmap - the Arduino build instead reads buttons.png
directly, with no intermediate per-animal bitmap step.

buttons.png must be an 8-bit, non-interlaced PNG exported at "RGB 8" or
"RGBA 8" bit depth (Inkscape's PNG export dialog). RGB565 has no alpha
channel, so RGB 8 is the recommended export setting - it's smaller and
side-steps any accidental partial-alpha edge pixels; RGBA 8 is also
accepted; every pixel must be fully opaque either way. Compression level
and interlacing are export-only settings that don't affect the decoded
pixels, except: interlaced ("Adam7") PNGs are not supported and must be
turned off at export.

Run from anywhere; paths are resolved relative to this script:
    python convert_images.py
"""

import os
import struct
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PNG_PATH = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "..", "buttons.png"))
OUTPUT_PATH = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "Images.h"))

TILE_SIZE = 80
SHEET_COLUMNS = 4

# Row order in buttons.png (top to bottom), paired with how many of the 4
# state columns each button actually uses (2 for momentary, 4 for latching).
ROWS = [
    ("pig", 4),
    ("panda", 4),
    ("deer", 2),
    ("tiger", 2),
    ("elephant", 2),
    ("fox", 2),
]


def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _paeth_predictor(a, b, c):
    """PNG Paeth filter predictor (PNG spec 9.2)."""
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def load_png_rgba(path):
    """Return (width, height, flat bytearray of RGBA bytes) for an 8-bit,
    non-interlaced PNG using colour type 2 (RGB) or 6 (RGBA) - i.e. Inkscape's
    "RGB 8" or "RGBA 8" PNG export bit depth. RGB input has no alpha channel
    to reconstruct, so it's normalized here to RGBA with alpha=255 (opaque),
    giving callers a uniform 4-bytes-per-pixel buffer regardless of which
    bit depth buttons.png was exported with. Anything else raises ValueError."""
    with open(path, "rb") as f:
        data = f.read()

    if data[0:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG file")

    width = height = bit_depth = color_type = None
    idat_chunks = []
    pos = 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        chunk_type = data[pos + 4:pos + 8]
        chunk_data = data[pos + 8:pos + 8 + length]
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, compression, filter_method, interlace = (
                struct.unpack(">IIBBBBB", chunk_data)
            )
            if compression != 0 or filter_method != 0 or interlace != 0:
                raise ValueError(f"{path}: unsupported PNG compression/filter/interlace method")
        elif chunk_type == b"IDAT":
            idat_chunks.append(chunk_data)
        elif chunk_type == b"IEND":
            break
        pos += 8 + length + 4  # length + type + data + CRC

    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError(
            f"{path}: only 8-bit RGB or RGBA PNGs are supported (got bitdepth={bit_depth}, "
            f"colortype={color_type}); in Inkscape's PNG export dialog, set bit depth to "
            "'RGB 8' or 'RGBA 8'"
        )

    src_channels = 3 if color_type == 2 else 4
    stride = width * src_channels
    raw = zlib.decompress(b"".join(idat_chunks))

    pixels = bytearray(width * height * 4)  # always normalized to RGBA
    prev_row = bytearray(stride)  # implicit all-zero row above the first scanline
    offset = 0
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        row = bytearray(raw[offset:offset + stride])
        offset += stride

        # Undo the per-scanline PNG filter in place. Left/upper neighbours
        # must be read from already-reconstructed bytes, so this runs
        # byte-by-byte in increasing x order (row[x] is written before it's
        # needed as row[x - src_channels] for a later x).
        for x in range(stride):
            left = row[x - src_channels] if x >= src_channels else 0
            up = prev_row[x]
            up_left = prev_row[x - src_channels] if x >= src_channels else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filter_type == 2:
                row[x] = (row[x] + up) & 0xFF
            elif filter_type == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                row[x] = (row[x] + _paeth_predictor(left, up, up_left)) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"{path}: unsupported PNG filter type {filter_type}")

        if src_channels == 4:
            pixels[y * width * 4:(y + 1) * width * 4] = row
        else:
            # RGB source: synthesize an opaque alpha byte per pixel so the
            # returned buffer matches the RGBA layout extract_tile_rows()
            # expects, regardless of the PNG's source colour type.
            dst_start = y * width * 4
            for px in range(width):
                s = px * 3
                d = dst_start + px * 4
                pixels[d:d + 3] = row[s:s + 3]
                pixels[d + 3] = 255

        prev_row = row

    return width, height, pixels


def extract_tile_rows(sheet_width, sheet_pixels, row_index, tile_count):
    """Slice tile_count tiles (tile_count * TILE_SIZE wide, TILE_SIZE tall)
    starting at sheet row row_index, converting each opaque RGBA pixel to
    RGB565. Returns (width, rows) where rows is TILE_SIZE lists of RGB565
    values."""
    width = tile_count * TILE_SIZE
    y0 = row_index * TILE_SIZE
    rows = []
    for y in range(y0, y0 + TILE_SIZE):
        line_start = y * sheet_width * 4
        row = []
        for x in range(width):
            o = line_start + x * 4
            r, g, b, a = sheet_pixels[o:o + 4]
            if a != 255:
                raise ValueError(
                    f"buttons.png: unexpected non-opaque pixel at ({x}, {y}) (alpha={a}); "
                    "the RGB565 output format has no alpha channel"
                )
            row.append(rgb888_to_rgb565(r, g, b))
        rows.append(row)
    return width, rows


def emit_array(f, name, row_index, width, height, rows):
    f.write(f"// Source: buttons.png row {row_index} ({name}; {width}x{height}, "
            f"{width // TILE_SIZE} tile(s) of {TILE_SIZE}x{TILE_SIZE})\n")
    f.write(f"const uint16_t {name}_data[{width * height}] = {{\n")
    for row in rows:
        f.write("    " + ", ".join(f"0x{v:04X}" for v in row) + ",\n")
    f.write("};\n\n")


def main():
    sheet_width, sheet_height, sheet_pixels = load_png_rgba(PNG_PATH)
    expected_width = SHEET_COLUMNS * TILE_SIZE
    expected_height = len(ROWS) * TILE_SIZE
    if sheet_width != expected_width or sheet_height != expected_height:
        raise ValueError(
            f"buttons.png: expected {expected_width}x{expected_height} "
            f"({SHEET_COLUMNS} columns x {len(ROWS)} rows of {TILE_SIZE}x{TILE_SIZE} tiles), "
            f"got {sheet_width}x{sheet_height}"
        )

    with open(OUTPUT_PATH, "w", newline="\n") as f:
        f.write("// Auto-generated by tools/convert_images.py - do not edit by hand.\n")
        f.write("// Button state bitmaps sliced from the shared ../../../buttons.png sprite\n")
        f.write("// sheet into flat RGB565 pixel arrays, stored in flash (const => .rodata/XIP\n")
        f.write("// on RP2040).\n")
        f.write("//\n")
        f.write("// Each array is a horizontal strip of 80x80 pixel tiles: tile N starts at\n")
        f.write("// pixel column N*80. Use Button::TILE_SIZE (80) to compute tile offsets.\n")
        f.write("#pragma once\n\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(f"static const int IMAGE_TILE_SIZE = {TILE_SIZE};\n\n")

        for row_index, (name, tile_count) in enumerate(ROWS):
            width, rows = extract_tile_rows(sheet_width, sheet_pixels, row_index, tile_count)
            emit_array(f, name, row_index, width, TILE_SIZE, rows)

    print(f"Wrote {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
