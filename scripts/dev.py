#!/usr/bin/env python3
"""Build, flash and drive a device over its HTTP API.

    uv run scripts/dev.py flash              # build, park, OTA, verify the version
    uv run scripts/dev.py flash --no-build   # flash whatever is already built
    uv run scripts/dev.py app Stock          # switch the running app
    uv run scripts/dev.py park               # park on Clock and wait until it serves
    uv run scripts/dev.py version            # what is running vs what is in the tree
    uv run scripts/dev.py cycle Stock        # flash, switch to Stock, settle
    uv run scripts/dev.py flashfs            # build and OTA the FILESYSTEM, then verify it
    uv run scripts/dev.py flashfs --keep-wifi   # ... and carry the credentials across
    uv run scripts/dev.py web                # pack and push the whole web UI over HTTP
    uv run scripts/dev.py web style.css      # ... or just the named files

The address defaults to $SMOLTV_HOST; pass --host to override. There is no default IP
baked in, because one developer's device address is not a project constant.

THE TWO WAYS AN OTA SILENTLY LIES, both of which this exists to stop:

 1. FLASHING A STALE BINARY. Uploading .pio/build/... does not build it first. Editing a
    header and running a flash therefore ships the PREVIOUS image, and everything looks
    like it worked. So `flash` builds by default; --no-build opts out.

 2. VERIFYING AGAINST THE PRE-REBOOT FIRMWARE. Polling straight after the upload is
    answered by the instance that is still running - for a second or two every check
    passes and you carry on testing the OLD image. Waiting for "it responds" is therefore
    not a verification at all.

    The fix is to compare IDENTITY, not liveness: read FW_VERSION out of src/Config.h,
    then poll /v.json until its "v" EQUALS that string. Bump FW_VERSION when you change
    the firmware and this is exact. Leave it unchanged and `flash` says so rather than
    pretending to have verified anything.

/update needs contiguous heap, and an app holding a TLS session does not leave enough -
the device answers "Update failed: Aborted". Parking on Clock first frees it, so `flash`
does that automatically.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / ".pio" / "build" / "smalltv_pro" / "firmware.bin"
FS_BIN = ROOT / ".pio" / "build" / "smalltv_pro" / "littlefs.bin"
DATA = ROOT / "data"
CONFIG_H = ROOT / "src" / "Config.h"


def clean(s) -> str:
    """ASCII-only. Device output reaches a terminal, and possibly an agent session."""
    return "".join(c for c in (s or "") if c in "\t\n" or 32 <= ord(c) < 127)


def host_of(a) -> str:
    h = a.host or os.environ.get("SMOLTV_HOST", "").strip()
    if not h:
        sys.exit("no device address: pass --host <ip> or set SMOLTV_HOST")
    return h


def get(host: str, path: str, timeout: float = 5.0):
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
            return r.read().decode("ascii", "replace")
    except (urllib.error.URLError, OSError, TimeoutError):
        return None


# Both images identify themselves in their first 16 bytes. The DEVICE checks this too and
# answers 400, but only after the whole file has gone over the wire and been drained - which for
# littlefs.bin is 3.8 MB and the better part of a minute. Checking here fails in milliseconds.
APP_MAGIC = 0xE9              # ESP32 image header, byte 0
FS_MAGIC = b"littlefs"        # littlefs superblock, offset 8


def check_image(path: Path, want_fs: bool) -> bool:
    """True when `path` is the kind of image the caller is about to upload."""
    with path.open("rb") as f:
        head = f.read(16)
    if len(head) < 16:
        print(f"{path.name} is only {len(head)} bytes - not an image")
        return False
    is_fs = head[8:16] == FS_MAGIC
    is_app = head[0] == APP_MAGIC
    if want_fs and not is_fs:
        print(f"REFUSING: {path.name} is not a filesystem image "
              f"(expected {FS_MAGIC.decode()} at offset 8"
              f"{', this looks like FIRMWARE' if is_app else ''}).")
        return False
    if not want_fs and not is_app:
        print(f"REFUSING: {path.name} is not a firmware image "
              f"(expected 0x{APP_MAGIC:02x} at byte 0"
              f"{', this looks like a FILESYSTEM image' if is_fs else ''}).")
        return False
    return True


def get_raw(host: str, path: str, timeout: float = 10.0):
    """Raw bytes, for comparing what the device serves against what was packed.

    NO Accept-Encoding header: the .html and .css files are STORED gzipped and served with
    Content-Encoding: gzip, and urllib does not ask for compression, so what arrives is the
    stored bytes - exactly what data/ holds. Decoding here would compare the wrong thing.
    """
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
            return r.read()
    except (urllib.error.URLError, OSError, TimeoutError):
        return None


def source_version() -> str:
    """FW_VERSION as the tree currently defines it."""
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', CONFIG_H.read_text(encoding="utf-8"))
    if not m:
        sys.exit(f"could not find FW_VERSION in {CONFIG_H}")
    return m.group(1)


def device_version(host: str):
    """The "v" field of /v.json, or None when the device is not answering."""
    body = get(host, "/v.json", timeout=3)
    if not body:
        return None
    m = re.search(r'"v"\s*:\s*"([^"]*)"', body)
    return m.group(1) if m else None


def wait_for_version(host: str, want: str, limit: float = 90.0) -> bool:
    """Poll until /v.json reports EXACTLY `want`. See point 2 in the module docstring."""
    t0 = time.time()
    while time.time() - t0 < limit:
        if device_version(host) == want:
            return True
        time.sleep(1.0)
    return False


def wait_alive(host: str, limit: float = 90.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < limit:
        if get(host, "/space.json", timeout=3) is not None:
            return True
        time.sleep(1.0)
    return False


# -- commands ---------------------------------------------------------------------------

def _scrub_msys_env() -> None:
    """Hide MSYS/MinGW from the build, or every pio run prints the same four-line failure.

    The pioarduino platform runs idf_tools.py to install its esptool, and idf_tools.py
    REFUSES to run under MSYS ("MSys/Mingw is not supported"). It then falls back to
    `pip install <tool-esptoolpy dir>`, which is not a pip package either, so a build from
    any MSYS-flavoured shell - Git Bash, and anything launched from it, which inherits
    MSYSTEM even when the child is PowerShell - emits:

        idf_tools.py installation failed (rc=1) ... MSys/Mingw is not supported
        error: ...tool-esptoolpy does not appear to be a Python project
        Warning: Failed to install esptool from ...tool-esptoolpy (exit 2)

    twice per invocation. It is only noise here because esptool is already present in the
    PlatformIO venv, but it buries real output and has sent us chasing the wrong fault once.
    Unsetting these for OUR CHILD PROCESSES ONLY (os.environ of this script, which pio
    inherits) lets idf_tools.py run normally. The parent shell is untouched.
    """
    for var in ("MSYSTEM", "MSYSTEM_CARCH", "MSYSTEM_CHOST", "MSYSTEM_PREFIX",
                "MINGW_CHOST", "MINGW_PREFIX", "MINGW_PACKAGE_PREFIX"):
        os.environ.pop(var, None)


def _pio() -> str:
    """Locate the PlatformIO CLI.

    Its installer puts pio in a virtualenv under ~/.platformio that is NOT added to PATH, so
    calling "pio" outright fails with a bare WinError 2 that says nothing about what is
    missing. Prefer PATH, fall back to that venv.
    """
    _scrub_msys_env()
    found = shutil.which("pio")
    if found:
        return found
    for c in (Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe",
              Path.home() / ".platformio" / "penv" / "bin" / "pio"):
        if c.exists():
            return str(c)
    sys.exit("FATAL: pio not found on PATH or in ~/.platformio/penv - is PlatformIO installed?")


def cmd_build(_a) -> int:
    print("building ...")
    return subprocess.run([_pio(), "run", "-e", "smalltv_pro"], cwd=ROOT).returncode


def cmd_app(a) -> int:
    host = host_of(a)
    body = clean(get(host, f"/set?open_app={a.name}", timeout=6)).strip()
    print(f"app {a.name} -> {body or 'no response'}")
    return 0 if body == "OK" else 1


def cmd_park(a) -> int:
    # Clock touches no network, so the heap is free for the flash write.
    host = host_of(a)
    clean(get(host, "/set?open_app=Clock", timeout=6))
    if not wait_alive(host):
        print("device not serving after park")
        return 1
    print("parked on Clock, serving")
    return 0


def cmd_version(a) -> int:
    host = host_of(a)
    src = source_version()
    dev = device_version(host)
    print(f"  source  {src}")
    print(f"  device  {dev or '(not answering)'}")
    if dev is None:
        return 1
    print("  -> " + ("match" if dev == src
                     else "DIFFERENT - the device is not running this tree"))
    return 0 if dev == src else 1


def cmd_flash(a) -> int:
    host = host_of(a)
    want = source_version()

    if not a.no_build and cmd_build(a) != 0:
        print("build failed")
        return 1
    if not BIN.is_file():
        print(f"no {BIN.relative_to(ROOT)} - build first")
        return 1
    if not check_image(BIN, want_fs=False):
        return 1

    running = device_version(host)
    print(f"device {running or '(not answering)'} -> flashing {want}")
    if running == want and not a.force:
        # Not a failure, but say so plainly: the check below is an equality test against
        # this very string, so a flash now could not be verified either way.
        print("FW_VERSION is unchanged, so the flash cannot be verified.")
        print("Bump FW_VERSION in src/Config.h, or pass --force to flash anyway.")
        return 1

    cmd_park(a)

    r = subprocess.run(
        ["curl", "-s", "-m", "180", "-F", f"update=@{BIN}", f"http://{host}/update"],
        capture_output=True, text=True)
    body = clean(r.stdout)
    if "Update OK" not in body:
        print("UPLOAD FAILED:", body[:200] or "(no response)")
        return 1
    print("upload accepted, waiting for the reboot")

    time.sleep(3.0)                       # let it actually reset before polling
    if a.force and running == want:
        ok = wait_alive(host)
        print("serving again (version unchanged, so this is liveness only)" if ok
              else "device did not come back")
        return 0 if ok else 1
    if not wait_for_version(host, want):
        print(f"TIMED OUT: /v.json never reported {want} (still {device_version(host)})")
        return 1
    print(f"verified: the device is running {want}")
    return 0


def cmd_flashfs(a) -> int:
    """Build the filesystem image and write it over the air.

    NOT the same risk profile as a firmware OTA. The app partitions are A/B and roll back; the
    filesystem has ONE copy, so a failed write leaves it unusable and the device reboots into a
    formatted, empty one - reachable, because AP mode and the built-in /update page need no
    filesystem at all, but empty.

    Parking first is not politeness, it is required: the Picture app holds a GIF open across
    frames, and the device unmounts the filesystem the moment the upload starts.
    """
    host = host_of(a)

    if not a.no_build:
        print("building the filesystem image ...")
        if subprocess.run([_pio(), "run", "-e", "smalltv_pro", "-t", "buildfs"],
                          cwd=ROOT).returncode != 0:
            print("buildfs failed")
            return 1
    if not FS_BIN.is_file():
        print(f"no {FS_BIN.relative_to(ROOT)} - run buildfs first")
        return 1
    if not check_image(FS_BIN, want_fs=True):
        return 1

    # What the image should contain, so the verification below compares against the source of
    # truth rather than trusting the upload's own 200.
    want = {}
    for f in sorted(DATA.rglob("*")):
        if f.is_file():
            want["/" + f.relative_to(DATA).as_posix()] = hashlib.sha256(f.read_bytes()).hexdigest()
    print(f"{FS_BIN.name}: {FS_BIN.stat().st_size:,} B, {len(want)} files in data/")

    if not a.keep_wifi:
        print("NOTE: this ERASES /.sys - WiFi credentials included. Pass --keep-wifi to carry")
        print("      them across in NVS, or expect the device in AP mode afterwards.")

    cmd_park(a)

    url = f"http://{host}/update?target=fs"
    if a.keep_wifi:
        url += "&keepwifi=1"
    r = subprocess.run(["curl", "-s", "-m", "300", "-F", f"update=@{FS_BIN}", url],
                       capture_output=True, text=True)
    body = clean(r.stdout)
    if "Filesystem written" not in body:
        print("UPLOAD FAILED:", body[:300] or "(no response)")
        return 1
    print("upload accepted, waiting for the reboot")

    time.sleep(4.0)

    # Without --keep-wifi there is nothing to wait for: the image erased the credentials, so the
    # device is now an access point and this host cannot reach it. Verification is impossible by
    # construction, and pretending otherwise would report a failure that is the expected result.
    if not a.keep_wifi:
        print("no --keep-wifi: the device is rebooting into AP mode, so it cannot be reached")
        print('  join "SmolTV PRO" and open http://192.168.4.1 to set up Wi-Fi again')
        print("  (nothing verified from here - use the serial log if you need proof)")
        return 0

    if not wait_alive(host, limit=120.0):
        print("device did not come back")
        return 1

    # Verify by CONTENT: fetch each file the image should hold and compare its hash. A 200 from
    # the upload only says bytes arrived, not that they landed in a mountable filesystem.
    bad = []
    for path, digest in want.items():
        got = get_raw(host, path, timeout=30)
        if got is None:
            bad.append(f"{path}: no response")
        elif hashlib.sha256(got).hexdigest() != digest:
            bad.append(f"{path}: {len(got)} B, hash mismatch")
    if bad:
        print(f"VERIFY FAILED, {len(bad)} of {len(want)} files wrong:")
        for b in bad:
            print("  " + b)
        return 1
    print(f"verified: all {len(want)} files match data/")
    if a.keep_wifi:
        print("wifi carried across" if device_version(host) else "device answered")
    return 0


def cmd_web(a) -> int:
    """Push web UI files to a running device over /doUpload, then verify by readback.

    The fast iteration loop for html/: seconds per page instead of a full filesystem OTA,
    and /.sys is untouched, so nothing about it is destructive - WiFi credentials included.

    Two rules carried over from the OTA commands:

     1. It PACKS FIRST (--no-build opts out), for the same reason `flash` builds first:
        uploading data/ after editing html/ ships the PREVIOUS pack, and everything looks
        like it worked. pack_fs only rewrites changed files, so an unchanged pack is cheap.

     2. It only pushes files the pack LAYOUT defines, taken from data/ - never html/
        directly - so it cannot put anything on the device that a filesystem build would
        not produce (the .html files are stored as gzip bytes, which is what data/ holds).

    Verification is by CONTENT: each file is read back with no Accept-Encoding, so what
    arrives is the stored bytes, and its hash must equal the data/ file that was sent.
    """
    host = host_of(a)

    # Sibling scripts. sys.path[0] is scripts/ when dev.py is run as a script, but be
    # explicit so this also works if dev.py is ever imported.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import device_fs
    import pack_fs

    if not a.no_build:
        print("packing data/ ...")
        if pack_fs.pack() != 0:
            print("pack failed")
            return 1

    all_paths = [dev for dev, _kind, _rel in pack_fs.layout()]

    if a.files:
        targets = []
        for f in a.files:
            # Accept "style.css", "/style.css", "html/style.css" and "data/style.css" -
            # they all name the same device path.
            p = f.replace("\\", "/")
            for prefix in ("html/", "data/"):
                if p.startswith(prefix):
                    p = p[len(prefix):]
            if not p.startswith("/"):
                p = "/" + p
            if p not in all_paths:
                print(f"unknown file {f!r} - the packed filesystem holds:")
                for q in all_paths:
                    print(f"  {q}")
                return 1
            targets.append(p)
    else:
        # The whole web UI, whichever source is configured - not the artwork, which is
        # bulky and never part of the html/ iteration loop. Name a file to push one.
        targets = [dev for dev, _kind, _rel in pack_fs.WEB_UI[pack_fs.web_ui_choice()]]

    base = device_fs.normalise_host(host)
    session = device_fs.make_session()

    failed = []
    for p in targets:
        local = DATA / p.lstrip("/")
        if not local.is_file():
            print(f"  FAIL {p}: no {local.relative_to(ROOT)} - pack first")
            failed.append(p)
            continue
        data = local.read_bytes()
        dir_part, name = p.rsplit("/", 1)
        if not device_fs.post_file(session, f"{base}/doUpload?dir={dir_part}", name, data):
            print(f"  FAIL {p}: upload refused")
            failed.append(p)
            continue
        got = get_raw(host, p, timeout=15)
        if got is None:
            print(f"  FAIL {p}: uploaded but no readback")
            failed.append(p)
        elif got != data:
            print(f"  FAIL {p}: readback differs "
                  f"(sent {len(data):,} B {hashlib.sha256(data).hexdigest()[:12]}, "
                  f"got {len(got):,} B {hashlib.sha256(got).hexdigest()[:12]})")
            failed.append(p)
        else:
            print(f"  ok   {len(data):>7,} B  {p}")

    if failed:
        print(f"{len(failed)} of {len(targets)} file(s) FAILED")
        return 1
    print(f"verified: {len(targets)} file(s) on the device match data/")
    return 0


def cmd_cycle(a) -> int:
    if cmd_flash(a) != 0:
        return 1
    if a.name:
        cmd_app(a)
    if a.settle:
        print(f"settling {a.settle:.0f}s")
        time.sleep(a.settle)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("--host", default="", help="device address (default: $SMOLTV_HOST)")

    # The same flag on every subcommand, so it works either side of it. SUPPRESS matters:
    # without it the subparser's default would overwrite a value given before the
    # subcommand, and `dev.py --host X flash` would silently lose the address.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--host", default=argparse.SUPPRESS,
                        help="device address (default: $SMOLTV_HOST)")

    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("build", help="pio run -e smalltv_pro", parents=[common]).set_defaults(fn=cmd_build)
    sub.add_parser("park", help="switch to Clock and wait until it serves", parents=[common]) \
       .set_defaults(fn=cmd_park)
    sub.add_parser("version", help="compare the running version with the tree", parents=[common]) \
       .set_defaults(fn=cmd_version)

    f = sub.add_parser("flash", help="build, park, OTA, then verify the version", parents=[common])
    f.add_argument("--no-build", action="store_true", help="flash the existing binary")
    f.add_argument("--force", action="store_true",
                   help="flash even when FW_VERSION is unchanged (unverifiable)")
    f.set_defaults(fn=cmd_flash)

    fs = sub.add_parser("flashfs", help="build and OTA the filesystem image, then verify it",
                        parents=[common])
    fs.add_argument("--no-build", action="store_true", help="upload whatever is already built")
    fs.add_argument("--keep-wifi", action="store_true",
                    help="carry the WiFi credentials across in NVS; without this the device "
                         "comes back in AP mode, because the image erases /.sys")
    fs.set_defaults(fn=cmd_flashfs)

    w = sub.add_parser("web", help="pack, push web UI files over HTTP, verify by readback",
                       parents=[common])
    w.add_argument("files", nargs="*",
                   help="files to push, e.g. style.css index.html (default: the whole web UI)")
    w.add_argument("--no-build", action="store_true",
                   help="push data/ as it stands instead of packing first")
    w.set_defaults(fn=cmd_web)

    p = sub.add_parser("app", help="switch the running app", parents=[common])
    p.add_argument("name")
    p.set_defaults(fn=cmd_app)

    c = sub.add_parser("cycle", help="flash, switch app, settle", parents=[common])
    c.add_argument("name", nargs="?")
    c.add_argument("--settle", type=float, default=25.0)
    c.add_argument("--no-build", action="store_true")
    c.add_argument("--force", action="store_true")
    c.set_defaults(fn=cmd_cycle)

    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
