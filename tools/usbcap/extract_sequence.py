#!/usr/bin/env python3
"""Cut the vendor control requests of a time window out of a usbmon pcap into a small
JSON file that the native driver can replay (pi/scan-station/sequences/).

  extract_sequence.py CAPTURE.pcap T_START T_END OUT.json --note "color 300 dpi duplex"

Each step: [bmRequestType, bRequest, wValue, wIndex, wLength, data hex (OUT) or expected
reply hex (IN), pause in seconds before the step]. Times are seconds from the first
packet, as printed by usbmon_decode.py.
"""
import argparse
import json
import struct

from usbmon_decode import transfers

ap = argparse.ArgumentParser()
ap.add_argument("capture")
ap.add_argument("t_start", type=float)
ap.add_argument("t_end", type=float)
ap.add_argument("out")
ap.add_argument("--note", default="")
args = ap.parse_args()

xs = list(transfers(args.capture))
c0 = xs[0]["t0"]
steps, prev = [], None
for x in xs:
    if not (args.t_start <= x["t"] - c0 <= args.t_end and x["xfer"] == 2 and x["setup"] and x["setup"][0] & 0x60):
        continue
    bm, req, val, idx, ln = struct.unpack("<BBHHH", x["setup"])
    gap = 0 if prev is None else max(round((x["t0"] - c0) - prev, 3), 0)
    prev = x["t"] - c0
    steps.append([bm, req, val, idx, ln, x["data"].hex(), gap if gap > 0.02 else 0])
with open(args.out, "w") as f:
    f.write('{"note": %s,\n "steps": [\n' % json.dumps(args.note))
    f.write(",\n".join("  " + json.dumps(s) for s in steps))
    f.write("\n ]}\n")
print(len(steps), "steps ->", args.out)
