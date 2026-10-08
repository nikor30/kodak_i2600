#!/usr/bin/env python3
"""Cut the vendor driver's power-up initialisation out of a usbmon capture.

  extract_powerup.py CAPTURE.pcap OUTDIR [--t-start S --t-end S]

Writes OUTDIR/powerup.json (the request list) and OUTDIR/powerup.bin (large payloads:
the scanner's firmware and FPGA image as the vendor driver sent them). **These files
contain vendor firmware: keep them local, never commit or redistribute them.**

Without --t-start/--t-end the window is found automatically: from the first GetStatus
that reports the boot firmware (id 1) before a FirmwareDownload request to the SetLamp-off
that ends the vendor's open.

Left out on purpose: the NVRam write (40 35) and the LCD message upload (40 62), which the
vendor driver also sends but which write the scanner's permanent storage.
"""
import argparse
import json
import os
import struct

from usbmon_decode import transfers

SKIP_OUT = {0x35, 0x62}

ap = argparse.ArgumentParser()
ap.add_argument("capture")
ap.add_argument("outdir")
ap.add_argument("--t-start", type=float)
ap.add_argument("--t-end", type=float)
args = ap.parse_args()

xs = [x for x in transfers(args.capture)
      if (x["xfer"] == 2 and x["setup"] and x["setup"][0] & 0x60) or (x["xfer"] == 3 and x["dir"] == "OUT")]
c0 = min(x["t0"] for x in xs)
if args.t_start is None:
    first_dl = next(i for i, x in enumerate(xs) if x["xfer"] == 2 and x["setup"][:2] == b"\x40\x21")
    i = first_dl
    while i > 0 and xs[i - 1]["xfer"] == 2 and xs[i - 1]["setup"][:2] == b"\xc0\x00" and xs[i - 1]["data"][:1] == b"\x01" \
            and xs[first_dl]["t"] - xs[i - 1]["t"] < 5:
        i -= 1
    j = max(n for n, x in enumerate(xs) if x["xfer"] == 2 and x["setup"][:4] == b"\x40\x11\x00\x00"
            and x["t"] - xs[first_dl]["t"] < 60 and n > first_dl)
    xs = xs[i:j + 1]
else:
    xs = [x for x in xs if args.t_start <= x["t"] - c0 <= args.t_end]

os.makedirs(args.outdir, exist_ok=True)
blob = bytearray()
steps, prev, skipped = [], None, 0
for x in xs:
    gap = 0 if prev is None else max(round(x["t0"] - prev, 3), 0)
    prev = x["t"]
    gap = gap if gap > 0.02 else 0
    if x["xfer"] == 3:
        steps.append({"bulk": x["ep"], "blob": [len(blob), len(x["data"])], "gap": gap})
        blob += x["data"]
        continue
    bm, req, val, idx, ln = struct.unpack("<BBHHH", x["setup"])
    if bm == 0x40 and req in SKIP_OUT:
        skipped += 1
        continue
    step = {"bm": bm, "req": req, "val": val, "idx": idx, "len": ln, "gap": gap}
    if bm == 0x40 and len(x["data"]) >= 256:
        step["blob"] = [len(blob), len(x["data"])]
        blob += x["data"]
    else:
        step["data"] = x["data"].hex()
    steps.append(step)
with open(os.path.join(args.outdir, "powerup.bin"), "wb") as f:
    f.write(blob)
with open(os.path.join(args.outdir, "powerup.json"), "w") as f:
    f.write('{"note": "Power-up initialisation as sent by the vendor driver. Contains vendor firmware in powerup.bin: local use only.",\n "steps": [\n')
    f.write(",\n".join("  " + json.dumps(s) for s in steps))
    f.write("\n ]}\n")
dur = xs[-1]["t"] - xs[0]["t0"]
print(f"{len(steps)} steps ({skipped} permanent-storage writes left out), {len(blob)} blob bytes, {dur:.1f} s -> {args.outdir}")
