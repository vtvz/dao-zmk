#!/usr/bin/env python3
"""
Reads per-half battery levels from the dao_dongle's vendor USB HID interface
and writes them to a small JSON state file that the Plasma widget polls.

The dongle (config/src/battery_hid.c) publishes a 3-byte input report on a
vendor-defined HID interface (Usage Page 0xFF00):

    byte 0: report id (0x01)
    byte 1: left  state of charge (0..100, 0xFF = unknown)
    byte 2: right state of charge (0..100, 0xFF = unknown)

The hidraw device NUMBER is not stable across replug/reflash, so we locate the
interface by its report-descriptor prefix (06 00 ff) rather than hardcoding it.
Reports only fire on change or at the firmware's report interval, so we block on
read() and keep publishing the last known values (with a connected flag) in
between. Designed to run as a user systemd service; no third-party deps.
"""

import glob
import json
import os
import struct
import sys
import time
from pathlib import Path

# The dongle's vendor interface: USB VID:PID + report-descriptor prefix.
# Both are needed — the 0x06 0x00 0xFF vendor prefix alone also matches other
# devices (e.g. a Logitech receiver), so we additionally require our VID:PID.
DONGLE_VID = 0x1D50
DONGLE_PID = 0x615E
VENDOR_DESC_PREFIX = bytes([0x06, 0x00, 0xFF])
REPORT_ID = 0x01
UNKNOWN = 0xFF

def default_state_path():
    # Fixed, uid-independent location so the Plasma widget can find it via ~.
    # Honours XDG_STATE_HOME, else ~/.local/state.
    base = os.environ.get(
        "XDG_STATE_HOME", os.path.join(Path.home(), ".local", "state")
    )
    return os.path.join(base, "dao-battery.json")


STATE_PATH = Path(os.environ.get("DAO_BATTERY_STATE", default_state_path()))
STATE_PATH.parent.mkdir(parents=True, exist_ok=True)


def hidraw_vid_pid(sys_path):
    """Return (vid, pid) for a hidraw sysfs dir, or None if unreadable.

    The device's uevent has a line like: HID_ID=0003:00001D50:0000615E
    (bus:vid:pid, hex).
    """
    uevent = Path(sys_path) / "device" / "uevent"
    try:
        for line in uevent.read_text().splitlines():
            if line.startswith("HID_ID="):
                _, vid_hex, pid_hex = line.split("=", 1)[1].split(":")
                return int(vid_hex, 16), int(pid_hex, 16)
    except (OSError, ValueError):
        pass
    return None


def find_battery_hidraw():
    """Return the /dev/hidrawN that is our dongle's vendor interface.

    Matches on BOTH our VID:PID and the vendor report-descriptor prefix, since
    the prefix alone collides with other vendor HID devices on the system.
    """
    for sys_path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        if hidraw_vid_pid(sys_path) != (DONGLE_VID, DONGLE_PID):
            continue
        desc = Path(sys_path) / "device" / "report_descriptor"
        try:
            head = desc.read_bytes()[: len(VENDOR_DESC_PREFIX)]
        except OSError:
            continue
        if head == VENDOR_DESC_PREFIX:
            return f"/dev/{os.path.basename(sys_path)}"
    return None


def write_state(left, right, connected):
    """Atomically write the current state so the widget never reads a half-write."""
    data = {
        "connected": connected,
        "left": None if left in (None, UNKNOWN) else left,
        "right": None if right in (None, UNKNOWN) else right,
        "updated": int(time.time()),
    }
    tmp = STATE_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(data))
    tmp.replace(STATE_PATH)


def main():
    left = right = None
    write_state(left, right, connected=False)

    while True:
        dev = find_battery_hidraw()
        if dev is None:
            # Dongle unplugged; mark disconnected and wait for it to reappear.
            write_state(left, right, connected=False)
            time.sleep(3)
            continue

        try:
            with open(dev, "rb", buffering=0) as f:
                write_state(left, right, connected=True)
                while True:
                    report = f.read(3)
                    if len(report) < 3:
                        # Short read / device went away.
                        break
                    rid, l, r = struct.unpack("BBB", report)
                    if rid != REPORT_ID:
                        continue
                    left, right = l, r
                    write_state(left, right, connected=True)
        except OSError as e:
            # Permission denied (missing udev rule / group) or unplug mid-read.
            print(f"dao-battery-reader: {dev}: {e}", file=sys.stderr)
            write_state(left, right, connected=False)
            time.sleep(3)


if __name__ == "__main__":
    main()
