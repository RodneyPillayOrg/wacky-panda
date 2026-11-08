#!/usr/bin/env python3
# /// script
# requires-python = ">=3.13"
# dependencies = [
#     "psutil>=7.2.2",
# ]
# ///
"""Simulate the AIDA64 RemoteSensor companion that AppMonitor polls.

    uv run scripts/basic_pc_monitor.py                  # real stats, listen on 0.0.0.0:8000
    uv run scripts/basic_pc_monitor.py --port 8080 -v
    uv run scripts/basic_pc_monitor.py --demo            # synthetic oscillating values instead

Point the device's Monitor tab "PC IP address" at "<this-machine-ip>:<port>" (a
colon-suffixed port is supported by AppMonitor::fetchMonitor - no colon means
port 80, which usually needs elevated privileges to bind on this machine, so
this defaults to 8000 instead).

Protocol, reverse-engineered from src/apps/AppMonitor.h (NOT the real AIDA64
RemoteSensor wire format - this only reproduces what our firmware's fetchMonitor
actually does):

  1. Device opens a raw TCP connection and writes a hand-built request:
       GET /sse HTTP/1.1
       Content-Type:application/json;charset=utf-8
       Host: <ip[:port]>
       User-Agent:SmolTV-Pro
       Connection: close
  2. It scans the response for a literal "\\n\\n" (WiFiClient::find) to skip
     headers - a CRLF header block ("\\r\\n\\r\\n") never contains that, since a
     "\\r" always sits between the two "\\n"s. The header block below therefore
     uses BARE "\\n" line endings, deliberately, to satisfy that scan.
  3. It then reads exactly ONE line as the payload and parses substrings of the
     form KEY:value<terminator> out of it (see PAYLOAD_KEYS below) - order and
     surrounding text do not matter, only that each KEY: is followed eventually
     by its terminator.

Default mode (no --demo) reads REAL stats from this machine via psutil, so this
doubles as a lightweight AIDA64 RemoteSensor substitute - no application install
needed. Two fields have no honest cross-platform source and are always 0 rather
than faked - see read_real() below.
"""
import argparse
import os
import socket
import threading
import time

import psutil

DEG_C = chr(0x2103)  # the exact terminator AppMonitor::parsePayload searches for

DRIVE_TOTAL_GB = 512.0  # --demo only: fake C:\ capacity: used/free derive from this + drive_usage


def read_real() -> dict:
    """Real host stats via psutil - the same calls work on Windows/Linux/macOS.

    cpu_temp and cpu_power are left at 0, not estimated: psutil.sensors_temperatures()
    is Linux-only (raises AttributeError elsewhere), and CPU power draw has no portable
    API at all (RAPL, Intel Power Gadget, vendor WMI providers are all platform-specific).
    Reporting 0 honestly is better than a plausible-looking number nobody asked us to invent.
    """
    cpu_usage = psutil.cpu_percent(interval=None)
    freq = psutil.cpu_freq()
    cpu_freq = int(freq.current) if freq else 0
    ram_usage = psutil.virtual_memory().percent

    root = os.path.abspath(os.sep)  # "C:\\" on Windows, "/" on POSIX
    disk = psutil.disk_usage(root)
    drive_usage = disk.percent
    drive_used = disk.used / (1024 ** 3)
    drive_free = disk.free / (1024 ** 3)

    cpu_temp = 0.0
    sensors_temperatures = getattr(psutil, "sensors_temperatures", None)
    if sensors_temperatures is not None:  # not defined at all on Windows/macOS
        for entries in sensors_temperatures().values():
            if entries:
                cpu_temp = entries[0].current
                break

    return dict(
        cpu_usage=cpu_usage, cpu_freq=cpu_freq, cpu_temp=cpu_temp, cpu_power=0.0,
        ram_usage=ram_usage, drive_usage=drive_usage,
        drive_used=drive_used, drive_free=drive_free,
    )


def read_demo(t: float) -> dict:
    """Synthetic oscillating values, for exercising bars/graphs with no real host to poll."""
    cpu_usage = 30 + 25 * abs((t % 20) - 10) / 10
    cpu_freq = 3600 + int(200 * ((t % 7) - 3))
    cpu_temp = 45 + 10 * abs((t % 13) - 6) / 6
    cpu_power = 35 + 20 * abs((t % 11) - 5) / 5
    ram_usage = 40 + 15 * abs((t % 17) - 8) / 8
    drive_usage = 60 + int(10 * abs((t % 23) - 11) / 11)
    # Tied to drive_usage, not independently oscillated, so used+free always sums to the
    # same fake total and tracks the UP_DRIVE_C% bar instead of drifting apart from it.
    drive_used = DRIVE_TOTAL_GB * drive_usage / 100
    drive_free = DRIVE_TOTAL_GB - drive_used

    return dict(
        cpu_usage=cpu_usage, cpu_freq=cpu_freq, cpu_temp=cpu_temp, cpu_power=cpu_power,
        ram_usage=ram_usage, drive_usage=drive_usage,
        drive_used=drive_used, drive_free=drive_free,
    )


def build_payload(stats: dict) -> str:
    time_str = time.strftime("%H:%M:%S")
    # Only the fields AppMonitor::parsePayload actually reads are needed; the
    # comment block in AppMonitor.h documents several more (S_FAN, UP_GPU,
    # U_RAM, GPU1/GPU2) that the firmware never parses, so they are omitted.
    return (
        f"F_CPU:{stats['cpu_freq']}MHz "
        f"UP_CPU:{stats['cpu_usage']:.1f}% "
        f"T_CPU:{stats['cpu_temp']:.1f}{DEG_C} "
        f"PW_CPU:{stats['cpu_power']:.1f}W "
        f"UP_RAM:{stats['ram_usage']:.1f}% "
        f"UP_DRIVE_C:{stats['drive_usage']:.0f}% "
        f"FR_DRIVE_C:{stats['drive_free']:.1f}GB "
        f"U_DRIVE_C:{stats['drive_used']:.1f}GB "
        f"TIME:{time_str}{{"
    )


def handle(conn: socket.socket, addr, verbose: bool, demo: bool) -> None:
    conn.settimeout(2.0)
    try:
        try:
            request = conn.recv(4096)
        except socket.timeout:
            request = b""
        if verbose:
            print(f"[{addr[0]}:{addr[1]}] request: {request!r}")

        stats = read_demo(time.time()) if demo else read_real()
        payload = build_payload(stats)
        response = (
            "HTTP/1.1 200 OK\n"
            "Content-Type: application/json;charset=utf-8\n"
            "Connection: close\n"
            "\n"
            f"{payload}\n"
        )
        conn.sendall(response.encode("utf-8"))
        if verbose:
            # backslashreplace: the console codepage (e.g. Windows cp1252) can't print
            # DEG_C directly, and this is a log line, not the wire payload - ASCII is fine.
            safe = payload.encode("ascii", "backslashreplace").decode("ascii")
            print(f"[{addr[0]}:{addr[1]}] sent: {safe}")
    except OSError as e:
        if verbose:
            print(f"[{addr[0]}:{addr[1]}] error: {e}")
    finally:
        conn.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=8000, help="listen port (default 8000)")
    ap.add_argument("--demo", action="store_true",
                     help="synthetic oscillating values instead of real host stats")
    ap.add_argument("-v", "--verbose", action="store_true", help="log each request/response")
    args = ap.parse_args()

    if not args.demo:
        psutil.cpu_percent(interval=None)  # warm-up: the first real call always returns 0.0

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(5)
    mode = "DEMO (synthetic)" if args.demo else "REAL (this machine's stats)"
    print(f"Monitor simulator listening on {args.host}:{args.port} [{mode}]")
    print(f"Set the device's Monitor tab PC IP to this machine's address, port {args.port}")

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle, args=(conn, addr, args.verbose, args.demo),
                              daemon=True).start()
    except KeyboardInterrupt:
        print("stopping")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
