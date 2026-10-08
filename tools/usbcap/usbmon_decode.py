#!/usr/bin/env python3
"""Turn a usbmon pcap (DLT_USB_LINUX, from usbmon_capture.py) into a transfer log.

One line per completed transfer: time, endpoint, direction, length, payload.
Submissions and completions are paired by URB id, so OUT payloads (carried by
the submission) and IN payloads (carried by the completion) both show up.

  usbmon_decode.py file.pcap [--max 64] [--ascii] [--no-std] [--summary]
"""
import argparse
import collections
import struct
import sys

XFER = {0: "ISO", 1: "INT", 2: "CTL", 3: "BLK"}


def events(path):
    with open(path, "rb") as f:
        gh = f.read(24)
        magic, _, _, _, _, _, link = struct.unpack("<IHHiIII", gh)
        if magic != 0xA1B2C3D4 or link not in (189, 220):
            sys.exit(f"{path}: not a usbmon pcap (linktype {link})")
        hdr = 48 if link == 189 else 64
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            _, _, caplen, _ = struct.unpack("<IIII", ph)
            ev = f.read(caplen)
            (urb, typ, xfer, ep, dev, bus, flag_setup, flag_data,
             sec, usec, status, length, len_cap) = struct.unpack_from("<QBBBBHbbqiiII", ev, 0)
            yield dict(urb=urb, typ=chr(typ), xfer=xfer, ep=ep, dev=dev,
                       t=sec + usec / 1e6, status=status, length=length,
                       setup=ev[40:48] if flag_setup == 0 else None,
                       data=ev[hdr:hdr + len_cap])


def transfers(path):
    """Yield completed transfers (dict) in completion order."""
    pending = {}
    for e in events(path):
        if e["typ"] == "S":
            pending[e["urb"]] = e
            continue
        s = pending.pop(e["urb"], None)
        ep_in = bool(e["ep"] & 0x80)
        setup = s["setup"] if s else None
        if e["xfer"] == 2 and setup:
            ep_in = bool(setup[0] & 0x80)
        data = e["data"] if ep_in else (s["data"] if s else b"")
        yield dict(t=e["t"], t0=s["t"] if s else e["t"], xfer=e["xfer"], ep=e["ep"] & 0x7F,
                   dir="IN" if ep_in else "OUT", status=e["status"],
                   length=e["length"] if ep_in else (s["length"] if s else e["length"]),
                   setup=setup, data=data)


def fmt(data, maxlen, ascii_):
    shown = data[:maxlen] if maxlen else data
    s = shown.hex(" ", 4)
    if ascii_:
        s += "  |" + "".join(chr(b) if 32 <= b < 127 else "." for b in shown) + "|"
    if maxlen and len(data) > maxlen:
        s += f" …(+{len(data) - maxlen})"
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--max", type=int, default=64, help="payload bytes shown (0 = all)")
    ap.add_argument("--ascii", action="store_true")
    ap.add_argument("--no-std", action="store_true", help="hide standard (chapter 9) control requests")
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()

    t_first = None
    count = collections.Counter()
    size = collections.Counter()
    for x in transfers(args.pcap):
        t_first = x["t0"] if t_first is None else t_first
        key = f'{XFER[x["xfer"]]} ep{x["ep"]} {x["dir"]}'
        if x["xfer"] == 2 and x["setup"]:
            bm, req, val, idx, ln = struct.unpack("<BBHHH", x["setup"])
            if args.no_std and (bm & 0x60) == 0:
                continue
            key = f"CTL {bm:02x} {req:02x}"
            head = f"CTL {bm:02x} {req:02x} v={val:04x} i={idx:04x} l={ln:<5}"
        else:
            head = f'{key:<14}'
        count[key] += 1
        size[key] += len(x["data"])
        if args.summary:
            continue
        st = "" if x["status"] == 0 else f' st={x["status"]}'
        print(f'{x["t"] - t_first:10.4f} {head} {x["length"]:>7}{st}  {fmt(x["data"], args.max, args.ascii)}')
    if args.summary:
        for k in sorted(count):
            print(f"{k:<16} {count[k]:>7} transfers {size[k]:>11} bytes")


if __name__ == "__main__":
    main()
