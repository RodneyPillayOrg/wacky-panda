#!/usr/bin/env python3
"""Build the firmware's font and image tables from freely licensed upstream sources.

    python scripts/build_assets.py --fetch --plan --render --emit      (or --all)

Stages, each usable on its own so a failure never forces a full redo:

  --fetch   Download the upstream fonts and icon sets into assets/cache/ and their
            licences into LICENSES/. Records a SHA-256 for every file in
            scripts/assets.lock.toml; on later runs a changed hash is a hard error, so
            an upstream edit can never silently change what the firmware looks like.

  --plan    Resolve each face's pixel size. assets.toml states the line_height the
            firmware's layout depends on, NOT a px size -- those are not the same number
            and the relationship differs per typeface. This searches for the px size
            whose rendered line_height matches, and writes it to the lock file.

  --render  Instance each variable font at its weight, convert each face to an LVGL
            table, and rasterise each SVG icon to the pixel size the firmware blits.

  --emit    Assemble src/LvFontsGen.h and src/LvImagesGen.h from the rendered output.

Design rules worth keeping:

  * assets.toml is the frozen spec and is NEVER written by this script. Everything the
    script derives goes to scripts/assets.lock.toml. A build cannot rewrite its own
    input, so a bad run is always recoverable by deleting the lock file.

  * Variable fonts MUST be instanced at an explicit weight. Their default axis value is
    the thinnest instance, so skipping this step yields a hairline face that looks like
    a rendering bug rather than a missing parameter.

  * Kerning is per-face, from the manifest. Adding kern data a face should not have
    changes every string's width and shifts centred text.
"""
import argparse
import hashlib
import json
import math
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "scripts" / "assets.toml"
LOCK = ROOT / "scripts" / "assets.lock.toml"
CACHE = ROOT / "assets" / "cache"
# Subdirectories of CACHE that this pipeline owns and may prune. Anything else under
# assets/cache/ belongs to another tool -- scripts/device_fs.py parks the stock-device
# capture in assets/cache/stock/ -- and must be left alone. Add a directory here only if
# stage_fetch is what fills it.
CACHE_OWNED = ("fonts", "icons", "textures", "icos")
BUILD = ROOT / "assets" / "build"
LICENSES = ROOT / "LICENSES"

# Upstream sources. Fonts come from the Google Fonts repo (the canonical home of these
# OFL families); icons from each set's own repo. Everything is fetched by explicit path
# and hash-locked, never resolved through a search or a "latest" alias.
GF = "https://raw.githubusercontent.com/google/fonts/main/ofl"
FONT_SRC = {
    "montserrat": (f"{GF}/montserrat/Montserrat%5Bwght%5D.ttf", f"{GF}/montserrat/OFL.txt"),
    "inter":      (f"{GF}/inter/Inter%5Bopsz,wght%5D.ttf",      f"{GF}/inter/OFL.txt"),
    "antonio":    (f"{GF}/antonio/Antonio%5Bwght%5D.ttf",       f"{GF}/antonio/OFL.txt"),
    "teko":       (f"{GF}/teko/Teko%5Bwght%5D.ttf",             f"{GF}/teko/OFL.txt"),
    # Static single-weight file, not variable -- see instanced_ttf.
    "chakrapetch": (f"{GF}/chakrapetch/ChakraPetch-Bold.ttf",   f"{GF}/chakrapetch/OFL.txt"),
}
ICON_SRC = {
    # set: (svg url template, licence url, licence spdx)
    # Coin marks. Replaced spothq/cryptocurrency-icons (CC0) because that set stopped being
    # updated and has no SUI at all, which forced a second source for one coin. web3icons
    # carries all twelve, is actively released, and is MIT -- which unlike CC0 obliges us to
    # ship its copyright notice, hence LICENSES/web3icons-MIT.txt.
    #
    # The 'branded' variant (bare colour glyph on transparency) is used, NOT 'background'.
    # 'background' sounds like the coloured disc a coin mark wants, but it rasterises as a
    # hard-edged SQUARE tile -- an orange box for BTC, a white box for DOGE -- which reads as
    # a button rather than a coin. This was checked by rendering both at 32x32 and looking.
    #
    # Two per-coin consequences to watch on a light panel: XRP's glyph is pure white, and
    # ADA's is a dark navy, so both lose contrast. Either can be switched to its
    # 'background' tile individually if a screen turns out to need it.
    "web3icons": (
        "https://raw.githubusercontent.com/0xa3k5/web3icons/main/packages/core/src/svgs/tokens/branded/{icon}.svg",
        "https://raw.githubusercontent.com/0xa3k5/web3icons/main/LICENCE", "MIT"),
    # basmilius/weather-icons has been renamed to basmilius/meteocons. The old name still
    # serves the SVGs, and the layout under it is the one these paths match -- meteocons
    # reorganised the tree, so its own paths differ. The licence, however, only exists
    # under the new name.
    "basmilius": (
        "https://raw.githubusercontent.com/basmilius/weather-icons/dev/production/fill/svg/{icon}.svg",
        "https://raw.githubusercontent.com/basmilius/meteocons/main/LICENSE", "MIT"),
    # Dual-licensed "either MIT or the Good Boy License"; we elect MIT. The repo ships only
    # the joke licence as a file, which is why GitHub reports the repo as NOASSERTION -- the
    # fetched LICENSE.md is kept as the evidence for that election.
    "flat-color-icons": (
        "https://raw.githubusercontent.com/icons8/flat-color-icons/master/svg/{icon}.svg",
        "https://raw.githubusercontent.com/icons8/flat-color-icons/master/LICENSE.md", "MIT"),
}

# Textures for the generated GIFs. Public-domain NASA imagery, hash-locked like everything
# else. The land/ocean/ice composite is used rather than the familiar Blue Marble ANIMATION
# for two reasons: that animation is cloud-dominated, so thresholding it yields cloud blobs
# instead of continents, and this one carries no graticule -- an earlier candidate had grid
# lines drawn on it, which the land threshold picked up as a hard seam at the antimeridian.
GIF_TEXTURE_SRC = {
    "nasa-land-ocean-ice": (
        "https://upload.wikimedia.org/wikipedia/commons/c/cd/Land_ocean_ice_2048.jpg",
        None,          # public domain: there is no licence file to fetch
        "PD-NASA"),
}

# Source for favicon.ico: the brain emoji (U+1F9E0). Twemoji is used unmodified - its heavier
# outline survives 32x32 better than Noto's finer linework, which mushes at that size.
# Note the graphics and the code carry DIFFERENT licences in that repo -- LICENSE covers the
# code (MIT) and LICENSE-GRAPHICS covers the images (CC-BY-4.0). The graphics one is what
# applies here, so it is the one fetched.
ICO_SRC = {
    "twemoji": (
        "https://raw.githubusercontent.com/jdecked/twemoji/main/assets/svg/{icon}.svg",
        "https://raw.githubusercontent.com/jdecked/twemoji/main/LICENSE-GRAPHICS", "CC-BY-4.0"),
}


def load_spec():
    return tomllib.loads(SPEC.read_text(encoding="utf-8"))


def load_lock():
    return tomllib.loads(LOCK.read_text(encoding="utf-8")) if LOCK.exists() else {}


def save_lock(lock):
    """Write the lock file by hand: tomli-w is not a dependency and the shape is fixed."""
    out = ["# GENERATED by scripts/build_assets.py -- do not edit.",
           "# Hashes pin upstream content; sizes/baselines are the --plan results.",
           "#",
           "# Delete this file to force a clean re-fetch and re-plan.",
           ""]
    # The manifest this run built from. prebuild.py compares the CURRENT manifest against
    # it to decide whether the committed outputs are stale. It has to be a hash, not an
    # mtime: after a clone every file carries the checkout time, so mtime says nothing about
    # what was generated from what.
    if SPEC.is_file():
        out.append("[manifest]")
        out.append(f'sha256 = {json.dumps(sha256(SPEC.read_bytes()))}')
        out.append("")
    for section in ("hashes", "sizes", "baselines"):
        if not lock.get(section):
            continue
        out.append(f"[{section}]")
        for k, v in sorted(lock[section].items()):
            out.append(f'"{k}" = {json.dumps(v)}')
        out.append("")
    LOCK.write_text("\n".join(out), encoding="utf-8", newline="\n")


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# -- fetch ----------------------------------------------------------------------------

def fetch_one(url, dest, lock, force=False):
    """Download url to dest, verifying it against the locked hash.

    A hash mismatch is fatal rather than a warning. These files decide what every glyph
    and icon looks like; silently accepting different bytes would change the firmware's
    appearance with nothing in the diff to show it.
    """
    import requests

    # Use the OS trust store, not certifi's bundle. On a network that terminates TLS at a
    # proxy, the chain ends in a CA the machine trusts but certifi has never heard of, so
    # a certifi-only verify fails on every URL here.
    import truststore
    truststore.inject_into_ssl()

    key = str(dest.relative_to(ROOT)).replace("\\", "/")
    if dest.exists() and not force:
        have = sha256(dest.read_bytes())
        want = lock.get("hashes", {}).get(key)
        if want and have != want:
            sys.exit(f"FATAL {key}: cached copy hash {have[:12]} != locked {want[:12]}\n"
                     f"      Delete the file to re-fetch, or the lock entry if the change is intended.")
        # Pin cached files too. Returning early without this leaves everything already on
        # disk unlocked, so the lock file silently covers only whatever the last run
        # happened to download -- the exact files it is meant to protect go unpinned.
        lock.setdefault("hashes", {})[key] = have
        return "cached", dest.read_bytes()

    # A descriptive User-Agent is REQUIRED by some upstreams, not merely polite: Wikimedia
    # answers 403 to the default python-requests agent, which is where the GIF textures live.
    r = requests.get(url, timeout=120, headers={
        "User-Agent": "SmolTV-Pro-assets/1.0 (firmware asset pipeline; "
                      "https://github.com/sweetlilmre)"})
    if r.status_code != 200:
        sys.exit(f"FATAL {url} -> HTTP {r.status_code}")
    body = r.content
    got = sha256(body)
    want = lock.get("hashes", {}).get(key)
    if want and got != want:
        sys.exit(f"FATAL {key}: upstream hash {got[:12]} != locked {want[:12]}\n"
                 f"      Upstream changed. Review the new file, then remove the lock entry to accept it.")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(body)
    lock.setdefault("hashes", {})[key] = got
    return "new" if not want else "verified", body


def stage_fetch(spec, lock, force=False):
    print("== fetch ==")
    LICENSES.mkdir(exist_ok=True)

    # Every path the manifest actually wants, collected as we go. Pruning is driven by THIS,
    # not by the lock file's own contents: the lock is loaded from the previous run, so a
    # source that has just been dropped from the manifest is still in it and would look
    # "live". Retargeting CLOCK62 away from a family left that family's .ttf cached and
    # hash-pinned exactly this way.
    wanted = set()

    def want(dest):
        wanted.add(str(dest.relative_to(ROOT)).replace("\\", "/"))
        return dest

    families = sorted({f["family"] for f in spec["fonts"].values()
                       if f.get("source") != "lvgl-builtin"})
    for fam in families:
        url, lic = FONT_SRC[fam]
        state, _ = fetch_one(url, want(CACHE / "fonts" / f"{fam}.ttf"), lock, force)
        fetch_one(lic, want(LICENSES / f"{fam}-OFL.txt"), lock, force)
        print(f"  font {fam:12} {state}")

    for name, g in sorted(spec.get("gifs", {}).items()):
        url, lic, spdx = GIF_TEXTURE_SRC[g["source"]]
        state, _ = fetch_one(url, want(CACHE / "textures" / f"{g['source']}.jpg"), lock, force)
        if lic:
            fetch_one(lic, want(LICENSES / f"{g['source']}-{spdx}.txt"), lock, force)
        print(f"  gif  {name:12} texture {state}")

    for name, ico in sorted(spec.get("icos", {}).items()):
        tmpl, lic, spdx = ICO_SRC[ico["set"]]
        fetch_one(lic, want(LICENSES / f"{ico['set']}-{spdx}.txt"), lock, force)
        state, _ = fetch_one(tmpl.format(icon=ico["icon"]),
                             want(CACHE / "icos" / f"{ico['set']}-{ico['icon']}.svg"),
                             lock, force)
        print(f"  ico  {name:12} {state}")

    sets = {}
    for img in spec["images"].values():
        sets.setdefault(img["set"], []).append(img)
    for setname, imgs in sorted(sets.items()):
        tmpl, lic, spdx = ICON_SRC[setname]
        fetch_one(lic, want(LICENSES / f"{setname}-{spdx}.txt"), lock, force)
        states = {}
        for img in imgs:
            state, _ = fetch_one(tmpl.format(icon=img["icon"]),
                                 want(CACHE / "icons" / setname / f"{img['icon']}.svg"),
                                 lock, force)
            states[state] = states.get(state, 0) + 1
        print(f"  icons {setname:17} {len(imgs):2} files  {dict(states)}")

    # Drop cached files, licences and lock entries the manifest no longer refers to, so the
    # lock always describes exactly this build and no stale file can be picked up later.
    for key in sorted(set(lock.get("hashes", {})) - wanted):
        del lock["hashes"][key]
        print(f"  unpin {key}")
    # Prune ONLY the subdirectories this pipeline populates. It used to walk all of
    # assets/cache/, which deleted anything else parked there -- notably the stock-device
    # capture in assets/cache/stock/, which nothing in this manifest references and which was
    # therefore wiped on every --fetch, silently undoing a "capture once" step and sending the
    # next build back to prompting for a device address.
    live = wanted
    for sub in CACHE_OWNED:
        root = CACHE / sub
        if not root.exists():
            continue
        for f in sorted(root.rglob("*")):
            if not f.is_file():
                continue
            key = str(f.relative_to(ROOT)).replace("\\", "/")
            if key not in live:
                f.unlink()
                print(f"  prune {key}")
        for d in sorted((p for p in root.rglob("*") if p.is_dir()), reverse=True):
            if not any(d.iterdir()):
                d.rmdir()
    for f in sorted(LICENSES.iterdir()):
        if f.is_file() and str(f.relative_to(ROOT)).replace(chr(92), "/") not in wanted:
            f.unlink()
            print(f"  prune LICENSES/{f.name}")

    save_lock(lock)
    print(f"  lock: {len(lock.get('hashes', {}))} hashes -> {LOCK.relative_to(ROOT)}")


# -- instancing -----------------------------------------------------------------------

def instanced_ttf(fam, weight):
    """Pin a variable font to one weight, since the converter reads the default instance.

    Left un-instanced, Montserrat[wght] renders at wght=100 (Thin). That failure looks
    like a broken rasteriser rather than a missing argument, so it is done explicitly.
    """
    from fontTools import ttLib
    from fontTools.varLib import instancer

    out = BUILD / "fonts" / f"{fam}-{weight}.ttf"
    if out.exists():
        return out
    out.parent.mkdir(parents=True, exist_ok=True)
    font = ttLib.TTFont(CACHE / "fonts" / f"{fam}.ttf")
    axes = {a.axisTag: (a.minValue, a.defaultValue, a.maxValue)
            for a in font["fvar"].axes} if "fvar" in font else {}
    if "wght" not in axes:
        # A static single-weight file, not a variable font. Nothing to instance -- the weight
        # is whatever the file already is, so the manifest's weight is documentation here
        # rather than an instruction. Copied through so later stages take one code path.
        shutil.copyfile(CACHE / "fonts" / f"{fam}.ttf", out)
        return out
    lo, _, hi = axes["wght"]
    if not lo <= weight <= hi:
        sys.exit(f"FATAL {fam}: weight {weight} outside axis range {lo}..{hi}")
    # opsz (Inter) is left at its default: it is an optical-size axis, and the px size is
    # already chosen by --plan against the rendered line_height.
    instancer.instantiateVariableFont(font, {"wght": weight}, inplace=True, updateFontNames=False)
    font.save(out)
    return out


# -- plan -----------------------------------------------------------------------------

def render_face(ttf, px, bpp, ranges, kerning, out_c, symbol):
    """Render one face to an LVGL C source. Returns the converter's output.

    Uses scripts/lv_font_gen.py, this project's own converter, rather than lv_font_conv.
    Both call FreeType with the same flags, so the output is byte-identical - which
    scripts/lv_font_gen_verify.py checks face by face against the real thing. The point of
    the swap is that the pipeline needs one runtime instead of two; lv_font_conv is Node.
    """
    cmd = [sys.executable, str(ROOT / "scripts" / "lv_font_gen.py"),
           "--font", str(ttf), "--size", str(px), "--bpp", str(bpp),
           "--format", "lvgl", "--lv-font-name", symbol, "--no-compress",
           "-r", ranges, "-o", str(out_c)]
    if not kerning:
        # Faces specced without kerning must not gain it: kern data changes every
        # string's advance width, which moves centred and right-aligned text.
        cmd.append("--no-kerning")
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"FATAL lv_font_gen failed for {symbol} @ {px}px:\n{p.stdout}\n{p.stderr}")
    return p.stdout + p.stderr


def measured_metrics(c_path):
    """Read a rendered face's declared metrics AND where its glyphs actually put ink.

    The declared base_line is a rounded descent, which is not the same question as "does
    any glyph reach below the baseline". A digits-only face routinely reports a descent of
    1px while having no ink under the baseline at all, so judging clipping by the metric
    rejects perfectly good sizes. ink_top/ink_bottom answer the real question.

    Returns (line_height, base_line, ink_top, ink_bottom), the last two measured from the
    baseline: ink_top is how far the tallest glyph rises, ink_bottom how far the deepest
    one descends (0 if none does).
    """
    src = c_path.read_text(encoding="utf-8", errors="replace")
    lh = re.search(r"\.line_height\s*=\s*(\d+)", src)
    bl = re.search(r"\.base_line\s*=\s*(\d+)", src)
    if not lh or not bl:
        sys.exit(f"FATAL could not read metrics from {c_path}")

    boxes = re.findall(
        r"\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*-?\d+,"
        r"\s*\.ofs_y\s*=\s*(-?\d+)", src)
    if not boxes:
        sys.exit(f"FATAL could not read any glyph boxes from {c_path}; "
                 f"the emitted format changed and the clipping check would silently pass")
    # Blank glyphs (space) carry box_h 0 and would drag ink_top to 0, so skip them.
    ink = [(int(w), int(h), int(y)) for w, h, y in boxes if int(h) > 0]
    ink_top = max(y + h for _, h, y in ink)
    ink_bottom = min(y for _, _, y in ink)

    # Widest INK, for faces drawn as separate labels pinned at a fixed pitch. It is the ink
    # that decides whether they collide, NOT the advance: the labels have transparent
    # backgrounds, and for two glyphs at x and x+pitch the ink overlaps only when
    # x+ofs_x+box_w > (x+pitch)+ofs_x, i.e. when box_w > pitch -- ofs_x cancels out. Measuring
    # the advance instead (which includes both sidebearings) rejects fonts that would in fact
    # sit side by side perfectly well.
    max_ink_w = max((w for w, _, _ in ink), default=0)
    return int(lh.group(1)), int(bl.group(1)), ink_top, min(0, ink_bottom), max_ink_w


def plan_face(name, f, lock):
    """Pick the largest px size whose natural line_height still fits the spec's.

    px size and line_height are different numbers: line_height is the font's ascender
    minus descender scaled to px, so the ratio differs per family. It is also not
    surjective -- ascent and descent round separately, so as px increments the resulting
    line_height can jump straight past a value. Montserrat goes 22px -> 26 -> 23px -> 28,
    and never produces 27 at any size. Demanding exact equality therefore fails on faces
    that are perfectly usable.

    So: choose the largest px that does not OVERSHOOT the target, and let --render pin
    line_height/base_line to the spec afterwards. Padding a line box upward is safe;
    shrinking one below the glyphs' natural height would clip ascenders and descenders,
    which is why the search is biased downward rather than to the nearest value.
    """
    target = f["line_height"]
    ttf = instanced_ttf(f["family"], f["weight"])
    tmp = BUILD / "plan" / f"{name}.c"
    tmp.parent.mkdir(parents=True, exist_ok=True)

    # Binary search rather than a fixed scan window. A window is both slow and WRONG: px and
    # line_height are only loosely related, so how far px must exceed line_height varies per
    # family. A target+5 ceiling silently capped TIME76 and LEAD31 at the ceiling itself,
    # rendering them smaller than they should be with no error to show it.
    #
    # Every constraint below is monotone in px -- ink height, descent and advance all grow
    # with size -- so once a size fails it fails for all larger sizes, which is exactly what
    # makes a binary search valid here.
    tried = {}

    def measure(px):
        if px not in tried:
            render_face(ttf, px, f["bpp"], f["ranges"], f["kerning"], tmp, f["symbol"])
            tried[px] = measured_metrics(tmp)
        return tried[px]

    # line_height is the hard constraint -- panel heights and multi-line spacing are laid
    # out against it, so --render always pins it to the spec exactly. base_line is allowed
    # to float UP by a pixel or two when the glyphs need it.
    #
    # That asymmetry is deliberate. Round digits carry typographic overshoot: 0, 6 and 8
    # put real ink a pixel below the baseline at large sizes. Treating the spec's base_line
    # as inviolable makes the search reject every large size to avoid that one pixel --
    # CLOCK62 dropped from 67px to 50px, shrinking the clock by a fifth to protect a
    # baseline that a 1px shift would have satisfied. Losing a pixel of position is a far
    # smaller error than losing a fifth of the glyph size.
    # Some faces are drawn as separate labels pinned to fixed x positions rather than as one
    # string, so what limits their size is the DIGIT ADVANCE, not the line height. CLOCK62's
    # digits sit 35px apart; a face whose advance exceeds that overlaps its neighbour. Without
    # this, swapping to a wider family still "passes" --plan and silently breaks the screen.
    max_ink_limit = f.get("max_ink_width")

    def fitted(m):
        _lh, _bl, ink_top, ink_bottom, _adv = m
        base = max(f["base_line"], -ink_bottom)      # deepen the box only as much as needed
        return base, ink_top + base <= target        # total ink must still fit line_height

    def ok(px):
        m = measure(px)
        return fitted(m)[1] and (max_ink_limit is None or m[4] <= max_ink_limit)

    pinned = f.get("size") or 0
    if pinned:
        # An explicit size in the manifest wins over the search. The largest size that FITS is
        # not always the size that LOOKS right -- the fit test only knows about the line box,
        # not about how the face sits against everything around it. Constraints are still
        # checked and reported, so a pinned size cannot quietly overflow its slot.
        px = pinned
        m = measure(px)
        if not fitted(m)[1]:
            sys.exit(f"FATAL {name}: pinned size {px}px does not fit line_height {target} "
                     f"(ink +{m[2]}/{m[3]}). Lower the pinned size or fix the spec.")
        if max_ink_limit is not None and m[4] > max_ink_limit:
            sys.exit(f"FATAL {name}: pinned size {px}px gives a {m[4]:.1f}px advance, wider "
                     f"than the {max_ink_limit}px slot -- digits would overlap.")
    else:
        lo = 6
        if not ok(lo):
            m = measure(lo)
            extra = (f" Widest ink {m[4]:.0f}px vs the {max_ink_limit}px slot."
                     if max_ink_limit is not None else "")
            sys.exit(f"FATAL {name}: even {lo}px does not fit line_height {target} "
                     f"(ink +{m[2]}/{m[3]}).{extra} The spec is wrong.")
        hi = target * 3                      # far above any plausible answer
        if ok(hi):
            sys.exit(f"FATAL {name}: {hi}px still fits, so the search has no upper bound. "
                     f"Check the constraints -- this should be impossible.")
        while hi - lo > 1:                   # invariant: ok(lo), not ok(hi)
            mid = (lo + hi) // 2
            if ok(mid):
                lo = mid
            else:
                hi = mid
        px = lo
    lh, bl, ink_top, ink_bottom, adv = tried[px]
    base = fitted(tried[px])[0]

    lock.setdefault("sizes", {})[name] = px
    lock.setdefault("baselines", {})[name] = base
    drift = "" if base == f["base_line"] else f"  base_line {f['base_line']}->{base}"
    slack = target - base - ink_top
    pitch = f"  ink_w {adv:.0f}/{max_ink_limit}" if max_ink_limit is not None else ""
    print(f"  {name:10} {px:3}px  ink +{ink_top}/{ink_bottom}  lh {lh}->{target} "
          f"base {bl}->{base}  slack {slack}{pitch}{drift}")
    return not drift


def stage_plan(spec, lock):
    print("== plan ==")
    clean = True
    for name, f in spec["fonts"].items():
        if f.get("source") == "lvgl-builtin":
            print(f"  {name:10} built-in {f['builtin']} (skipped)")
            continue
        clean &= plan_face(name, f, lock)
    save_lock(lock)
    if not clean:
        print("\n  NOTE the faces showing base_line X->Y need a deeper descent than the spec, so\n"
              "  their baseline sits 1-2px higher inside an unchanged line box. Nothing is\n"
              "  clipped and panel heights are exact; text on those screens is a touch high.")
    return clean


# -- render: fonts --------------------------------------------------------------------

def pin_metrics(c_path, line_height, base_line):
    """Force a rendered face's line_height/base_line to the values the layout expects.

    The converter has no flag for this, and the two numbers cannot both be hit by choosing a
    px size (see plan_face). Rewriting them here is safe because they are layout metadata,
    not glyph data: LVGL reads line_height to advance between lines and base_line to place
    the baseline within that box. --plan has already guaranteed the ink fits.
    """
    src = c_path.read_text(encoding="utf-8")
    src, n1 = re.subn(r"(\.line_height\s*=\s*)\d+", rf"\g<1>{line_height}", src)
    src, n2 = re.subn(r"(\.base_line\s*=\s*)\d+", rf"\g<1>{base_line}", src)
    if n1 != 1 or n2 != 1:
        sys.exit(f"FATAL {c_path.name}: expected one line_height and one base_line, "
                 f"found {n1} and {n2}; refusing to guess which to pin")
    c_path.write_text(src, encoding="utf-8", newline="\n")


def render_fonts(spec, lock):
    out_dir = BUILD / "fonts"
    out_dir.mkdir(parents=True, exist_ok=True)
    results = {}
    for name, f in spec["fonts"].items():
        if f.get("source") == "lvgl-builtin":
            continue
        px = lock.get("sizes", {}).get(name)
        base = lock.get("baselines", {}).get(name)
        if px is None or base is None:
            sys.exit(f"FATAL {name} has no planned size; run --plan first")
        ttf = instanced_ttf(f["family"], f["weight"])
        c = out_dir / f"{name}.c"
        render_face(ttf, px, f["bpp"], f["ranges"], f["kerning"], c, f["symbol"])
        pin_metrics(c, f["line_height"], base)
        size = c.stat().st_size
        results[name] = c
        print(f"  {name:10} {px:3}px  {size // 1024:4} KB  {f['symbol']}")
    return results


# -- render: images -------------------------------------------------------------------

def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def encode_image(png_path, cf, w, h, trim=False):
    """Turn a rasterised PNG into the byte layout LVGL expects for this colour format.

    Returns (bytes, description). Formats, all little-endian 16-bit colour:

      TRUE_COLOR         2 B/px, RGB565.
      TRUE_COLOR_ALPHA   3 B/px, RGB565 then one alpha byte.
      INDEXED_4BIT       a 16-entry palette followed by 2 pixels per byte, high nibble first.

    The palette is lv_color32_t, whose byte order is B,G,R,A -- NOT RGB565 and NOT RGBA.
    Writing it as RGB565 previously produced pink clouds on the weather screen: the image
    decoded without error, so nothing failed except the appearance.
    """
    from PIL import Image

    def pixels(im):
        # getdata() is deprecated from Pillow 14; get_flattened_data() is its replacement but
        # does not exist on older versions, so prefer the new name and fall back.
        return list(im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata())

    img = Image.open(png_path).convert("RGBA")

    if trim:
        # Crop to the ink and scale it up to fill the slot. Some sources pad their glyphs
        # so heavily that rendering at the target size leaves the mark occupying under a
        # fifth of its box -- on the panel that reads as a stray dot rather than an icon.
        # Aspect ratio is preserved, so this cannot squash the artwork.
        box = img.split()[3].getbbox()
        if box:
            ink = img.crop(box)
            scale = min(w / ink.width, h / ink.height)
            img = ink.resize((max(1, round(ink.width * scale)),
                              max(1, round(ink.height * scale))), Image.Resampling.LANCZOS)

    if img.size != (w, h):
        # Centre rather than stretch: a squashed icon is a subtler wrong than a padded one.
        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        canvas.paste(img, ((w - img.width) // 2, (h - img.height) // 2))
        img = canvas

    if cf == "LV_IMG_CF_TRUE_COLOR_ALPHA":
        out = bytearray()
        for px in pixels(img):
            c = rgb565(*px[:3])
            out += bytes((c & 0xFF, c >> 8, px[3]))
        return bytes(out), "RGB565+A"

    if cf == "LV_IMG_CF_TRUE_COLOR":
        # No alpha channel to carry, so composite onto black -- the panels these sit on are
        # black, and leaving premultiplied edges against white would fringe the artwork.
        flat = Image.new("RGB", img.size, (0, 0, 0))
        flat.paste(img, mask=img.split()[3])
        out = bytearray()
        for px in pixels(flat):
            c = rgb565(*px)
            out += bytes((c & 0xFF, c >> 8))
        return bytes(out), "RGB565"

    if cf == "LV_IMG_CF_INDEXED_4BIT":
        # Quantise to 15 colours and reserve index 0 for fully transparent, so the icon can
        # sit on any background. LVGL indexed images carry alpha only via the palette.
        alpha = img.split()[3]
        pal_img = img.convert("RGB").quantize(colors=15, method=Image.Quantize.MEDIANCUT)
        pal = pal_img.getpalette()[: 15 * 3]
        idx = pixels(pal_img)
        a = pixels(alpha)

        out = bytearray()
        for i in range(15):
            r, g, b = pal[i * 3: i * 3 + 3]
            out += bytes((b, g, r, 0xFF))          # lv_color32_t: B,G,R,A
        out += bytes((0, 0, 0, 0))                 # index 15 = transparent

        rows = bytearray()
        for y in range(h):
            row = bytearray()
            for x in range(0, w, 2):
                n = []
                for dx in (0, 1):
                    p = y * w + x + dx
                    n.append(15 if (x + dx < w and a[p] < 128) else idx[p] if x + dx < w else 15)
                row.append(((n[0] & 0xF) << 4) | (n[1] & 0xF))
            rows += row
        return bytes(out + rows), "16-colour palette"

    sys.exit(f"FATAL unsupported colour format {cf}")


# -- SVG -> PNG -------------------------------------------------------------------------
# resvg, invoked as the CLI binary that `resvg-cli` installs into the venv alongside this
# interpreter. resvg rather than a pure-Python rasteriser because these icon sets rely on
# gradients and clip paths - SOL and ETH are gradient-filled, DOGE is 38 stacked paths - and
# rasterisers that silently drop those produce an icon that looks plausible and is wrong,
# which is the hardest kind of error to notice at 32x32.
#
# This used to shell out to Node (scripts/svg2png.js, @resvg/resvg-js). Same engine, but it
# made Node a second toolchain that nothing else here needed, and it was never declared, so
# a fresh clone died on MODULE_NOT_FOUND. The CLI wheel is the same renderer reached through
# a dependency uv already manages. Verified when it was swapped: all 33 icons render
# PIXEL-IDENTICAL to the Node output.


def _resvg_bin() -> str:
    """The resvg executable from this interpreter's environment.

    Looked up next to sys.executable rather than on PATH, so the version pinned in
    pyproject.toml is the one that runs even if another is installed system-wide.
    """
    d = Path(sys.executable).parent
    for c in (d / "resvg.exe", d / "resvg", d.parent / "bin" / "resvg"):
        if c.is_file():
            return str(c)
    sys.exit("FATAL resvg not found next to %s - run `uv sync`" % sys.executable)


def rasterise(jobs) -> None:
    """Render each {src, out, w, h} job to a PNG.

    FIT TO WIDTH, THEN FALL BACK TO HEIGHT. Every source viewBox here is square but the
    firmware has one non-square slot (the temperature gauge, 21x40). Fitting to width gives
    21x21 there, so when the result is not the slot size it is re-rendered fitted to HEIGHT
    and the encoder's `trim` handles the rest. Doing it the other way round, or forcing both
    dimensions, silently changes the aspect ratio.
    """
    from PIL import Image
    exe = _resvg_bin()
    for job in jobs:
        src, out = job["src"], Path(job["out"])
        w, h = int(job["w"]), int(job["h"])
        r = subprocess.run([exe, "-w", str(w), src, str(out)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not out.is_file():
            sys.exit(f"FATAL resvg failed on {src}:\n{r.stdout}\n{r.stderr}")
        if Image.open(out).size != (w, h):
            r = subprocess.run([exe, "-h", str(h), src, str(out)],
                               capture_output=True, text=True)
            if r.returncode != 0 or not out.is_file():
                sys.exit(f"FATAL resvg failed on {src}:\n{r.stdout}\n{r.stderr}")
    print(f"  rasterised {len(jobs)} svg -> png")


def render_images(spec, lock):
    png_dir = BUILD / "png"
    png_dir.mkdir(parents=True, exist_ok=True)

    jobs, meta = [], {}
    for name, im in spec["images"].items():
        src = CACHE / "icons" / im["set"] / f"{im['icon']}.svg"
        if not src.exists():
            sys.exit(f"FATAL {name}: {src} missing; run --fetch first")
        out = png_dir / f"{name}.png"
        jobs.append({"src": str(src), "out": str(out), "w": im["width"], "h": im["height"]})
        meta[name] = out

    rasterise(jobs)

    blobs = {}
    for name, im in spec["images"].items():
        data, desc = encode_image(meta[name], im["cf"], im["width"], im["height"],
                                  trim=im.get("trim", False))
        expect = im["data_size"]
        flag = "" if len(data) == expect else f"  SIZE {len(data)} != spec {expect}"
        blobs[name] = data
        print(f"  {name:16} {im['width']:3}x{im['height']:<3} {desc:18} {len(data):6} B{flag}")
    return blobs


# -- render: animated GIFs ------------------------------------------------------------

def render_gifs(spec):
    """Render the rotating-globe GIF by orthographic projection of a flat Earth texture.

    Projecting a texture is deliberate. Two other approaches were tried and both failed
    visibly on hardware:
      * downscaling NASA's Blue Marble ANIMATION -- it is cloud-dominated, so thresholding
        land against ocean produced cloud blobs with no recognisable continents;
      * drawing Natural Earth coastline VECTORS -- clipping each ring to the visible
        hemisphere by dropping off-globe points tears the polygons apart, so landmasses
        vanished at some rotations.

    Three output details are not cosmetic and must not be "tidied":

      * BLACK MUST BE PALETTE INDEX 0. A GIF decoder paints background and disposed regions
        with entry 0, and this firmware also treats black as transparent. An adaptive palette
        put cyan at index 0 once and the panel showed a flashing cyan square behind the globe.

      * optimize=False, disposal=0. Pillow's optimiser replaces changed pixels with a
        transparent fill, which this decoder does not composite correctly -- that showed as
        single pixels blinking on and off across Africa and the ocean.

    OPEN, NOT FIXED -- frames are still not whole. optimize=False stops the transparent-fill
    optimisation but NOT the frame cropping: _write_multiple_frames diffs each frame against
    its predecessor unconditionally and emits only the bounding box of the change. Inspecting
    the image descriptors shows ours are (2,2,75,77) after frame 0, where every stock GIF is
    (0,0,80,80) on every frame. So the outer ~2px ring is written once by frame 0 and then
    never rewritten. The comment here used to assert frames WERE whole; that was wrong.

    Consequence to check before blaming anything else: the residual "roughly 70 isolated
    pixels per frame" flicker, previously attributed to the hard land/ocean threshold, may be
    partly or wholly this instead -- stale pixels in regions no later frame covers. The
    threshold explanation was never verified against the cropping. Anyone fixing this should
    force full frames (e.g. make consecutive frames differ at the corners so the diff bbox is
    always the full canvas, or write the GIF without Pillow) and re-measure the flicker before
    reaching for an anti-aliased shade ramp.
    """
    from PIL import Image

    out_dir = BUILD / "gifs"
    out_dir.mkdir(parents=True, exist_ok=True)
    made = []
    for name, g in sorted(spec.get("gifs", {}).items()):
        tex_path = CACHE / "textures" / f"{g['source']}.jpg"
        if not tex_path.exists():
            sys.exit(f"FATAL {name}: {tex_path} missing; run --fetch first")
        tex = Image.open(tex_path).convert("RGB").resize((1024, 512), Image.LANCZOS)
        TW, TH = tex.size
        tp = tex.load()

        W, H = g["width"], g["height"]
        ss = g["supersample"]
        SZ, R, C = W * ss, g["radius"] * ss, (W // 2) * ss
        pal_cols = [tuple(c) for c in g["palette"]]
        transparent, land, ocean = pal_cols[0], pal_cols[1], pal_cols[2]

        pimg = Image.new("P", (1, 1))
        flat = [c for rgb in pal_cols for c in rgb]
        pimg.putpalette(flat + [0] * (768 - len(flat)))

        def is_land(r, gg, b):
            # Blue Marble oceans are blue-dominant; land is green/brown.
            return not (b > r + 10 and b > gg + 2)

        frames = []
        for i in range(g["frames"]):
            lon0 = i * 360.0 / g["frames"]
            im = Image.new("RGB", (SZ, SZ), transparent)
            px = im.load()
            for y in range(SZ):
                dy = (y - C) / R
                if abs(dy) > 1:
                    continue
                for x in range(SZ):
                    dx = (x - C) / R
                    rho2 = dx * dx + dy * dy
                    if rho2 > 1.0:
                        continue
                    rho = math.sqrt(rho2)
                    if rho < 1e-9:
                        lat, lon = 0.0, math.radians(lon0)
                    else:
                        # Inverse orthographic at lat0 = 0. atan2 spans the full 360; the
                        # asin(dx/cos lat) shortcut folds and leaves a seam at +/-90 degrees.
                        cc = math.asin(min(1.0, rho))
                        lat = math.asin(max(-1, min(1, (-dy) * math.sin(cc) / rho)))
                        lon = math.radians(lon0) + math.atan2(dx * math.sin(cc), rho * math.cos(cc))
                    tx = int(((math.degrees(lon) + 180) % 360) / 360 * TW) % TW
                    ty = max(0, min(TH - 1, int((90 - math.degrees(lat)) / 180 * TH)))
                    px[x, y] = land if is_land(*tp[tx, ty]) else ocean
            frames.append(im.resize((W, H), Image.LANCZOS)
                            .quantize(palette=pimg, dither=Image.Dither.NONE))

        # The global colour table must be passed explicitly. Without a `palette=` argument
        # Pillow sets include_color_table on EVERY frame (GifImagePlugin _write_multiple_frames),
        # stamping a 768-byte table onto each one: 240 frames x 768 = 184 KB, 59% of the file,
        # to re-describe the same three colours. Passing it drops the per-frame tables entirely
        # and shrinks the palette to one legal size (4 entries, 12 bytes), which also lets the
        # LZW coder use 2-bit codes instead of 8-bit. No stock GIF carries a local table.
        # Sizing note: a GIF colour table must hold a power of two >= 2 entries.
        n_slots = max(2, 1 << (len(pal_cols) - 1).bit_length())
        gct = bytes(c for rgb in pal_cols for c in rgb) + bytes(3 * (n_slots - len(pal_cols)))

        p = out_dir / g["out"]
        frames[0].save(p, save_all=True, append_images=frames[1:], loop=0,
                       duration=g["duration_ms"], optimize=False, disposal=0,
                       palette=gct)
        fps = 1000.0 / g["duration_ms"]
        print(f"  {name:10} {g['frames']}f @{fps:.0f}fps  {p.stat().st_size:7,} B  -> {p.relative_to(ROOT)}")
        print(f"             upload to {g['target_dir']}/{g['out']} (device DATA, not a header)")
        made.append(p)
    return made


def render_icos(spec):
    """Rasterise the favicon from an emoji SVG.

    Device DATA like the GIFs, not a compiled header -- it is served straight off LittleFS by
    the static file handler, and nothing in src/ references it by name.

    Written as a SINGLE 32x32 image. A modern .ico would normally carry 16/32/48 together;
    one size keeps the file small and the comparison simple.
    """
    out_dir = BUILD / "icos"
    out_dir.mkdir(parents=True, exist_ok=True)
    made = []
    jobs, targets = [], []
    for name, ico in sorted(spec.get("icos", {}).items()):
        svg = CACHE / "icos" / f"{ico['set']}-{ico['icon']}.svg"
        if not svg.exists():
            sys.exit(f"FATAL {name}: {svg} missing; run --fetch first")
        png = out_dir / f"{name}.png"
        jobs.append({"src": str(svg), "out": str(png), "w": ico["size"], "h": ico["size"]})
        targets.append((name, ico, png))

    if not jobs:
        return made
    rasterise(jobs)

    from PIL import Image
    for name, ico, png in targets:
        im = Image.open(png).convert("RGBA")
        dest = out_dir / ico["out"]
        im.save(dest, format="ICO", sizes=[(ico["size"], ico["size"])])
        print(f"  {name:10} {ico['size']}x{ico['size']}  {dest.stat().st_size:7,} B  "
              f"-> {dest.relative_to(ROOT)}")
        print(f"             upload to {ico['target_dir']}{ico['out']} (device DATA, not a header)")
        made.append(dest)
    return made


def stage_render(spec, lock):
    print("== render: fonts ==")
    fonts = render_fonts(spec, lock)
    print("== render: images ==")
    images = render_images(spec, lock)
    if spec.get("gifs"):
        print("== render: gifs ==")
        render_gifs(spec)
    if spec.get("icos"):
        print("== render: icos ==")
        render_icos(spec)
    return fonts, images


# -- emit -----------------------------------------------------------------------------

BANNER = """/* GENERATED FILE -- do not edit by hand.
 *
 * Produced by scripts/build_assets.py from the spec in scripts/assets.toml.
 * Upstream sources and their exact versions are pinned in scripts/assets.lock.toml;
 * the licences they ship under are in LICENSES/ and summarised in THIRD_PARTY.md.
 *
 * Regenerate with:  python scripts/build_assets.py --all
 */"""

# The converter names these the same way in every file it writes, so twelve faces in one
# translation unit would be twelve redefinitions. Each is prefixed with its face name.
FONT_INTERNALS = ["cache", "cmaps", "font_dsc", "glyph_bitmap", "glyph_dsc"]
FONT_INTERNAL_PATTERNS = [r"unicode_list_\d+", r"glyph_id_ofs_list_\d+", r"kern_\w+"]


def font_body(name, c_path):
    """Strip one rendered face down to something that can share a header with the others.

    Three things have to go:
      * the banner, which records the full command line including absolute local paths --
        those must never reach a tracked file;
      * the lvgl include dance, since the header includes <lvgl.h> once at the top;
      * the `#ifndef FONT_X / #define FONT_X 1 / #if FONT_X` guard, which would collide
        with the font's own symbol name.
    And every internal symbol is prefixed, or the faces redefine each other.
    """
    src = c_path.read_text(encoding="utf-8")

    src = re.sub(r"^/\*{10,}.*?\*{10,}/\s*", "", src, flags=re.S)
    src = re.sub(r"#ifdef LV_LVGL_H_INCLUDE_SIMPLE.*?#endif\s*", "", src, flags=re.S)
    src = re.sub(rf"#ifndef {name}\s*#define {name} 1\s*#endif\s*", "", src)
    src = re.sub(rf"#if {name}\s*", "", src, count=1)
    src = re.sub(rf"#endif\s*/\*#if {name}\*/\s*$", "", src)

    # The lookbehind matters: most of these names are ALSO struct field names, so a bare \b
    # match rewrites the designator in `.glyph_bitmap = glyph_bitmap` too, and the struct
    # then names a member LVGL does not declare. Only rename uses not preceded by a dot.
    for sym in FONT_INTERNALS:
        src = re.sub(rf"(?<![.\w]){sym}\b", f"{name}_{sym}", src)
    for pat in FONT_INTERNAL_PATTERNS:
        src = re.sub(rf"(?<![.\w])({pat})\b", rf"{name}_\1", src)

    if "D:" in src or "/source/" in src:
        sys.exit(f"FATAL {c_path.name}: a local absolute path survived stripping; "
                 f"refusing to write it into a tracked header")
    return src.strip()


def emit_fonts(spec, lock):
    parts = [BANNER, "", "#pragma once", "#include <lvgl.h>", ""]
    aliases = [(n, f) for n, f in spec["fonts"].items() if f.get("source") == "lvgl-builtin"]
    if aliases:
        parts += [
            "/* Faces LVGL already ships, enabled in lv_conf.h. Aliased rather than",
            " * duplicated: the tables would be identical, so generating them again would",
            " * cost flash to reproduce data the library already contains. */",
        ]
        for n, f in aliases:
            parts.append(f"#define {f['symbol']:<16} {f['builtin']}")
        parts.append("")

    for name, f in spec["fonts"].items():
        if f.get("source") == "lvgl-builtin":
            continue
        px = lock["sizes"][name]
        parts += [
            f"/* {f['symbol']} -- {f['family']} {f['weight']} at {px}px, "
            f"{f['bpp']}bpp, kerning {'on' if f['kerning'] else 'off'} */",
            font_body(f["symbol"], BUILD / "fonts" / f"{name}.c"),
            "",
        ]
    out = ROOT / "src" / "LvFontsGen.h"
    out.write_text("\n".join(parts) + "\n", encoding="utf-8", newline="\n")
    return out


def c_array(data, indent="    ", per_line=16):
    rows = []
    for i in range(0, len(data), per_line):
        rows.append(indent + ", ".join(f"0x{b:02x}" for b in data[i:i + per_line]) + ",")
    return "\n".join(rows)


def emit_images(spec, blobs):
    o = spec["order"]
    parts = [BANNER, "", "#pragma once", "#include <lvgl.h>", "#include <string.h>", ""]

    for name, im in spec["images"].items():
        data = blobs[name]
        parts += [
            f"static const uint8_t {name}_data[] = {{",
            c_array(data),
            "};",
            "",
            f"extern const lv_img_dsc_t {im['symbol']};",
            f"const lv_img_dsc_t {im['symbol']} = {{",
            # Field order follows lv_img_header_t's declaration (cf, always_zero, reserved,
            # w, h). C++ rejects out-of-order designated initialisers, so this is not a
            # style choice -- putting always_zero first fails to compile.
            "    .header = {",
            f"        .cf = {im['cf']},",
            "        .always_zero = 0,",
            "        .reserved = 0,",
            f"        .w = {im['width']},",
            f"        .h = {im['height']},",
            "    },",
            f"    .data_size = {len(data)},",
            f"    .data = {name}_data,",
            "};",
            "",
        ]

    parts += ["/* Indexed by launcher order -- the sequence the apps are registered in. */",
              "static const lv_img_dsc_t * const LV_APP_ICONS[] = {"]
    parts += [f"    &{spec['images'][n]['symbol']}," for n in o["app_icons"]]
    parts += ["};", f"static constexpr int LV_APP_ICON_COUNT = {len(o['app_icons'])};", ""]

    parts += ["/* Weather condition icons. The code table and the descriptor table are scanned",
              "   in lock-step, so they must stay in the same order. */",
              f"static constexpr int LV_WX_ICON_COUNT = {len(o['wx_codes'])};",
              "static const int LV_WX_ICON_CODES[] = {"
              + "".join(f"{c}, " for c in o["wx_codes"]) + "};",
              "static const lv_img_dsc_t * const LV_WX_ICONS[] = {"]
    parts += [f"    &IMG_WX_ICON_{c:02d}," for c in o["wx_codes"]]
    parts += ["};", ""]

    parts += ["/* -- Coin symbol icons -----------------------------------------------------",
              "   Two lookups share these twelve descriptors in this order, so one table serves",
              "   both: by trading-symbol PREFIX, and by coin id matched EXACTLY. A coin that is",
              "   not listed gets no icon, which is intended. */",
              f"static constexpr int LV_COIN_ICON_COUNT = {len(o['coin_order'])};",
              'static const char * const LV_COIN_ICON_SYMS[] = {'
              + "".join(f'"{s}", ' for s in o["coin_order"]) + "};",
              'static const char * const LV_COIN_ICON_IDS[] = {'
              + "".join(f'"{s}", ' for s in o["coin_ids"]) + "};",
              "static const lv_img_dsc_t * const LV_COIN_ICONS[] = {"]
    parts += [f"    &IMG_COIN_{s}," for s in o["coin_order"]]
    parts += ["};", ""]

    parts += ['/* Leading-symbol match, e.g. "BTCUSDT" / "BTCEUR" -> BTC. */',
              "static inline const lv_img_dsc_t* lvCoinIconForSymbol(const char* symbol) {",
              "    if (!symbol) return nullptr;",
              "    for (int i = 0; i < LV_COIN_ICON_COUNT; i++) {",
              "        const char* s = LV_COIN_ICON_SYMS[i];",
              "        if (strncmp(symbol, s, strlen(s)) == 0) return LV_COIN_ICONS[i];",
              "    }",
              "    return nullptr;",
              "}",
              "",
              '/* EXACT id match -- a prefix match would send "bitcoin-cash" to the BTC icon. */',
              "static inline const lv_img_dsc_t* lvCoinIconForId(const char* id) {",
              "    if (!id) return nullptr;",
              "    for (int i = 0; i < LV_COIN_ICON_COUNT; i++)",
              "        if (strcmp(id, LV_COIN_ICON_IDS[i]) == 0) return LV_COIN_ICONS[i];",
              "    return nullptr;",
              "}",
              ""]

    out = ROOT / "src" / "LvImagesGen.h"
    out.write_text("\n".join(parts) + "\n", encoding="utf-8", newline="\n")
    return out


def emit_third_party(spec):
    """Write the attribution file. MIT obliges it; doing it by hand invites drift."""
    by_licence = {}
    for f in spec["fonts"].values():
        src = "LVGL built-in Montserrat" if f.get("source") == "lvgl-builtin" else f["family"]
        by_licence.setdefault((src, f["licence"]), set()).add(f["symbol"])
    for im in spec["images"].values():
        by_licence.setdefault((im["set"], im["licence"]), set()).add(im["symbol"])
    # Generated GIFs are device DATA rather than compiled-in assets, but they are derived
    # from third-party source imagery and that still needs crediting.
    for name, g in spec.get("gifs", {}).items():
        by_licence.setdefault((g["source"], g["licence"]), set()).add(g["out"])
    for name, ico in spec.get("icos", {}).items():
        by_licence.setdefault((ico["set"], ico["licence"]), set()).add(ico["out"])

    lines = ["# Third-party assets", "",
             "The fonts and icons this firmware draws with come from the freely licensed",
             "sources below. Full licence texts are in `LICENSES/`; exact upstream versions",
             "are pinned by SHA-256 in `scripts/assets.lock.toml`.", "",
             "| Source | Licence | Used for |", "|---|---|---|"]
    for (src, lic), syms in sorted(by_licence.items()):
        s = ", ".join(sorted(syms))
        if len(s) > 90:
            s = f"{len(syms)} assets"
        lines.append(f"| {src} | {lic} | {s} |")
    lines += ["",
              "Icon artwork depicts third-party logos and trademarks. Those marks remain the",
              "property of their respective owners; the licences above cover the artwork's",
              "copyright, not any trademark rights in the marks themselves.", ""]
    out = ROOT / "THIRD_PARTY.md"
    out.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return out


def stage_emit(spec, lock, blobs):
    print("== emit ==")
    for out in (emit_fonts(spec, lock), emit_images(spec, blobs), emit_third_party(spec)):
        print(f"  {out.relative_to(ROOT)}  {out.stat().st_size // 1024} KB")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fetch", action="store_true", help="download upstream sources")
    ap.add_argument("--plan", action="store_true", help="resolve px sizes from line_height")
    ap.add_argument("--render", action="store_true", help="convert fonts / rasterise SVGs")
    ap.add_argument("--emit", action="store_true", help="assemble the generated headers")
    ap.add_argument("--all", action="store_true", help="all four stages in order")
    ap.add_argument("--force", action="store_true", help="re-download even if cached")
    a = ap.parse_args()
    if not any((a.fetch, a.plan, a.render, a.emit, a.all)):
        ap.error("pick at least one stage (or --all)")

    spec, lock = load_spec(), load_lock()
    if a.fetch or a.all:
        stage_fetch(spec, lock, a.force)
    if a.plan or a.all:
        stage_plan(spec, lock)
    blobs = None
    if a.render or a.all:
        _fonts, blobs = stage_render(spec, lock)
    if a.emit or a.all:
        if blobs is None:
            # --emit alone still needs the encoded pixel data; re-encoding from the cached
            # PNGs is cheap and keeps the stages independently runnable.
            blobs = render_images(spec, lock)
        stage_emit(spec, lock, blobs)


if __name__ == "__main__":
    main()
