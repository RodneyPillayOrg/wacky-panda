#!/usr/bin/env python3
"""PlatformIO pre-build hook: make sure the generated assets and data/ are current.

Wired up from platformio.ini as `extra_scripts = pre:scripts/prebuild.py`, so it runs
before every build. It can also be run by hand:

    python scripts/prebuild.py

Three steps, each a no-op when already satisfied:

  1. ASSETS   run scripts/build_assets.py if the manifest is newer than what it produced.
              Skipped otherwise -- a full render takes over a minute (240 GIF frames are
              projected in Python), which is far too slow to repeat on every compile.

  2. CAPTURE  some files can only come off a factory device -- the weather and gallery
              artwork, and the web UI if you ask for the stock one. If assets/cache/stock/
              is missing them, capture them with scripts/device_fs.py. Cached afterwards,
              so this happens once per checkout.

  3. PACK     assemble data/ from those two sources via scripts/pack_fs.py.

THE STOCK ADDRESS IS A BUILD PARAMETER, and this hook never prompts for it. Supply it as
an environment variable:

    powershell:  $env:SMOLTV_STOCK_HOST='<ip>'; pio run -t buildfs
    bash:        SMOLTV_STOCK_HOST=<ip> pio run -t buildfs

or persist it in platformio.ini under the environment being built:

    custom_stock_host = <ip>

THE WEB UI HAS TWO SOURCES: this project's own pages in html/, or a factory unit's. Select
with SMOLTV_WEB_UI=html|stock, or custom_web_ui in platformio.ini. See pack_fs.py.

Without one, the step fails and prints both forms. It does not prompt because prompting
from inside a PlatformIO build does not work -- see the note in step_stock, which records
what was tried so it is not attempted again.

Set SMOLTV_SKIP_PREBUILD=1 to bypass all of this.
"""

import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

def say(msg: str = "") -> None:
    """Write a line of hook output -- on STDERR, deliberately, and flushed.

    PlatformIO relays a build's stdout and stderr through separate pipes with independent
    timing, so lines written to one cannot be ordered against the other: messages from this
    hook interleaved into the middle of lists it had printed itself. Neither line buffering
    nor flushing every line fixes that (both were tried), because the reordering happens in
    the relay, not here. Keeping ALL of this hook's output on a single stream is what orders
    it. Note sys.stdout.reconfigure(line_buffering=True) is not merely ineffective but
    unavailable -- SCons substitutes its own stdout object -- so it fails silently.
    """
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def _project_option(name: str) -> str:
    """Read a custom_* option from platformio.ini for the environment being built.

    Only available when running as a build hook -- SCons injects the `env` that knows about
    project options. Running this script by hand returns "" and the caller falls back to the
    environment variable.
    """
    try:
        Import("env")                                        # type: ignore # noqa: F821
        return (env.GetProjectOption(name, "") or "").strip()  # type: ignore # noqa: F821
    except Exception:
        return ""


def _project_root() -> Path:
    """Locate the project root in both of the ways this file gets executed.

    Run directly, __file__ works. As a PlatformIO extra_script it does NOT: SCons exec()s
    the source, so the module has no __file__ at all and referencing it raises NameError
    before anything else runs. SCons injects an `env` instead, which knows PROJECT_DIR.
    """
    try:
        return Path(__file__).resolve().parent.parent
    except NameError:
        pass
    try:                                   # injected by SCons into the exec'd namespace
        Import("env")                      # type: ignore # noqa: F821
        return Path(env["PROJECT_DIR"])    # type: ignore # noqa: F821
    except Exception:
        # PlatformIO runs SCons with the project directory as cwd.
        return Path.cwd()


ROOT = _project_root()
SCRIPTS = ROOT / "scripts"
BUILD = ROOT / "assets" / "build"
STOCK = ROOT / "assets" / "cache" / "stock"
MANIFEST = SCRIPTS / "assets.toml"

LOCK = SCRIPTS / "assets.lock.toml"

# The pipeline's outputs, SPLIT BY WHO CONSUMES THEM. This distinction is the difference
# between a clone that builds and one that does not.
#
#   FIRMWARE_OUTPUTS are compiled into the image, and are COMMITTED. A clone has them, so a
#   firmware build needs no network, no rasteriser and no captured device.
#
#   FS_OUTPUTS are only read by pack_fs when assembling data/, and live in the untracked
#   assets/build/. A clone can never have them - which is why gating every build on them
#   made `pio run` demand the full fetch-render-emit on a fresh checkout, contradicting the
#   whole point of committing the headers.
FIRMWARE_OUTPUTS = [
    ROOT / "src" / "LvFontsGen.h",
    ROOT / "src" / "LvImagesGen.h",
]
FS_OUTPUTS = [
    BUILD / "gifs" / "earth.gif",
    BUILD / "icos" / "favicon.ico",
]


def _py() -> str:
    """The interpreter to run helper scripts with.

    sys.executable is PlatformIO's own Python when invoked as a build hook, which does not
    have this project's dependencies. Prefer .venv/ at the project root - which is exactly
    where `uv sync` puts them, so a clone that has run it needs no further wiring.
    """
    for c in (ROOT / ".venv" / "Scripts" / "python.exe", ROOT / ".venv" / "bin" / "python"):
        if c.exists():
            return str(c)
    return sys.executable


def run(args, **kw) -> int:
    say(f"  $ {' '.join(str(a) for a in args)}")
    return subprocess.run(args, cwd=ROOT, **kw).returncode


def want_filesystem() -> bool:
    """Is data/ actually needed by this invocation?

    Only the filesystem targets read it - `buildfs`, `uploadfs`. A plain `pio run` compiles
    the firmware and never looks at data/, so making it wait for an asset render, a capture
    from another device and a pack is pure cost. Run by hand with no SCons globals, do
    everything: someone invoking the hook directly wants the whole thing.

    COMMAND_LINE_TARGETS, not env["BUILD_TARGETS"] - the latter is still None this early in
    the build, so reading it silently classified every invocation as firmware-only.
    """
    try:
        targets = [str(t).lower() for t in COMMAND_LINE_TARGETS]   # type: ignore # noqa: F821
    except Exception:
        return True                                   # run by hand: do everything
    return any("fs" in t for t in targets)


def _manifest_sha() -> str:
    return hashlib.sha256(MANIFEST.read_bytes()).hexdigest() if MANIFEST.is_file() else ""


def _locked_manifest_sha() -> str:
    """The manifest hash recorded by the last successful pipeline run.

    Parsed with a regex rather than tomllib: this runs under PlatformIO's interpreter, which
    may predate 3.11, and the one value needed is a hex string on its own line.
    """
    if not LOCK.is_file():
        return ""
    m = re.search(r'^\s*sha256\s*=\s*"([0-9a-f]{64})"\s*$',
                  LOCK.read_text(encoding="utf-8"), re.M)
    return m.group(1) if m else ""


def assets_current(needed) -> bool:
    """True when `needed` all exist AND were built from the manifest as it stands now."""
    if not MANIFEST.is_file():
        return True                               # nothing to build against
    if any(not out.is_file() for out in needed):
        return False
    locked = _locked_manifest_sha()
    if not locked:
        # A lock file from before the hash was recorded. Trust the committed outputs rather
        # than forcing a network round trip on every build; editing the manifest is what is
        # supposed to trigger a rebuild, and that will be caught once the lock is rewritten.
        return True
    return locked == _manifest_sha()


def step_assets() -> int:
    needed = list(FIRMWARE_OUTPUTS) + (list(FS_OUTPUTS) if want_filesystem() else [])
    if assets_current(needed):
        say("  assets current -- skipping build_assets")
        return 0
    missing = [o.name for o in needed if not o.is_file()]
    why = f"missing {', '.join(missing)}" if missing else "assets.toml changed"
    say(f"  {why} -- running the asset pipeline")
    return run([_py(), str(SCRIPTS / "build_assets.py"), "--fetch", "--render", "--emit"])


def _pack_fs():
    sys.path.insert(0, str(SCRIPTS))
    import pack_fs                                    # noqa: E402
    return pack_fs


def art() -> str:
    """Whether this build includes the vendor artwork: $SMOLTV_ART, custom_art, or the
    default of none - on which nothing at all comes off a factory unit."""
    return _pack_fs().art_choice(
        os.environ.get("SMOLTV_ART", "").strip() or _project_option("custom_art"))


def web_ui() -> str:
    """Which web UI this build packs: $SMOLTV_WEB_UI, custom_web_ui, or pack_fs's default.

    Two ways to obtain the three pages -- gzip this project's html/, or use the ones captured
    off a factory unit. Only the second needs a capture, so the choice decides whether the
    step below has anything to do.
    """
    return _pack_fs().web_ui_choice(
        os.environ.get("SMOLTV_WEB_UI", "").strip() or _project_option("custom_web_ui"))


def stock_missing() -> list:
    """Which stock-sourced files pack_fs wants but the cache does not have."""
    pack_fs = _pack_fs()
    return [rel for _dev, kind, rel in pack_fs.layout(web_ui(), art())
            if kind == "stock" and not (STOCK / rel).is_file()]


def step_stock() -> int:
    missing = stock_missing()
    if not missing:
        say(f"  stock assets cached ({STOCK.relative_to(ROOT)})")
        return 0

    say(f"  {len(missing)} stock file(s) not cached "
        f"(web UI: {web_ui()}, artwork: {art()}):")
    for m in missing:
        say(f"    {m}")
    if any(m.endswith(".html.gz") for m in missing):
        say("")
        say("  The web UI is among these only because this build asked for the stock one.")
        say("  SMOLTV_WEB_UI=html uses this project's pages instead.")

    # The address is a BUILD PARAMETER -- this hook never prompts.
    #
    # Prompting from inside a build was tried thoroughly and does not work. PlatformIO
    # captures the build's output and re-emits it through a line-oriented assembler, so a
    # prompt with no trailing newline is held there and the user is left typing an answer to
    # an invisible question. It is not a buffering problem and not specific to one stream:
    # stdout with flush, stderr with flush, and writing straight to the console device
    # (CONOUT$/CONIN$, the getpass approach) were all tested at a real terminal and all
    # failed. Only a newline-terminated question with the answer typed on the line below
    # ever displayed, which is not a prompt anyone wants. Do not reintroduce it.
    host = os.environ.get("SMOLTV_STOCK_HOST", "").strip()
    if not host:
        host = _project_option("custom_stock_host")

    if not host:
        say("")
        say("  These come off a device running the FACTORY firmware, not this one.")
        say("  Supply its address and build again, either way:")
        say("")
        say("    powershell:  $env:SMOLTV_STOCK_HOST='<ip>'; pio run -t buildfs")
        say("    bash:        SMOLTV_STOCK_HOST=<ip> pio run -t buildfs")
        say("")
        say("  Or set it once in platformio.ini under [env:smalltv_pro]:")
        say("    custom_stock_host = <ip>")
        say("")
        say("  Or capture them yourself, after which no address is needed again:")
        say(f"    {_py()} scripts/device_fs.py backup --host <ip> "
            f"--output assets/cache/stock")
        return 1

    say(f"  using stock host {host}")

    # --only, never a full backup: it skips ~2.2 MB of gallery artwork and chart caches that
    # nothing in LAYOUT asks for. Credentials are handled independently of this - device_fs
    # refuses to capture them at all (NEVER_TRANSFER), so a build cannot pick them up even by
    # accident.
    wanted = _pack_fs().stock_device_paths(web_ui(), art())
    rc = run([_py(), str(SCRIPTS / "device_fs.py"), "backup",
              "--host", host, "--output", str(STOCK), "--only", *wanted])
    if rc != 0:
        return rc

    still = stock_missing()
    if still:
        # A successful backup that lacks what we need is worse than a failed one, because
        # the cache now exists and later builds would consider the step satisfied.
        say(f"\n  Backup succeeded but {len(still)} required file(s) are still absent:")
        for m in still:
            say(f"    {m}")
        say("  Is that device running the stock firmware?")
        return 1
    return 0


def step_pack() -> int:
    # Pass the choice explicitly: pack_fs would read $SMOLTV_WEB_UI itself, but not
    # custom_web_ui from platformio.ini, which only this process can see.
    return run([_py(), str(SCRIPTS / "pack_fs.py"),
                "--web", web_ui(), "--art", art()])


def main() -> int:
    if os.environ.get("SMOLTV_SKIP_PREBUILD"):
        say("== prebuild skipped (SMOLTV_SKIP_PREBUILD) ==")
        return 0
    stages = [("assets", step_assets)]
    if want_filesystem():
        stages += [("capture", step_stock), ("pack", step_pack)]
    else:
        say("== prebuild: firmware only -- data/ not needed, skipping capture and pack ==")
    for title, fn in stages:
        say(f"== prebuild: {title} ==")
        rc = fn()
        if rc != 0:
            say(f"== prebuild FAILED at '{title}' ==")
            return rc
    return 0


# SCons exec()s this file, so there is no __main__ guard to rely on and no return value it
# would look at. Raise on failure instead -- a silent non-zero would let the build continue
# and flash a stale or incomplete data/.
if __name__ == "__main__":
    sys.exit(main())
elif "SCons.Script" in sys.modules:
    if main() != 0:
        raise SystemExit("prebuild failed -- see the messages above")
