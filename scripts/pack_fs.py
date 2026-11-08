#!/usr/bin/env python3
"""Assemble data/ -- the LittleFS image PlatformIO builds -- from three sources.

  generated  scripts/build_assets.py output in assets/build/   (freely licensed sources)
  html       this project's own web UI in html/, gzipped on the way in
  stock      a stock unit's filesystem cached in assets/cache/stock/

Run directly, or let scripts/prebuild.py call it during a build:

  python scripts/pack_fs.py            assemble data/
  python scripts/pack_fs.py --check    report what is missing, write nothing
  python scripts/pack_fs.py --clean    remove data/ first

TWO WAYS TO GET A WEB UI, chosen with --web:

  --web html    (default)  gzip the pages in html/. Nothing external is needed, so a plain
                           checkout can build a complete filesystem.
  --web stock              use the pages captured off a factory unit into
                           assets/cache/stock/. Needs such a unit, or a cache from one.

Set SMOLTV_WEB_UI=stock to make that the default for a shell or a build.

VENDOR ARTWORK IS OPT IN, chosen with --art:

  --art none    (default)  only what this project generates - earth.gif and favicon.ico.
  --art stock              adds the factory weather GIFs and one gallery image from
                           assets/cache/stock/.

Set SMOLTV_ART=stock to make that the default. ON THE DEFAULTS NOTHING COMES OFF A FACTORY
UNIT, so a clean checkout builds a complete filesystem with no capture, no device and no
network - which is what a clean install is.

EVERY .html ON THE DEVICE IS GZIP BYTES UNDER A PLAIN NAME. The firmware serves anything
ending .html with Content-Encoding: gzip and never decompresses, so the filesystem holds
compressed bytes at /index.html. Two consequences:

  * "stock" sources are already compressed. device_fs.py names a captured file after its
    CONTENT, so they arrive as index.html.gz; packing copies them verbatim and drops the
    suffix. Decompressing here would produce a filesystem served as broken HTML.
  * "html" sources are plain text this project maintains, so they are gzipped here. mtime is
    forced to 0 -- a gzip header records it, and letting it float would make data/ differ on
    every pack and defeat the unchanged-content check below.

data/ is entirely generated and is gitignored. Nothing should ever be edited in place
there -- change LAYOUT below, or the thing that produces the source file.
"""

import argparse
import gzip
import os
import shutil
import sys
from pathlib import Path
from typing import List, Tuple

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "assets" / "build"
STOCK = ROOT / "assets" / "cache" / "stock"
HTML = ROOT / "html"
DATA = ROOT / "data"

# -- What goes on the device -----------------------------------------------------------
#
# (device path, source kind, source path relative to that kind's root)
#
# EDIT THIS TABLE to change the filesystem contents. Nothing else needs to change; the
# packer works out directories, reports sizes and checks the partition budget.
#
#   "gen"    assets/build/          produced by scripts/build_assets.py, freely licensed
#   "html"   html/                  this project's web UI, gzipped on the way in
#   "stock"  assets/cache/stock/    captured off a factory device by scripts/device_fs.py
#
# Deliberately NOT included:
#   ii.html        byte-identical duplicate of index.html on a live unit, and nothing
#                  serves it -- / serves index.html. Saves 25 KB.
#   /.sys/*        every config file is written by the firmware from its own defaults on
#                  first use. Shipping them would freeze one unit's settings into a build.
#                  A device boots fine with no /.sys at all - the factory-reset path relies
#                  on exactly that.
#
# The three pages come from ONE of WEB_UI below, not from LAYOUT, because there are two ways
# to obtain them - see the module docstring.
Entry = Tuple[str, str, str]

WEB_UI: dict = {
    # This project's own pages. Plain text on disk, gzipped by the packer. All three link
    # /style.css, so it is part of THIS choice and not of the layout at large - the stock
    # pages below carry their CSS inline and would 404 on it.
    "html": [
        ("/index.html",               "html",  "index.html"),
        ("/portal.html",              "html",  "portal.html"),
        ("/captive.html",             "html",  "captive.html"),
        ("/style.css",                "html",  "style.css"),
        # GET /update serves this when it is present and falls back to a page in .rodata when
        # it is not, so shipping it is optional - but it is 1.5 KB and it matches the rest of
        # the UI, so the default layout carries it.
        ("/update.html",              "html",  "update.html"),
    ],
    # Captured off a factory unit, already gzip. Needs such a unit to exist.
    "stock": [
        ("/index.html",               "stock", "index.html.gz"),
        ("/portal.html",              "stock", "portal.html.gz"),
        ("/captive.html",             "stock", "captive.html.gz"),
    ],
}
DEFAULT_WEB_UI = "html"

# Everything this project can produce for itself. A clean install is exactly this plus a web
# UI, and it needs no factory device, no capture and no network.
LAYOUT: List[Entry] = [
    ("/favicon.ico",                  "gen",   "icos/favicon.ico"),
    ("/image/80x80.gif/earth.gif",    "gen",   "gifs/earth.gif"),
]

# Vendor artwork, and therefore OPT IN - see --art.
#
# Nothing needs it: the weather screen's GIF picker offers whatever lives in
# /image/80x80.gif, so earth.gif alone is a valid choice, and the Picture app just scans
# /image, so an empty gallery is a valid state (it draws its own "No images" screen).
STOCK_ART: List[Entry] = [
    ("/image/80x80.gif/cube.gif",     "stock", "image/80x80.gif/cube.gif"),
    ("/image/80x80.gif/space.gif",    "stock", "image/80x80.gif/space.gif"),
    ("/image/80x80.gif/spaceman.gif", "stock", "image/80x80.gif/spaceman.gif"),
    ("/image/bootxp.gif",             "stock", "image/bootxp.gif"),
]
DEFAULT_ART = "none"

# From partitions.csv: the spiffs/littlefs partition. Used only to warn before a build
# produces an image that cannot fit.
PARTITION_BYTES = 0x3B0000      # 3,866,624


def web_ui_choice(explicit: str = "") -> str:
    """Which web UI to pack: the argument, else $SMOLTV_WEB_UI, else the default."""
    choice = (explicit or os.environ.get("SMOLTV_WEB_UI", "") or DEFAULT_WEB_UI).strip()
    if choice not in WEB_UI:
        sys.exit(f"FATAL: unknown web UI source {choice!r} "
                 f"(expected one of: {', '.join(sorted(WEB_UI))})")
    return choice


def art_choice(explicit: str = "") -> str:
    """Whether to include the vendor artwork: the argument, else $SMOLTV_ART, else default."""
    choice = (explicit or os.environ.get("SMOLTV_ART", "") or DEFAULT_ART).strip()
    if choice not in ("none", "stock"):
        sys.exit(f"FATAL: unknown art source {choice!r} (expected none or stock)")
    return choice


def layout(web: str = "", art: str = "") -> List[Entry]:
    """The full filesystem contents for a given web-UI source and artwork choice."""
    out = WEB_UI[web_ui_choice(web)] + LAYOUT
    if art_choice(art) == "stock":
        out = out + STOCK_ART
    return out


def stock_device_paths(web: str = "", art: str = "") -> List[str]:
    """Device paths for the stock-sourced entries, given the choices made.

    The cache mirrors device_fs's output, where gzip content carries a .gz suffix, so the
    device path is the cache-relative path with that suffix removed. Used by prebuild.py to
    fetch ONLY these, rather than pulling a whole gallery a build does not need - and on the
    defaults the list is EMPTY, so a build needs no factory device at all.
    """
    out = []
    for _dev, kind, rel in layout(web, art):
        if kind != "stock":
            continue
        p = rel[:-3] if rel.endswith(".gz") else rel
        out.append("/" + p.replace("\\", "/"))
    return out


def _src_root(kind: str) -> Path:
    if kind == "gen":
        return BUILD
    if kind == "html":
        return HTML
    if kind == "stock":
        return STOCK
    sys.exit(f"FATAL: unknown source kind {kind!r} in LAYOUT")


def _bytes_for(kind: str, src: Path) -> bytes:
    """The bytes that go on the device, which are not always the bytes on disk."""
    data = src.read_bytes()
    if kind == "html":
        # mtime=0 so identical HTML always yields identical gzip bytes.
        data = gzip.compress(data, compresslevel=9, mtime=0)
    return data


def resolve(web: str = "", art: str = "") -> Tuple[List[tuple], List[str]]:
    """Return (present, missing) for every entry the given choices imply."""
    present, missing = [], []
    for dev_path, kind, rel in layout(web, art):
        src = _src_root(kind) / rel
        if src.is_file():
            present.append((dev_path, kind, src))
        else:
            missing.append(f"{dev_path:32} <- {kind}: {src.relative_to(ROOT)}")
    return present, missing


def report_missing(missing: List[str]) -> None:
    print(f"  {len(missing)} source file(s) missing:")
    for m in missing:
        print(f"    {m}")
    if any("html:" in m for m in missing):
        print("\n  Web UI sources are missing from html/ -- that directory is part of the")
        print("  project and a checkout should have it.")
    need_stock = any("stock:" in m for m in missing)
    need_gen = any("gen:" in m for m in missing)
    if need_gen:
        print("\n  Generated assets are missing. Build them with:")
        print("    python scripts/build_assets.py --fetch --render --emit")
    if need_stock:
        print("\n  Stock assets are missing. Capture them from a stock unit with:")
        print("    python scripts/device_fs.py backup --host <ip> "
              "--output assets/cache/stock")
        if any(".html.gz" in m for m in missing):
            print("\n  The missing web UI can come from this project instead, which needs no")
            print("  stock unit:  --web html")


def pack(clean: bool = False, web: str = "", art: str = "") -> int:
    web = web_ui_choice(web)
    art = art_choice(art)
    print(f"  web UI: {web}   artwork: {art}")
    present, missing = resolve(web, art)
    if missing:
        report_missing(missing)
        return 1

    if clean and DATA.exists():
        shutil.rmtree(DATA)
        print(f"  cleaned {DATA.relative_to(ROOT)}/")

    total = 0
    changed = 0
    for dev_path, kind, src in present:
        dest = DATA / dev_path.lstrip("/")
        dest.parent.mkdir(parents=True, exist_ok=True)
        data = _bytes_for(kind, src)
        # Only rewrite when the content differs, so an unchanged pack does not churn
        # mtimes and retrigger downstream steps on every build.
        if dest.is_file() and dest.read_bytes() == data:
            state = "="
        else:
            dest.write_bytes(data)
            state = "+"
            changed += 1
        total += len(data)
        print(f"  {state} {len(data):>9,}  {dev_path:32} <- {kind}")

    # Anything in data/ the layout no longer lists is stale and would still be flashed.
    wanted = {(DATA / p.lstrip("/")).resolve() for p, _, _ in layout(web, art)}
    for f in sorted(DATA.rglob("*")):
        if f.is_file() and f.resolve() not in wanted:
            f.unlink()
            print(f"  - stale        {f.relative_to(DATA)}")
    for d in sorted((p for p in DATA.rglob("*") if p.is_dir()), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()

    pct = 100.0 * total / PARTITION_BYTES
    print(f"\n  {len(present)} files, {total:,} B "
          f"({pct:.1f}% of the {PARTITION_BYTES:,} B partition), {changed} changed")
    if total > PARTITION_BYTES:
        print("  FATAL: exceeds the filesystem partition")
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Assemble data/ for the LittleFS image")
    ap.add_argument("--check", action="store_true",
                    help="report missing sources and exit without writing")
    ap.add_argument("--clean", action="store_true",
                    help="remove data/ before assembling")
    ap.add_argument("--web", choices=sorted(WEB_UI), default="",
                    help="where the three web pages come from: 'html' gzips this project's "
                         "html/ (default), 'stock' uses the pages captured off a factory "
                         "unit. Overrides $SMOLTV_WEB_UI.")
    ap.add_argument("--art", choices=("none", "stock"), default="",
                    help="vendor artwork: 'none' (default) ships only generated assets, "
                         "'stock' adds the factory weather GIFs and a gallery image. "
                         "Overrides $SMOLTV_ART.")
    a = ap.parse_args()

    if a.check:
        web, art = web_ui_choice(a.web), art_choice(a.art)
        present, missing = resolve(web, art)
        print(f"== pack_fs --check ==\n  web UI: {web}   artwork: {art}\n"
              f"  {len(present)}/{len(layout(web, art))} sources present")
        if missing:
            report_missing(missing)
            return 1
        print("  all sources present")
        return 0

    print("== pack_fs ==")
    return pack(clean=a.clean, web=a.web, art=a.art)


if __name__ == "__main__":
    sys.exit(main())
