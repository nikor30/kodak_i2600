#!/usr/bin/env python3
"""Probe for the Kodak i2600 without the vendor driver (whitelisted requests only).

  kdsprobe.py info                  status, firmware versions, serial, meters
  kdsprobe.py watch [--seconds N]   print status changes and interrupt events
                                    (press buttons, load/remove paper, open the cover)
  kdsprobe.py lcd TEXT [--label N] [--size 12] [--dry-run]
                                    replace the LCD message "Rescan documents" (type 4, id 1; the
                                    vendor driver restores it on its next open)

Stop kodak-scand/kodak-saned first: `watch` claims the USB interface.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pi", "scan-station"))
import kds_usb as k


def show_status(st, prev=None):
    changed = [f for f in k.STATUS_FIELDS if f != "tick" and (prev is None or st[f] != prev[f])]
    return " ".join(f"{f}={st[f]:#x}" if isinstance(st[f], int) else f"{f}={st[f]}" for f in changed)


def cmd_info(dev, args):
    raw = dev.get(k.GET_STATUS, 32)
    print("status  ", raw.hex(" ", 4))
    print("        ", show_status(k.parse_status(raw)))
    print("versions", dev.get(k.GET_FW_VERSIONS, 56).hex(" ", 4))
    print("serial  ", dev.get(k.SERIAL_NUMBER, 16).decode("ascii", "replace"))
    print("meters  ", dev.get(k.METERS, 20).hex(" ", 4))
    print("eol     ", dev.get(k.EOL_CONFIGURATION, 55).hex(" ", 4))


def cmd_watch(dev, args):
    t0 = time.time()
    prev = None
    next_status = 0
    dev.claim()
    dev.events(True)
    print("watching (events enabled); times in seconds", flush=True)
    try:
        watch_loop(dev, args, t0, prev, next_status)
    finally:
        dev.events(False)


def watch_loop(dev, args, t0, prev, next_status):
    while not args.seconds or time.time() - t0 < args.seconds:
        for ep in (k.EP_INT_A, k.EP_INT_B):
            ev = dev.read_interrupt(ep, timeout=100)
            if ev:
                name = k.EVENT_NAMES.get(ev[0], "?")
                print(f"{time.time() - t0:8.2f} event ep{ep & 15} {ev.hex(' ')}  {name}", flush=True)
        if time.time() >= next_status:
            next_status = time.time() + 0.5
            raw = dev.get(k.GET_STATUS, 32)
            st = k.parse_status(raw)
            if prev is None or any(st[f] != prev[f] for f in k.STATUS_FIELDS if f != "tick"):
                print(f"{time.time() - t0:8.2f} status   {raw[12:].hex(' ')}  {show_status(st, prev)}", flush=True)
            prev = st


def cmd_lcd(dev, args):
    bitmap = k.lcd_text(args.text, args.size)
    for row in k.lcd_rows(bitmap):
        if row.strip():
            print("|" + row + "|")
    if args.dry_run:
        return
    msg = (k.LCD_TYPE_LABEL, args.label) if args.label else k.LCD_MSG_DISCONNECTED
    dev.lcd_populate(bitmap, msg)
    print(f"sent as LCD message type {msg[0]}, id {msg[1]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    w = sub.add_parser("watch")
    w.add_argument("--seconds", type=float, default=0)
    l = sub.add_parser("lcd")
    l.add_argument("text")
    l.add_argument("--size", type=int, default=12)
    l.add_argument("--label", type=int, help="write the label of this function number (1..9) instead")
    l.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    try:
        with k.Device() as dev:
            {"info": cmd_info, "watch": cmd_watch, "lcd": cmd_lcd}[args.cmd](dev, args)
    except KeyboardInterrupt:
        pass
    except k.UsbError as e:
        sys.exit(f"kdsprobe: {e}")


if __name__ == "__main__":
    main()
