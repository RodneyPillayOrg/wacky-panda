#!/usr/bin/env python3
"""Back up, restore and verify a device's LittleFS filesystem over HTTP.

No serial connection is needed for any of it -- everything goes through the device's own
web API, so this works on a sealed unit:

  GET  /filelist?dir=<path>   -- list a directory (an HTML table of <a href> links)
  GET  /<any path>            -- download any file
  POST /doUpload?dir=<path>   -- upload a file to a directory (multipart)
  GET  /space.json            -- filesystem usage
  GET  /set?reboot=1          -- reboot

Use it to capture a known-good filesystem before flashing and to populate a fresh device
afterwards. 'verify' re-downloads everything and compares SHA-256 against the manifest, so
a restore is verified rather than assumed.

WHAT THIS CAN AND CANNOT CAPTURE. It produces every FILE, not a block-level image of the
partition -- no LittleFS metadata, no free-space content, no deleted-file remnants -- so
the output cannot be flashed back as an image. A raw partition dump needs serial.

Directories are enumerated live from /filelist. Root is the exception: the firmware serves
files from / but does NOT list it, so root files can only be fetched by name and
ROOT_FILES below is necessarily hardcoded. A future firmware adding a root file would be
missed silently unless it is added there.

Pages the firmware SERVES but does not store as files (currently /update, the OTA form)
are captured too, into _firmware_pages/ under a separate manifest key. They are reference
material for a build step -- restore never uploads them, because writing one to LittleFS
would create a file shadowing the route it came from.

Bytes are transferred raw. HTML files are stored gzip-compressed and served with
Content-Encoding: gzip, so automatic decompression is disabled throughout -- a backup holds
the same gzip bytes the filesystem does, and restoring one is byte-exact.

Because of that, anything whose content is actually gzip is saved with a .gz SUFFIX on
disk: /index.html becomes index.html.gz locally. The suffix is local only -- the manifest
key stays the device path and restore uploads it under the device name -- so a packer
building a filesystem image should strip .gz to recover the on-device filename.

Usage:
  python scripts/device_fs.py backup  --host <ip>
  python scripts/device_fs.py backup  --host <ip> --output ./my-backup
  python scripts/device_fs.py restore --host <ip> --input ./backup
  python scripts/device_fs.py restore --host <ip> --input ./backup --include-html
  python scripts/device_fs.py verify  --host <ip> --input ./backup

Run it with uv, which installs what it needs:  uv run scripts/device_fs.py ...
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    # Force UTF-8 on Windows so Unicode symbols print correctly
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

try:
    import requests
    from requests import Session
except ImportError:
    sys.exit(
        "The 'requests' library is required.\n"
        "Run this with uv:  uv run scripts/device_fs.py ...\n"
        "Or populate the venv first:  uv sync"
    )

# -- Known LittleFS paths ------------------------------------------------------

# Root-level files. Root is not enumerable, so unlike LIST_DIRS this list is the only way
# these are found -- a root file not named here is missed silently.
#
# The four HTML files are stored gzip-compressed and are backed up as raw gzip bytes (and so
# land on disk as .html.gz). favicon.ico is NOT compressed and is saved as-is.
#
# --include-html gates this WHOLE list, favicon.ico included, so a default restore leaves
# the favicon off too. The default exists because these normally arrive with the firmware
# rather than from a backup.
ROOT_FILES: List[str] = [
    "/index.html",
    "/ii.html",
    "/portal.html",
    "/captive.html",
    "/favicon.ico",
]

# /.sys/ config files written by the firmware at runtime.
#
# THIS LIST IS A FALLBACK ONLY. /.sys is enumerated from /filelist like any other directory
# (see LIST_DIRS), which is the authoritative source; this list is merged in afterwards so a
# device whose firmware declines to list /.sys still yields its known config files.
#
# Relying on the list ALONE was a real bug: it silently missed every /.sys/cg_chart_*.bin
# chart cache, capturing 15 of 27 files on a device while reporting success. Anything not
# named here was invisible, and a missing file looked identical to a file that simply did
# not exist yet. Do not reintroduce that by dropping /.sys from LIST_DIRS.
SYS_FILES: List[str] = [
    "/.sys/app.json",
    "/.sys/config.json",        # WiFi credentials -- see NEVER_TRANSFER, never captured
    "/.sys/coin2.json",         # Binance symbols
    "/.sys/cgk.json",           # CoinGecko coin IDs
    "/.sys/cgkl.json",          # CoinGecko coin list / logo URLs
    "/.sys/stock2.json",        # Yahoo Finance tickers
    "/.sys/city.json",          # Weather city / OWM ID / lat-lon
    "/.sys/key.json",           # OpenWeatherMap API key
    "/.sys/unit.json",          # Temperature unit (metric/imperial)
    "/.sys/weather_screen.json",# Cached weather display data
    "/.sys/monitor.json",       # AIDA64 RemoteSensor IP + interval
    "/.sys/brt.json",           # Brightness level
    "/.sys/timebrt.json",       # Night-mode schedule
    "/.sys/ntp.json",           # NTP server override
    "/.sys/tz.json",            # POSIX timezone string
    "/.sys/day.json",           # Date format (1/2/3)
    "/.sys/hour12.json",        # 12/24h clock
    "/.sys/timecolor.json",     # Clock digit colours
    "/.sys/delay.json",         # Boot countdown + dwell seconds
    "/.sys/album.json",         # Picture app settings
    "/.sys/gif.json",           # Selected weather GIF path
    "/.sys/sep.json",           # Separator config
    "/.sys/ccolor.json",        # Shared colour settings
    "/.sys/c_theme.json",       # Binance colour theme index
    "/.sys/g_theme.json",       # CoinGecko colour theme index
    "/.sys/c_chart.json",       # Binance chart visible flag
    "/.sys/st_chart.json",      # Stock chart visible flag
    "/.sys/cg_range.json",      # CoinGecko chart days range
    "/.sys/ck.json",            # Yahoo Finance crumb cache
    "/.sys/cru.json",           # Yahoo crumb URL
    "/.sys/cr.json",            # Binance rotation interval
    "/.sys/k.json",             # Binance kline interval
    "/.sys/stk.json",           # Stock kline interval
]

# Directories enumerated dynamically via /filelist. This is the authoritative discovery
# path -- prefer adding a directory here over hardcoding filenames anywhere else.
#
# /filelist is NOT recursive, so every sub-directory must be named. /image/80x80.gif is a
# DIRECTORY despite the extension, which is exactly the sort of thing a recursive-looking
# listing would hide.
LIST_DIRS: List[str] = [
    "/.sys",
    "/image",
    "/image/80x80.gif",
]

# Device paths this tool will NEVER copy, in either direction, under any flag.
#
# /.sys/config.json is the WiFi credential file, {"a":"ssid","p":"pass"}. Credentials are
# per-device and belong to whoever set the device up; a backup should not lift someone's
# network password onto a build machine, and a restore should not push one device's network
# onto another. A restored device comes up in AP mode and is configured through the portal,
# which is the correct outcome rather than a missing feature.
#
# Deliberately not gated behind an --include-credentials flag: a flag that exists gets used.
#
# The firmware side backs this up. It serves /.sys/config.json through its own handler that
# blanks both fields, so even a hand-rolled fetch of that path returns nothing sensitive.
NEVER_TRANSFER: Tuple[str, ...] = (
    "/.sys/config.json",
)

# Pages the FIRMWARE serves that are not files on the filesystem. They are fetched over
# HTTP exactly like a file, but they live in the binary, so /filelist never mentions them
# and re-uploading one would create a real file that shadows the route it came from.
#
# They are therefore saved under FW_PAGES_DIR and recorded in a SEPARATE manifest section,
# and restore never touches them. The point of capturing them is to feed a build step --
# e.g. keeping a copy of the OTA form as a reference for what a device serves.
#
# Keys are the URL, values the filename to save under.
FIRMWARE_PAGES: Dict[str, str] = {
    "/update": "update.html",
}
FW_PAGES_DIR = "_firmware_pages"

# Leading bytes of a gzip member. Content matching this is saved with a .gz suffix so the
# filename does not lie about what is inside it.
GZIP_MAGIC = b"\x1f\x8b"


# -- HTTP helpers --------------------------------------------------------------

def make_session() -> Session:
    """Session that transfers raw bytes - no auto-decompression of gzip."""
    s = requests.Session()
    # Suppress urllib3's automatic Content-Encoding decompression so that
    # files stored as gzip on device (HTML files) are saved as gzip bytes.
    s.headers["Accept-Encoding"] = "identity"
    return s


def get_raw(session: Session, url: str, timeout: int = 20) -> Tuple[int, bytes]:
    """GET url and return (status_code, raw_bytes).  Never raises."""
    try:
        r = session.get(url, timeout=timeout, stream=True)
        r.raw.decode_content = False   # belt-and-suspenders: skip decompression
        content = r.raw.read()
        return r.status_code, content
    except requests.RequestException as exc:
        print(f"  [ERR] GET {url}: {exc}", file=sys.stderr)
        return 0, b""


def post_file(session: Session, url: str, filename: str,
              data: bytes, timeout: int = 60) -> bool:
    """POST multipart file upload.  Returns True on HTTP 200."""
    try:
        r = session.post(
            url,
            files={"file": (filename, data, "application/octet-stream")},
            timeout=timeout,
        )
        return r.status_code == 200
    except requests.RequestException as exc:
        print(f"  [ERR] POST {url}: {exc}", file=sys.stderr)
        return False


def sha256hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fmt_size(n: int) -> str:
    if n >= 1_048_576:
        return f"{n/1_048_576:.1f} MB"
    if n >= 1_024:
        return f"{n/1_024:.1f} KB"
    return f"{n} B"


def normalise_host(host: str) -> str:
    host = host.strip().rstrip("/")
    if not host.startswith("http"):
        host = "http://" + host
    return host


# -- Discovery -----------------------------------------------------------------

def list_dir(session: Session, base: str, dir_path: str) -> List[str]:
    """Query /filelist?dir=<dir_path> and return the full device path of every file.

    The endpoint answers with an HTML table whose <a href> values are the paths.
    """
    code, body = get_raw(session, f"{base}/filelist?dir={dir_path}")
    if code != 200 or not body:
        return []
    text = body.decode("utf-8", errors="replace")
    paths = re.findall(r"href='(/[^']+)'", text)
    # Entries with no extension in the last segment are sub-directories, which are listed
    # explicitly in LIST_DIRS rather than followed here -- /filelist is not recursive.
    return [p for p in paths if "." in p.split("/")[-1]]


def discover_all_files(session: Session, base: str,
                       verbose: bool) -> Tuple[List[str], set]:
    """Every device path worth attempting, deduplicated and in a stable order.

    Returns (paths, guessed) where `guessed` holds those that came ONLY from a hardcoded
    list and were never confirmed by a directory listing. That distinction matters when
    reporting: a listed path failing to download is a broken backup, whereas a guessed path
    404ing just means the firmware has not written that config yet.

    Enumeration first, hardcoded names second. ROOT_FILES is unavoidable because the
    firmware will not list /, and SYS_FILES is only a safety net behind the live /.sys
    listing -- see the comment on SYS_FILES for why trusting it alone lost 12 files.
    """
    seen: set = set()
    paths: List[str] = []

    def add(p: str) -> bool:
        if p in seen:
            return False
        seen.add(p)
        paths.append(p)
        return True

    for p in ROOT_FILES:
        add(p)

    for d in LIST_DIRS:
        found = list_dir(session, base, d)
        if found:
            new = sum(add(p) for p in found)
            if verbose:
                print(f"  {d:20} {len(found):3} listed, {new} new")
        elif verbose:
            print(f"  {d:20} empty or not listable")

    guessed: set = set()
    for p in SYS_FILES:
        if add(p):
            guessed.add(p)
    if verbose and guessed:
        print(f"  {'(SYS_FILES fallback)':20} {len(guessed)} path(s) not in any listing")

    return paths, guessed


# -- Backup --------------------------------------------------------------------

def cmd_backup(args: argparse.Namespace) -> int:
    base = normalise_host(args.host)
    out = Path(args.output)
    session = make_session()

    # -- Check device reachability ---------------------------------------------
    print(f"Connecting to {base} ...")
    code, body = get_raw(session, f"{base}/space.json")
    if code != 200:
        sys.exit(f"Device unreachable at {base}  (HTTP {code})")
    # Initialised before the try: unparseable JSON left it unbound and the manifest build
    # below then raised NameError, turning a cosmetic problem into a failed backup.
    space: Dict = {}
    try:
        space = json.loads(body)
        total = space.get("total", 0)
        free  = space.get("free", 0)
        used  = total - free
        print(f"  LittleFS: {fmt_size(used)} used / {fmt_size(total)} total "
              f"({fmt_size(free)} free)")
    except (json.JSONDecodeError, KeyError, AttributeError):
        print("  (could not parse /space.json)")

    # -- Discover all files ----------------------------------------------------
    print("\nDiscovering files ...")
    all_paths, guessed_only = discover_all_files(session, base, verbose=True)
    print(f"  {len(all_paths)} candidate paths found")

    # Credentials never leave the device. See NEVER_TRANSFER.
    excluded = [p for p in all_paths if p in NEVER_TRANSFER]
    if excluded:
        all_paths = [p for p in all_paths if p not in NEVER_TRANSFER]
        guessed_only = {p for p in guessed_only if p not in NEVER_TRANSFER}
        for p in excluded:
            print(f"  excluded: {p} (credentials are never captured)")

    # --only narrows the capture to specific device paths. A build step should use it to
    # avoid fetching megabytes of gallery artwork nothing needs.
    if args.only:
        # Accept "index.html" as well as "/index.html".
        #
        # Not mere tolerance: on Windows, Git Bash and MSYS2 rewrite any argument that looks
        # like an absolute POSIX path into a Windows one, so `--only /index.html` arrives as
        # `C:/Program Files/Git/index.html` and matches nothing. The failure is baffling
        # because the command is correct as typed. Taking the basename-relative form as well
        # means one documented command works in every shell.
        want = {p if p.startswith("/") else "/" + p for p in args.only}
        unknown = want - set(all_paths)
        all_paths = [p for p in all_paths if p in want]
        guessed_only = {p for p in guessed_only if p in want}
        print(f"  --only: {len(all_paths)} of them selected")
        if unknown:
            # Requested but not discovered: report rather than fetch blindly, so a typo or a
            # file the device does not have is visible instead of becoming a silent 404.
            print(f"  --only: {len(unknown)} requested path(s) not found on the device:")
            for u in sorted(unknown):
                print(f"    {u}")
            return 1

    # -- Download --------------------------------------------------------------
    out.mkdir(parents=True, exist_ok=True)
    manifest: Dict = {
        "host": base,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "space": space,
        "files": {},
    }

    # Three outcomes for a path that will not download, and they mean different things.
    # Collapsing them into one "skipped" count is what let 12 genuinely missing files look
    # like a successful backup.
    #
    #   failed  - the directory listing said it exists, so this is a BROKEN backup.
    #   absent  - a SYS_FILES guess; the firmware has not written that config. Expected.
    #   noroot  - a ROOT_FILES entry. Root cannot be enumerated so these are guesses too,
    #             but the list is short and deliberate, so name them without crying wolf.
    #             Not every build ships every one (ii.html is absent by design here).
    ok = total_bytes = 0
    absent: List[str] = []
    noroot: List[str] = []
    failed: List[str] = []
    root_set = set(ROOT_FILES)
    print(f"\nDownloading to {out}/")
    for path in all_paths:
        code, data = get_raw(session, f"{base}{path}")
        if code != 200:
            entry = f"[{code:3}] {path}"
            if path in guessed_only:
                absent.append(entry)
            elif path in root_set:
                noroot.append(entry)
            else:
                failed.append(entry)
            continue

        # Name gzipped content .gz on disk. Several files (the web UI HTML) are stored
        # gzip-compressed on the device and served with Content-Encoding: gzip, and this tool
        # keeps the raw bytes -- so "index.html" on disk was gzip, not HTML. Editors, greps
        # and browsers all took it at face value and got binary. The suffix makes the content
        # honest; a packer feeding these back into a filesystem image strips it to recover the
        # device path, which is recorded verbatim in the manifest key either way.
        rel = path.lstrip("/")
        if data[:2] == GZIP_MAGIC and not rel.endswith(".gz"):
            rel += ".gz"
        local = out / rel
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(data)

        digest = sha256hex(data)
        manifest["files"][path] = {"size": len(data), "sha256": digest, "file": rel}
        total_bytes += len(data)
        ok += 1
        gz = "  (gzip)" if rel.endswith(".gz") else ""
        print(f"  ok   {len(data):>9,}  {path}{gz}")

    # -- Firmware-served pages -------------------------------------------------
    # Deliberately after the file loop and into a separate manifest key, so that nothing
    # here can ever be fed back to the device by restore.
    manifest["firmware_pages"] = {}
    fw_ok = 0
    for url, filename in FIRMWARE_PAGES.items():
        code, data = get_raw(session, f"{base}{url}")
        if code != 200 or not data:
            print(f"  [{code:3}] {url}  (firmware page, not captured)")
            continue
        # Same content-driven rule as the file loop: suffix .gz only if the bytes really are
        # gzip. Measured against a stock unit, /update is served PLAIN even when the request
        # offers gzip -- no Content-Encoding header, body starting "<!DO" -- while
        # /index.html comes back with Content-Encoding: gzip and 1f 8b. The page is stored
        # compressed in flash but decompressed before sending, so what a build step consumes
        # is plain HTML. This check means a route that ever does serve gzip is handled
        # automatically rather than being silently mislabelled.
        if data[:2] == GZIP_MAGIC and not filename.endswith(".gz"):
            filename += ".gz"
        local = out / FW_PAGES_DIR / filename
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(data)
        manifest["firmware_pages"][url] = {
            "file": f"{FW_PAGES_DIR}/{filename}",
            "size": len(data),
            "sha256": sha256hex(data),
        }
        fw_ok += 1
        gz = "  (gzip)" if filename.endswith(".gz") else ""
        print(f"  +  {len(data):>9,}  {url}  -> {FW_PAGES_DIR}/{filename}{gz}")

    # -- Write manifest --------------------------------------------------------
    manifest_path = out / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))

    print(f"\n{'-'*60}")
    print(f"  Saved : {ok} files ({fmt_size(total_bytes)})")
    if fw_ok:
        print(f"  Firmware-served pages: {fw_ok} in {FW_PAGES_DIR}/ "
              f"(reference only -- never restored)")
    if absent:
        print(f"  Not present on device: {len(absent)} (config the firmware has "
              f"not written yet -- expected)")
        if args.verbose:
            for line in absent:
                print(f"    {line}")
    if noroot:
        print(f"  Root files this build does not ship: {len(noroot)}")
        for line in noroot:
            print(f"    {line}")
    if failed:
        # Loud, and never hidden behind --verbose: the directory listing said these exist.
        print(f"  FAILED to download {len(failed)} file(s) THAT THE DEVICE LISTED:")
        for line in failed:
            print(f"    {line}")
        print("  This backup is INCOMPLETE -- do not treat it as a full copy.")
    print(f"  Manifest: {manifest_path}")
    return 1 if failed else 0


# -- Restore -------------------------------------------------------------------

def device_dir(device_path: str) -> Tuple[str, str]:
    """
    Split a device path like '/.sys/config.json' into (dir, filename).
    Returns ("", "index.html") for root-level files.
    """
    parts = device_path.rsplit("/", 1)
    if len(parts) == 2 and parts[0]:
        return parts[0], parts[1]
    return "", parts[-1]


def cmd_restore(args: argparse.Namespace) -> None:
    base = normalise_host(args.host)
    inp = Path(args.input)
    session = make_session()

    # -- Load manifest ---------------------------------------------------------
    manifest_path = inp / "manifest.json"
    if not manifest_path.exists():
        sys.exit(f"No manifest.json found in {inp}.  Run 'backup' first.")
    manifest = json.loads(manifest_path.read_text())
    print(f"Backup from {manifest.get('timestamp','?')}  "
          f"(source: {manifest.get('host','?')})")

    # -- Check device reachability ---------------------------------------------
    print(f"Connecting to {base} ...")
    code, _ = get_raw(session, f"{base}/space.json")
    if code != 200:
        sys.exit(f"Device unreachable at {base}  (HTTP {code})")

    # -- Build restore list ----------------------------------------------------
    to_restore: List[Tuple[str, Path, int]] = []  # (device_path, local_path, size)

    root_paths = set(ROOT_FILES)

    for device_path, info in manifest["files"].items():
        # The on-disk name can differ from the device path -- gzipped content is stored with a
        # .gz suffix. The manifest records the local name, so trust that; fall back to deriving
        # it only for backups written before the suffix existed.
        # Credentials never travel, even if an older backup captured them. See NEVER_TRANSFER.
        if device_path in NEVER_TRANSFER:
            print(f"  [skip] {device_path} (credentials are never restored)")
            continue

        local = inp / info.get("file", device_path.lstrip("/"))
        if not local.exists():
            print(f"  [MISSING] {device_path} -- not in backup directory, skipping")
            continue

        # --include-html gates every ROOT_FILES entry, not just the HTML: favicon.ico is in
        # that list too, so a default restore leaves the favicon off. The default exists
        # because the web UI normally arrives with the firmware rather than from a backup.
        #
        # Note this is keyed on the DEVICE path, never on the .gz suffix. When these are
        # restored the local file is index.html.gz but the upload name comes from the device
        # path below, so the gzip bytes land as index.html exactly as the filesystem holds
        # them -- the suffix is a local convention and never reaches the device.
        is_root_file = device_path in root_paths

        if is_root_file and not args.include_html:
            if args.verbose:
                print(f"  [skip] {device_path} (use --include-html to restore)")
            continue

        to_restore.append((device_path, local, info["size"]))

    if not to_restore:
        print("Nothing to restore.")
        return

    print(f"\nRestoring {len(to_restore)} files to {base} ...")

    ok = failed = total_bytes = 0
    for device_path, local, _expected_size in to_restore:
        data = local.read_bytes()
        dir_part, filename = device_dir(device_path)

        upload_url = f"{base}/doUpload?dir={dir_part}"
        success = post_file(session, upload_url, filename, data)

        if success:
            print(f"  ok   {len(data):>9,}  {device_path}")
            ok += 1
            total_bytes += len(data)
        else:
            print(f"  XX   FAILED          {device_path}")
            failed += 1

    print(f"\n{'-'*60}")
    print(f"  Restored : {ok} files ({fmt_size(total_bytes)})")
    if failed:
        print(f"  Failed   : {failed} files")

    if args.reboot and ok > 0:
        print("\nRebooting device ...")
        code, _ = get_raw(session, f"{base}/set?reboot=1")
        if code == 200:
            print("  Reboot command sent.")
        else:
            print(f"  Reboot failed (HTTP {code})")
    elif ok > 0:
        print("\nTip: reboot the device to apply all config changes.")
        print(f"     {base}/set?reboot=1")


# -- Verify --------------------------------------------------------------------

def cmd_verify(args: argparse.Namespace) -> None:
    base = normalise_host(args.host)
    inp = Path(args.input)
    session = make_session()

    manifest_path = inp / "manifest.json"
    if not manifest_path.exists():
        sys.exit(f"No manifest.json found in {inp}.")
    manifest = json.loads(manifest_path.read_text())

    files = manifest["files"]
    print(f"Verifying {len(files)} files from backup "
          f"({manifest.get('timestamp','?')}) against {base} ...\n")

    match = differ = missing = 0
    for path, info in files.items():
        code, data = get_raw(session, f"{base}{path}")
        if code != 200:
            print(f"  MISSING  {path}")
            missing += 1
            continue

        digest = sha256hex(data)
        size_ok = len(data) == info["size"]
        hash_ok = digest == info["sha256"]

        if size_ok and hash_ok:
            print(f"  ok   {path}")
            match += 1
        else:
            details = []
            if not size_ok:
                details.append(f"size: device={len(data)} backup={info['size']}")
            if not hash_ok:
                details.append("sha256 mismatch")
            print(f"  XX   {path}  [{', '.join(details)}]")
            differ += 1

    print(f"\n{'-'*60}")
    print(f"  Match  : {match}")
    print(f"  Differ : {differ}")
    print(f"  Missing: {missing}")
    if differ == 0 and missing == 0:
        print("  All files match -- backup is current.")


# -- Entry point ---------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(
        prog="device_fs.py",
        description="Back up, restore and verify a device filesystem over HTTP",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
commands:
  backup   Download every discoverable file from the device.
  restore  Upload backed-up files back to the device.
           By default, web UI HTML files (index.html etc.) are skipped
           because they are restored by flashing firmware, not by this tool.
  verify   Compare backup checksums against the live device.

examples:
  python scripts/device_fs.py backup  --host <ip>
  python scripts/device_fs.py backup  --host <ip> --output ./my-backup
  python scripts/device_fs.py restore --host <ip> --input  ./backup
  python scripts/device_fs.py restore --host <ip> --input  ./backup --include-html --reboot
  python scripts/device_fs.py verify  --host <ip> --input  ./backup
""",
    )
    p.add_argument("command", choices=["backup", "restore", "verify"],
                   help="Action to perform")
    p.add_argument("--host", required=True,
                   help="Device IP or hostname  (e.g. <ip> or smoltv.local)")
    p.add_argument("--output", default="./backup",
                   help="Directory to save backup into  [./backup]")
    p.add_argument("--input", default="./backup",
                   help="Directory to restore from  [./backup]")
    p.add_argument("--include-html", action="store_true",
                   help="(restore) Also restore ROOT files -- the web UI HTML AND favicon.ico; "
                        "skipped by default because they normally come with the firmware")
    p.add_argument("--reboot", action="store_true",
                   help="(restore) Reboot device after upload completes")
    p.add_argument("--verbose", "-v", action="store_true",
                   help="Show skipped / 404 files during backup")
    p.add_argument("--only", nargs="+", metavar="PATH", default=None,
                   help="(backup) Capture ONLY these device paths, e.g. --only index.html "
                        "image/bootxp.gif. Use this from automation to avoid fetching "
                        "megabytes of gallery artwork nothing needs. A leading slash is "
                        "optional, and omitting it avoids MSYS2/Git Bash rewriting the "
                        "argument into a Windows path")

    args = p.parse_args()

    # Propagate the exit code. This used to discard it, so a backup that failed -- an
    # unreachable file, a bad --only path -- still exited 0 and any caller treated the
    # incomplete capture as a success.
    if args.command == "backup":
        return cmd_backup(args) or 0
    if args.command == "restore":
        return cmd_restore(args) or 0
    if args.command == "verify":
        return cmd_verify(args) or 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
