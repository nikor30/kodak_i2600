#!/usr/bin/env python3
"""Capture full-payload USB traffic of one device from the kernel's binary usbmon
interface (/dev/usbmonN) into a pcap file (DLT_USB_LINUX, readable by Wireshark).

No dependencies. Needs root and `modprobe usbmon`.

  usbmon_capture.py -o out.pcap [--bus 1] [--vidpid 040a:601d] [--seconds N]

Stops on SIGINT/SIGTERM or after --seconds. With --vidpid the device number is
looked up in sysfs at start and re-checked when a new device number shows up
(the scanner keeps its address across the firmware load, F-030).
"""
import argparse
import fcntl
import glob
import os
import select
import signal
import struct
import sys
import time

HDR = 48                      # struct mon_bin_hdr as returned by read(2) (API 0)
DLT_USB_LINUX = 189
MON_IOCT_RING_SIZE = 0x9204   # _IO('\x92', 4)
RING = 1200 * 1024            # kernel caps captured data per URB at ring/5


def devnums(bus, vidpid):
    vid, pid = vidpid.lower().split(":")
    out = set()
    for d in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        base = os.path.dirname(d)
        try:
            if (open(d).read().strip() == vid
                    and open(base + "/idProduct").read().strip() == pid
                    and int(open(base + "/busnum").read()) == bus):
                out.add(int(open(base + "/devnum").read()))
        except OSError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--bus", type=int, default=1)
    ap.add_argument("--vidpid", default="040a:601d")
    ap.add_argument("--all", action="store_true", help="keep every device on the bus")
    ap.add_argument("--seconds", type=float, default=0)
    args = ap.parse_args()

    fd = os.open(f"/dev/usbmon{args.bus}", os.O_RDONLY)
    try:
        fcntl.ioctl(fd, MON_IOCT_RING_SIZE, RING)
    except OSError as e:
        print(f"ring size not changed: {e}", file=sys.stderr)

    stop = []
    signal.signal(signal.SIGINT, lambda *a: stop.append(1))
    signal.signal(signal.SIGTERM, lambda *a: stop.append(1))

    keep = set() if args.all else devnums(args.bus, args.vidpid)
    skip = set()
    n = nbytes = 0
    end = time.time() + args.seconds if args.seconds else None
    with open(args.out, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 0x40000, DLT_USB_LINUX))
        while not stop and (end is None or time.time() < end):
            if not select.select([fd], [], [], 0.25)[0]:
                continue
            ev = os.read(fd, HDR + RING)
            if len(ev) < HDR:
                continue
            dev = ev[11]
            if not args.all and dev not in keep:
                if dev in skip:
                    continue
                keep = devnums(args.bus, args.vidpid)
                if dev not in keep:
                    skip.add(dev)
                    continue
            ts_sec, ts_usec = struct.unpack_from("<qi", ev, 16)
            f.write(struct.pack("<IIII", ts_sec & 0xFFFFFFFF, ts_usec, len(ev), len(ev)))
            f.write(ev)
            n += 1
            nbytes += len(ev) - HDR
    print(f"{n} events, {nbytes} payload bytes -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
