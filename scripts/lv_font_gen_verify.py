#!/usr/bin/env python3
"""Diff scripts/lv_font_gen.py against lv_font_conv, face by face.

    uv run scripts/lv_font_gen_verify.py

For every face in scripts/assets.toml this renders the same font twice - once with
lv_font_conv, once with lv_font_gen - using identical arguments, and compares the two C
files byte for byte. Any difference is printed as a unified diff and the run fails.

WHEN TO RUN IT
  * after touching lv_font_gen.py
  * after bumping freetype-py, whose autohinter decides every glyph pixel
  * before trusting a regenerated LvFontsGen.h

lv_font_conv is only needed HERE, by this verifier. If it is not installed the run is
skipped rather than failed, so the check never blocks someone who is not changing fonts:

    npm install lv_font_conv
"""

import difflib
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "scripts" / "assets.toml"
LOCK = ROOT / "scripts" / "assets.lock.toml"
FONTS = ROOT / "assets" / "build" / "fonts"
GEN = ROOT / "scripts" / "lv_font_gen.py"


def find_conv():
    local = ROOT / "node_modules" / ".bin"
    for c in (local / "lv_font_conv.cmd", local / "lv_font_conv"):
        if c.is_file():
            return str(c)
    return shutil.which("lv_font_conv") or shutil.which("lv_font_conv.cmd")


def instanced(family, weight):
    return FONTS / f"{family}-{weight}.ttf"


def main() -> int:
    conv = find_conv()
    if not conv:
        print("lv_font_conv not installed - nothing to verify against.")
        print("  npm install lv_font_conv")
        return 0

    spec = tomllib.loads(SPEC.read_text(encoding="utf-8"))
    lock = tomllib.loads(LOCK.read_text(encoding="utf-8")) if LOCK.is_file() else {}
    sizes = lock.get("sizes", {})

    faces = [(n, f) for n, f in spec["fonts"].items() if f.get("source") != "lvgl-builtin"]
    ok = bad = skip = 0

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for name, f in faces:
            px = sizes.get(name)
            ttf = instanced(f["family"], f["weight"])
            if px is None or not ttf.is_file():
                print(f"SKIP {name:10} (no planned size or missing {ttf.name})")
                skip += 1
                continue

            a = td / f"{name}.conv.c"
            b = td / f"{name}.gen.c"
            args = ["--font", str(ttf), "--size", str(px), "--bpp", str(f["bpp"]),
                    "--format", "lvgl", "--lv-font-name", f["symbol"], "--no-compress",
                    "-r", f["ranges"]]
            if not f["kerning"]:
                args.append("--no-kerning")

            r = subprocess.run([conv] + args + ["-o", str(a)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL {name:10} lv_font_conv errored: {r.stderr.strip()[:120]}")
                bad += 1
                continue

            # The banner records the -o path, so feed lv_font_gen the same one verbatim.
            opts = re.search(r"\* Opts: (.*)", a.read_text(encoding="utf-8")).group(1)
            r = subprocess.run([sys.executable, str(GEN)] + args
                               + ["-o", str(b), "--opts-line", opts],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL {name:10} lv_font_gen errored: {r.stderr.strip()[:200]}")
                bad += 1
                continue

            ta = a.read_text(encoding="utf-8")
            tb = b.read_text(encoding="utf-8")
            if ta == tb:
                print(f"OK   {name:10} {px:>3}px  {len(ta):>7,} bytes identical")
                ok += 1
            else:
                bad += 1
                d = list(difflib.unified_diff(ta.splitlines(), tb.splitlines(),
                                              "lv_font_conv", "lv_font_gen", lineterm="",
                                              n=1))
                print(f"FAIL {name:10} {px:>3}px  {len(d)} diff lines, first 20:")
                for line in d[:20]:
                    print("     " + line)

    print()
    print(f"{ok} identical, {bad} differ, {skip} skipped")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
