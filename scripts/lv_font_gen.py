#!/usr/bin/env python3
"""Render a TTF into an LVGL v8 `lv_font_t` C source.

    uv run scripts/lv_font_gen.py --font f.ttf --size 21 --bpp 4 \
        --lv-font-name FONT_BODY27 -r 0x20-0x7E,0xA0-0x17F -o out.c

A drop-in replacement for the subset of `lv_font_conv` this project uses, so the asset
pipeline needs one runtime instead of two. Output is BYTE-IDENTICAL to lv_font_conv 1.5.3
for every face in scripts/assets.toml - verified by scripts/lv_font_gen_verify.py, which
diffs this against the real thing.

WHY THIS CAN BE BYTE-IDENTICAL AT ALL
-------------------------------------
lv_font_conv does not have its own rasteriser. It calls FreeType, compiled to WASM, with
    FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT
and freetype-py binds the same library, so the glyph bitmaps come out the same. That was
measured before any of this was written: 565 glyphs across all 12 generated faces, zero
differing pixels. Everything else - the packing, the cmap split, the C text - is
deterministic bookkeeping.

The autohinter's output IS version-sensitive, so a freetype-py upgrade could in principle
move a pixel. That is what the verifier is for; run it after bumping the dependency.

DELIBERATELY NOT IMPLEMENTED
----------------------------
Only what the manifest actually asks for is here, because unused code in a converter is
untested code that silently produces wrong firmware:

  * compression        - the pipeline always passes --no-compress
  * kerning tables     - every face resolves to kern_dsc = NULL, because the instanced
                         fonts carry GPOS rather than a legacy `kern` table. --kerning is
                         accepted and ignored, matching that outcome; if a font ever does
                         carry `kern`, the verifier will fail rather than quietly drop it
  * subpixel, LCD, mono, colour glyphs, multi-font merging, the bin/dump writers

Each of those exits with a clear error rather than emitting something plausible.
"""

import argparse
import sys
from pathlib import Path

try:
    import freetype
except ImportError:
    sys.exit("freetype-py is missing - run this with uv:  uv run scripts/lv_font_gen.py ...")


# lv_font_conv's flags, from lib/freetype/index.js. Do not change these without re-running
# the verifier: they decide every pixel.
FT_FLAGS = (freetype.FT_LOAD_RENDER
            | freetype.FT_LOAD_FORCE_AUTOHINT
            | freetype.FT_LOAD_TARGET_LIGHT)


def js_round(x):
    """Math.round: half goes UP, i.e. toward +infinity.

    Python's round() is banker's rounding, so round(2.5) is 2 where JavaScript gives 3.
    Every metric here is a rounded scale, so the difference is visible in the output.
    """
    import math
    return math.floor(x + 0.5)


# -- glyph collection --------------------------------------------------------------------

class Glyph:
    __slots__ = ("code", "adv", "w", "h", "ox", "oy", "pixels", "bitmap_index")

    def __init__(self, code, adv, w, h, ox, oy, pixels):
        self.code = code
        self.adv = adv          # advance in pixels, float
        self.w = w
        self.h = h
        self.ox = ox            # bbox x, from the pen position
        self.oy = oy            # bbox y, from the baseline (negative = below)
        self.pixels = pixels    # list of rows, each a list of 0..(2^bpp - 1)
        self.bitmap_index = 0


def parse_ranges(text):
    """"0x20-0x7E,0xA0" -> a sorted list of codepoints.

    Also accepts decimal. Matches lv_font_conv's -r, minus the symbol-remapping form
    (`0x20=>0x40`), which this project does not use.
    """
    out = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        if "=>" in part:
            sys.exit(f"range remapping is not supported: {part!r}")
        if "-" in part[1:]:                       # not a leading minus
            a, b = part.split("-", 1) if not part.startswith("-") else (None, None)
            lo, hi = int(a, 0), int(b, 0)
            if hi < lo:
                sys.exit(f"empty range {part!r}")
            out += list(range(lo, hi + 1))
        else:
            out.append(int(part, 0))
    return sorted(set(out))


def collect(ttf: Path, px: int, bpp: int, codes):
    """Rasterise every requested codepoint. Missing ones are dropped, as lv_font_conv does."""
    face = freetype.Face(str(ttf))
    face.set_pixel_sizes(0, px)
    shift = 8 - bpp
    glyphs = []
    for cp in codes:
        if face.get_char_index(cp) == 0 and cp != 0:
            continue                              # not in the font
        face.load_char(cp, FT_FLAGS)
        g = face.glyph
        b = g.bitmap
        rows = []
        for y in range(b.rows):
            line = b.buffer[y * b.pitch: y * b.pitch + b.width]
            rows.append([v >> shift for v in line])
        glyphs.append(Glyph(code=cp,
                            adv=g.linearHoriAdvance / 65536.0,
                            w=b.width, h=b.rows,
                            ox=g.bitmap_left, oy=g.bitmap_top - b.rows,
                            pixels=rows))
    if not glyphs:
        sys.exit("no glyphs matched the requested ranges")
    return face, glyphs


# -- bitmap packing ----------------------------------------------------------------------

def pack_bitmaps(glyphs, bpp):
    """Concatenate every glyph's pixels into one byte array.

    Rows run straight on with no per-row padding (lv_font_conv's storePixelsRaw), but each
    GLYPH starts on a byte boundary, because glyph_dsc.bitmap_index is a byte offset.
    """
    out = bytearray()
    acc = 0          # bit accumulator
    nbits = 0
    for g in glyphs:
        g.bitmap_index = len(out)
        for row in g.pixels:
            for v in row:
                acc = (acc << bpp) | v
                nbits += bpp
                while nbits >= 8:
                    nbits -= 8
                    out.append((acc >> nbits) & 0xFF)
                    acc &= (1 << nbits) - 1
        if nbits:                                  # pad this glyph out to a byte
            out.append((acc << (8 - nbits)) & 0xFF)
            acc = 0
            nbits = 0
    return bytes(out)


# -- cmap --------------------------------------------------------------------------------

def cmap_split(codes):
    """Split codepoints into the cheapest set of subtables.

    A direct port of lv_font_conv's lib/font/cmap_build_subtables.js: a shortest-path scan
    over the sorted codepoints, costing each candidate run as one of three encodings. The
    sizes are the on-device table sizes, so the choice minimises flash.

    Relies on glyph ids being consecutive and ordered with the codepoints, which they are
    here - so the ids never enter the calculation.
    """
    INF = float("inf")
    n = len(codes)
    best = [None] * n
    for i in range(n):
        cur = {"dist": INF}
        for j in range(i + 1):
            prev = best[j - 1]["dist"] if j - 1 >= 0 else 0
            span = codes[i] - codes[j]
            if span < 256:
                s = 16 + (codes[i] - codes[j] + 1)          # format0: one byte per code
                if prev + s < cur["dist"]:
                    cur = {"dist": prev + s, "start": j, "end": i, "format": "format0"}
            if span < 256 and codes[i] - i == codes[j] - j:
                s = 16                                      # format0_tiny: nothing stored
                if prev + s < cur["dist"]:
                    cur = {"dist": prev + s, "start": j, "end": i, "format": "format0_tiny"}
            if span < 65536:
                s = 16 + (i - j + 1) * 2                    # sparse_tiny: u16 per code
                if prev + s < cur["dist"]:
                    cur = {"dist": prev + s, "start": j, "end": i, "format": "sparse_tiny"}
        best[i] = cur

    out = []
    i = n - 1
    while i >= 0:
        p = best[i]
        out.insert(0, (p["format"], codes[p["start"]: p["end"] + 1]))
        i = p["start"] - 1
    return out


CMAP_ENUM = {
    "format0_tiny": "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY",
    "format0":      "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL",
    "sparse_tiny":  "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY",
    "sparse":       "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL",
}


# -- C text ------------------------------------------------------------------------------
#
# Everything below reproduces lv_font_conv's output character for character. The shapes are
# not negotiable - the pipeline pastes this text into a shared header (build_assets.py's
# font_body), and the committed LvFontsGen.h is compared byte for byte.

def long_dump(values, hexfmt=False, per_line=8):
    """u.long_dump: eight values a line, four-space indent, commas AT THE JOINS.

    The comma placement matters more than it looks: values and lines are joined with
    commas, so the last value in the whole array carries none. Appending a comma per value
    instead is a one-character diff at the end of a 30 KB array.
    """
    items = [f"0x{v:x}" if hexfmt else str(v) for v in values]
    lines = ["    " + ", ".join(items[i:i + per_line])
             for i in range(0, len(items), per_line)]
    return ",\n".join(lines)


def dump_bytes(data, per_line=8):
    """The glyph bitmap dump: hex, eight per line."""
    return long_dump(list(data), hexfmt=True, per_line=per_line)


def char_comment(cp):
    """The `/* U+0041 "A" */` label that precedes each glyph's bytes.

    The character is escaped as JSON.stringify would, since that is what lv_font_conv uses:
    a double quote becomes \\" and a backslash becomes \\\\.
    """
    ch = chr(cp)
    if cp < 0x20 or cp == 0x7F:
        ch = ""
    ch = ch.replace("\\", "\\\\").replace('"', '\\"')
    return f'    /* U+{cp:04X} "{ch}" */'


def emit(face, ttf_path, glyphs, bitmap, name, px, bpp, opts_line, cmaps):
    lh_ascent = max(g.oy + g.h for g in glyphs)
    lh_descent = min(g.oy for g in glyphs)

    # Underline metrics come from the POST TABLE, scaled to the pixel size.
    #
    # NOT face.underline_position. FreeType adjusts that value: for Montserrat it reports
    # -125 where post says -100, which at 21px is -2.625 against -2.1 and rounds to a
    # different pixel. lv_font_conv reads the raw post table, so this does too.
    from fontTools.ttLib import TTFont
    post = TTFont(str(ttf_path), lazy=True)["post"]
    scale = px / face.units_per_EM
    ul_pos = js_round(post.underlinePosition * scale)
    ul_thick = js_round(post.underlineThickness * scale)

    out = []
    out.append("/" + "*" * 79)
    out.append(f" * Size: {px} px")
    out.append(f" * Bpp: {bpp}")
    out.append(f" * Opts: {opts_line}")
    out.append(" " + "*" * 78 + "/")     # both banner lines are 80 columns
    out.append("")
    out.append("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    out.append('#include "lvgl.h"')
    out.append("#else")
    out.append('#include "lvgl/lvgl.h"')
    out.append("#endif")
    out.append("")
    out.append(f"#ifndef {name}")
    out.append(f"#define {name} 1")
    out.append("#endif")
    out.append("")
    out.append(f"#if {name}")
    out.append("")
    out.append("/*-----------------")
    out.append(" *    BITMAPS")
    out.append(" *----------------*/")
    out.append("")
    out.append("/*Store the image of the glyphs*/")
    out.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    # Each glyph is its comment then its bytes. The separator carries the comma, so the
    # LAST glyph gets none - and a glyph with no bytes at all (space) is followed by a bare
    # newline rather than ",\n\n", which is why this is assembled as one string instead of
    # a list of lines.
    blocks = []
    for i, g in enumerate(glyphs):
        end = glyphs[i + 1].bitmap_index if i + 1 < len(glyphs) else len(bitmap)
        blob = bitmap[g.bitmap_index:end]
        txt = char_comment(g.code) + "\n" + dump_bytes(blob)
        if i < len(glyphs) - 1:
            txt += ",\n\n" if len(blob) else "\n"
        blocks.append(txt)
    out.append("".join(blocks))
    out.append("};")
    out.append("")
    out.append("")
    out.append("/*---------------------")
    out.append(" *  GLYPH DESCRIPTION")
    out.append(" *--------------------*/")
    out.append("")
    out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    rows = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, "
            ".ofs_y = 0} /* id = 0 reserved */"]
    for g in glyphs:
        rows.append(f"    {{.bitmap_index = {g.bitmap_index}, .adv_w = {js_round(g.adv * 16)}, "
                    f".box_w = {g.w}, .box_h = {g.h}, .ofs_x = {g.ox}, .ofs_y = {g.oy}}}")
    out.append(",\n".join(rows))
    out.append("};")
    out.append("")
    out.append(cmaps)
    out.append("")
    out.append("")
    out.append("")
    out.append("/*--------------------")
    out.append(" *  ALL CUSTOM DATA")
    out.append(" *--------------------*/")
    out.append("")
    out.append("#if LVGL_VERSION_MAJOR == 8")
    out.append("/*Store all the custom data of the font*/")
    out.append("static  lv_font_fmt_txt_glyph_cache_t cache;")
    out.append("#endif")
    out.append("")
    out.append("#if LVGL_VERSION_MAJOR >= 8")
    out.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    out.append("#else")
    out.append("static lv_font_fmt_txt_dsc_t font_dsc = {")
    out.append("#endif")
    out.append("    .glyph_bitmap = glyph_bitmap,")
    out.append("    .glyph_dsc = glyph_dsc,")
    out.append("    .cmaps = cmaps,")
    out.append("    .kern_dsc = NULL,")
    out.append("    .kern_scale = 0,")
    out.append(f"    .cmap_num = {cmaps.count('.range_start')},")
    out.append(f"    .bpp = {bpp},")
    out.append("    .kern_classes = 0,")
    out.append("    .bitmap_format = 0,")
    out.append("#if LVGL_VERSION_MAJOR == 8")
    out.append("    .cache = &cache")
    out.append("#endif")
    out.append("};")
    out.append("")
    out.append("")
    out.append("")
    out.append("/*-----------------")
    out.append(" *  PUBLIC FONT")
    out.append(" *----------------*/")
    out.append("")
    out.append("/*Initialize a public general font descriptor*/")
    out.append("#if LVGL_VERSION_MAJOR >= 8")
    out.append(f"const lv_font_t {name} = {{")
    out.append("#else")
    out.append(f"lv_font_t {name} = {{")
    out.append("#endif")
    out.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    "
               "/*Function pointer to get glyph's data*/")
    out.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    "
               "/*Function pointer to get glyph's bitmap*/")
    out.append(f"    .line_height = {lh_ascent - lh_descent},          "
               "/*The maximum line height required by the font*/")
    out.append(f"    .base_line = {-lh_descent},             "
               "/*Baseline measured from the bottom of the line*/")
    out.append("#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)")
    out.append("    .subpx = LV_FONT_SUBPX_NONE,")
    out.append("#endif")
    out.append("#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8")
    out.append(f"    .underline_position = {ul_pos},")
    out.append(f"    .underline_thickness = {ul_thick},")
    out.append("#endif")
    out.append("    .dsc = &font_dsc,          "
               "/*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */")
    out.append("#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9")
    out.append("    .fallback = NULL,")
    out.append("#endif")
    out.append("    .user_data = NULL,")
    out.append("};")
    out.append("")
    out.append("")
    out.append("")
    out.append(f"#endif /*#if {name}*/")
    # The file ends "*/\n\n" - the guard line, then one blank line.
    return "\n".join(out) + "\n\n"


def build_cmaps(glyphs):
    """The CHARACTER MAPPING section, text and all."""
    codes = [g.code for g in glyphs]
    plan = cmap_split(codes)
    defs, heads = [], []
    for idx, (fmt, cps) in enumerate(plan):
        start_gid = codes.index(cps[0]) + 1          # ids are 1-based, glyph 0 is reserved
        lo, hi = cps[0], cps[-1]
        u_list = "NULL"
        id_list = "NULL"
        count = 0
        if fmt == "format0_tiny":
            pass
        elif fmt == "format0":
            # One byte per codepoint across the whole span: the glyph id's DELTA from this
            # subtable's start id, and 0 for codepoints the font does not carry.
            gid_of = {c: i + 1 for i, c in enumerate(codes)}
            ids = [gid_of[c] - start_gid if c in gid_of else 0 for c in range(lo, hi + 1)]
            if any(d < 0 or d > 255 for d in ids):
                sys.exit("glyph id delta out of format0 range")
            count = len(ids)
            defs.append(f"static const uint8_t glyph_id_ofs_list_{idx}[] = {{\n"
                        f"{long_dump(ids)}\n}};")
            id_list = f"glyph_id_ofs_list_{idx}"
        elif fmt == "sparse_tiny":
            rel = [c - lo for c in cps]
            count = len(rel)
            defs.append(f"static const uint16_t unicode_list_{idx}[] = {{\n"
                        f"{long_dump(rel, hexfmt=True)}\n}};")
            u_list = f"unicode_list_{idx}"
        else:
            sys.exit(f"cmap format {fmt} is not implemented")
        heads.append(
            "    {\n"
            f"        .range_start = {lo}, .range_length = {hi - lo + 1}, "
            f".glyph_id_start = {start_gid},\n"
            f"        .unicode_list = {u_list}, .glyph_id_ofs_list = {id_list}, "
            f".list_length = {count}, .type = {CMAP_ENUM[fmt]}\n"
            "    }")
    body = ["/*---------------------", " *  CHARACTER MAPPING", " *--------------------*/", ""]
    if defs:
        body.append("\n\n".join(defs))
        body.append("")
    else:
        body.append("")
        body.append("")
    body.append("/*Collect the unicode lists and glyph_id offsets*/")
    body.append("static const lv_font_fmt_txt_cmap_t cmaps[] =")
    body.append("{")
    body.append(",\n".join(heads))
    body.append("};")
    return "\n".join(body)


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("--font", required=True)
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--bpp", type=int, required=True, choices=(1, 2, 4, 8))
    ap.add_argument("--format", default="lvgl", choices=("lvgl",))
    ap.add_argument("--lv-font-name", required=True)
    ap.add_argument("--no-compress", action="store_true")
    ap.add_argument("--no-kerning", action="store_true")
    ap.add_argument("-r", "--range", required=True)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--opts-line", default=None,
                    help="verbatim text for the `Opts:` banner, when reproducing an "
                         "lv_font_conv file exactly")
    a = ap.parse_args()

    if not a.no_compress:
        sys.exit("only --no-compress is implemented; the compressed format is not used here")

    ttf = Path(a.font)
    codes = parse_ranges(a.range)
    face, glyphs = collect(ttf, a.size, a.bpp, codes)
    bitmap = pack_bitmaps(glyphs, a.bpp)
    cmaps = build_cmaps(glyphs)

    opts = a.opts_line or (
        f"--font {a.font} --size {a.size} --bpp {a.bpp} --format lvgl "
        f"--lv-font-name {a.lv_font_name} --no-compress -r {a.range} -o {a.output}"
        + (" --no-kerning" if a.no_kerning else ""))

    text = emit(face, ttf, glyphs, bitmap, a.lv_font_name, a.size, a.bpp, opts, cmaps)
    Path(a.output).write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
