#!/usr/bin/env python3
r"""Serial monitor and wiring diagnostic for the device, over USB.

    uv run scripts/device_serial.py monitor              # 15 s of log, then exit
    uv run scripts/device_serial.py monitor 60 --grep cg # 60 s, lines containing "cg"
    uv run scripts/device_serial.py monitor --no-reset   # attach without rebooting
    uv run scripts/device_serial.py diagnose             # why will it not flash?

The port defaults to $SMOLTV_SERIAL_PORT, then to the first USB-serial adapter found, so
in practice neither command needs arguments. Pass --port to be explicit.

NOT NAMED serial.py ON PURPOSE. A module of that name in this directory would shadow
pyserial for every script run from here, and `import serial` would import this file.

TWO THINGS THIS FILE EXISTS TO GET RIGHT
----------------------------------------
1. OPENING THE PORT MUST NOT ASSERT DTR/RTS. On the ESP32 auto-reset circuit those lines
   drive EN and GPIO0, so a default pyserial open holds the chip in reset or straps it into
   the ROM bootloader - silently. That is what once made "the serial line carries no data"
   look true when the wiring was fine.

2. ALL OUTPUT IS ASCII-FILTERED. A device that is mid-reset, or a wrong baud rate, emits
   arbitrary bytes. Those can corrupt a terminal, and they will corrupt an agent session
   that pastes the output back, so every byte printed here is run through CLEAN() first.
   Do not add a print path that bypasses it.
"""

import argparse
import os
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing - run this with uv:  uv run scripts/device_serial.py ...")

BAUD = 115200

# Printable ASCII plus tab/LF/CR. See the note above - this is not cosmetic.
_ALLOWED = re.compile(r"[^\x09\x0A\x0D\x20-\x7E]")


def clean(s: str) -> str:
    return _ALLOWED.sub("", s)


# Lines the ROM and the bootloader print on a genuine reset. Used by the diagnostic to tell
# "the chip rebooted" from "some bytes arrived".
BOOT_MARKERS = ("rst:0x", "ets ", "boot:0x", "setup start")


def pick_port(explicit: str = "") -> str:
    """Resolve the port: --port, then $SMOLTV_SERIAL_PORT, then autodetect."""
    if explicit:
        return explicit
    env = os.environ.get("SMOLTV_SERIAL_PORT", "").strip()
    if env:
        return env
    ports = list(list_ports.comports())
    if not ports:
        sys.exit("no serial ports found - check the USB cable and driver")

    # Rank USB-serial bridges ahead of built-in COM ports, then take the first that will
    # actually OPEN. A machine with more than one adapter often has one held by another
    # program, and "Access is denied" on a port you never chose is a confusing way to fail.
    ranked = []
    for want in ("CP210", "CH340", "FTDI", "USB"):
        ranked += [p for p in ports
                   if p not in ranked and want.lower() in f"{p.description} {p.hwid}".lower()]
    ranked += [p for p in ports if p not in ranked]

    for cand in ranked:
        try:
            open_port(cand.device).close()
            return cand.device
        except Exception:
            continue
    sys.exit("found %d serial port(s) but none would open: %s"
             % (len(ports), ", ".join(p.device for p in ports)))


def open_port(port: str, timeout: float = 0.3) -> "serial.Serial":
    """Open with DTR and RTS DE-ASSERTED. See point 1 in the module docstring."""
    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = timeout
    s.dtr = False
    s.rts = False
    s.open()
    return s


def reset_into_app(s: "serial.Serial") -> None:
    """Pulse EN via RTS with DTR de-asserted, so the chip restarts into the APP.

    Holding DTR as well is the bootloader strap - that is the diagnostic's job, not this.
    """
    s.dtr = False
    s.rts = True
    time.sleep(0.10)
    s.rts = False
    time.sleep(0.05)


def read_for(s: "serial.Serial", seconds: float):
    """Collect for N seconds. Returns (ascii_text, raw_byte_count)."""
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        buf += s.read(256)
    return clean(buf.decode("ascii", "ignore")), len(buf)


# -- monitor ---------------------------------------------------------------------------

def cmd_monitor(a) -> int:
    port = pick_port(a.port)
    try:
        s = open_port(port, timeout=0.2)
    except Exception as e:
        sys.exit(f"cannot open {port}: {clean(str(e))}")

    print(f"# {port} @ {BAUD}, {a.seconds:.0f}s"
          f"{'' if a.no_reset else ', resetting into the app'}", file=sys.stderr)
    if not a.no_reset:
        reset_into_app(s)

    buf = b""
    t0 = time.time()
    try:
        while time.time() - t0 < a.seconds:
            chunk = s.read(512)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = clean(raw.decode("ascii", "ignore")).rstrip()
                if not line:
                    continue
                if a.grep and a.grep not in line:
                    continue
                print(line)
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
    return 0


# -- diagnose --------------------------------------------------------------------------
#
# Tests each signal on its own, so a failure names the wire instead of leaving you with
# esptool's generic "No serial data received".

def _rule(title: str) -> None:
    print("\n" + "-" * 66)
    print(title)
    print("-" * 66)


def _slip(payload: bytes) -> bytes:
    """SLIP-frame an esptool packet."""
    out = b"\xc0"
    for b in payload:
        out += b"\xdb\xdd" if b == 0xDB else b"\xdb\xdc" if b == 0xC0 else bytes([b])
    return out + b"\xc0"


def _reset_test(port, label, key, drive, results) -> None:
    _rule(label)
    s = open_port(port)
    s.reset_input_buffer()
    drive(s)
    txt, n = read_for(s, 5)
    s.close()
    booted = any(m in txt for m in BOOT_MARKERS)
    results[key] = booted
    print("  bytes=%d  boot banner seen=%s" % (n, booted))
    for line in [l for l in txt.splitlines() if l.strip()][:5]:
        print("  | " + line)
    print("  => %s" % ("PASS - the chip rebooted" if booted else
                       "FAIL - that control wire is not reaching EN"))


def cmd_diagnose(a) -> int:
    port = pick_port(a.port)
    results = {}

    _rule("1) PORT -- adapter present and openable")
    print("  ports seen: " + (", ".join(sorted(p.device for p in list_ports.comports()))
                              or "none"))
    for p in list_ports.comports():
        if p.device == port:
            print("  %s = %s" % (p.device, clean(str(p.description))))
    try:
        open_port(port).close()
        results["PORT"] = True
    except Exception as e:
        print("  FAILED to open: %s: %s" % (type(e).__name__, clean(str(e))))
        print("\nCannot continue without the port. Check the USB cable / driver.")
        return 1

    _rule("2) RX -- device TX -> PC RX (listening 8s, no reset)")
    print("  The firmware prints a [heap] line every 10s, so give it a moment.")
    s = open_port(port)
    txt, n = read_for(s, 8)
    s.close()
    results["RX"] = n > 0
    print("  bytes=%d" % n)
    for line in [l for l in txt.splitlines() if l.strip()][:6]:
        print("  | " + line)
    print("  => %s" % ("PASS - we can hear the device" if n else
                       "FAIL - nothing received (check device TX -> adapter RX, and GND)"))

    _reset_test(port, "3) EN via RTS -- pulse RTS, expect a reboot banner", "EN_RTS",
                lambda s: (setattr(s, 'rts', True), time.sleep(0.15), setattr(s, 'rts', False)), results)
    _reset_test(port, "4) EN via DTR -- pulse DTR (in case DTR/RTS are swapped)", "EN_DTR",
                lambda s: (setattr(s, 'dtr', True), time.sleep(0.15), setattr(s, 'dtr', False)), results)
    _reset_test(port, "5) EN via BOTH -- assert both together", "EN_BOTH",
                lambda s: (setattr(s, 'dtr', True), setattr(s, 'rts', True), time.sleep(0.15),
                           setattr(s, 'dtr', False), setattr(s, 'rts', False)), results)

    _rule("6) TX + BOOT -- strap into the ROM bootloader, then send a real SYNC frame")
    print("  This is the only test that proves PC TX -> device RX works.")
    s = open_port(port)
    # The classic strap: GPIO0 low (DTR) held across an EN reset pulse (RTS).
    s.dtr = True
    s.rts = True
    time.sleep(0.12)
    s.rts = False
    time.sleep(0.35)
    s.dtr = False
    s.reset_input_buffer()
    # esptool SYNC (op 0x08): 0x07 0x07 0x12 0x20 followed by 32x 0x55.
    data = b"\x07\x07\x12\x20" + b"\x55" * 32
    pkt = b"\x00\x08" + len(data).to_bytes(2, "little") + (0).to_bytes(4, "little") + data
    got = b""
    for _ in range(6):
        s.write(_slip(pkt))
        s.flush()
        time.sleep(0.1)
        got += s.read(256)
        if got.count(b"\xc0") >= 2:
            break
    s.close()
    results["TX_BOOT"] = got.count(b"\xc0") >= 2
    print("  reply bytes=%d  SLIP frames=%d" % (len(got), max(0, got.count(b"\xc0") - 1)))
    print("  => %s" % ("PASS - the bootloader answered; flashing should work"
                       if results["TX_BOOT"] else
                       "FAIL - no answer (PC TX -> device RX missing, or GPIO0/EN not wired)"))

    _rule("SUMMARY")
    for key, desc in [("PORT",    "adapter opens"),
                      ("RX",      "device TX -> PC RX"),
                      ("EN_RTS",  "RTS -> EN (reset)"),
                      ("EN_DTR",  "DTR -> EN (reset)"),
                      ("EN_BOTH", "both -> EN (reset)"),
                      ("TX_BOOT", "PC TX -> device RX + bootloader strap")]:
        v = results.get(key)
        print("  %-8s %-40s %s"
              % (key, desc, "PASS" if v else "FAIL" if v is False else "skipped"))

    can_reset = any(results.get(k) for k in ("EN_RTS", "EN_DTR", "EN_BOTH"))
    print("\nVERDICT")
    if results.get("TX_BOOT"):
        print("  Wiring is good - a serial upload should work:")
        print("    pio run -t upload -e smalltv_pro --upload-port %s" % port)
    elif can_reset and results.get("RX"):
        print("  Reset works and we can hear the device, but the bootloader never answered")
        print("  -> suspect PC TX -> device RX (the adapter's TX pin), or GPIO0.")
    elif results.get("RX"):
        print("  We can hear the device but cannot reset it -> the EN, and probably GPIO0,")
        print("  control wires are not connected, so auto-flash cannot work.")
        print("  Workaround: hold GPIO0 to GND, briefly short EN to GND, release GPIO0,")
        print("  then flash with esptool --before no_reset.")
    else:
        print("  No data from the device at all -> check GND first, then device TX.")
    print()
    return 0 if results.get("TX_BOOT") else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("--port", default="",
                    help="serial port (default: $SMOLTV_SERIAL_PORT, else autodetect)")

    # Also on each subcommand, so --port works either side of it. SUPPRESS keeps the
    # subparser default from overwriting a value given before the subcommand.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", default=argparse.SUPPRESS,
                        help="serial port (default: $SMOLTV_SERIAL_PORT, else autodetect)")

    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("monitor", help="stream the device log, ASCII-filtered",
                       parents=[common])
    m.add_argument("seconds", nargs="?", type=float, default=15.0,
                   help="how long to capture (default 15)")
    m.add_argument("--grep", default="", help="only print lines containing this substring")
    m.add_argument("--no-reset", action="store_true",
                   help="attach without resetting, to watch a device already running")
    m.set_defaults(fn=cmd_monitor)

    d = sub.add_parser("diagnose", help="test each flashing signal independently",
                       parents=[common])
    d.set_defaults(fn=cmd_diagnose)

    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
