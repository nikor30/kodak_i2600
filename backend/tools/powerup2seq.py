#!/usr/bin/env python3
"""Convert a local power-up file (powerup.json from tools/usbcap/extract_powerup.py) to the text
format the C backend reads (docs/protocol/commands.md section 6). powerup.bin is used as it is
and must stay beside the output file under the same name (powerup.seq + powerup.bin).

  powerup2seq.py /etc/kodak-scan/firmware/powerup.json /etc/kodak-scan/firmware/powerup.seq

The output contains parts of the vendor firmware: keep it local, never commit or share it.
"""
import json
import os
import sys

src, dst = sys.argv[1:3]
steps = json.load(open(src))["steps"]
fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
with os.fdopen(fd, "w") as f:
    f.write("# Power-up sequence for the kodak_i2x00 backend. Contains vendor firmware data: local use only.\n")
    for st in steps:
        gap = f"{st['gap']:g}"
        if "bulk" in st:
            if st["bulk"] != 2:
                sys.exit(f"{src}: bulk transfer on unexpected endpoint {st['bulk']}")
            f.write(f"bulk {st['blob'][0]} {st['blob'][1]} {gap}\n")
            continue
        head = f"{st['req']:02x} {st['val']:04x} {st['idx']:04x}"
        if st["bm"] == 0x40 and "blob" in st:
            f.write(f"outblob {head} {st['blob'][0]} {st['blob'][1]} {gap}\n")
        elif st["bm"] == 0x40:
            f.write(f"out {head} {st['data'] or '-'} {gap}\n")
        elif st["bm"] == 0xC0 and st["req"] == 0x00:      # GetStatus = wait for this firmware id
            f.write(f"wait {bytes.fromhex(st['data'])[0]} {gap}\n")
        elif st["bm"] == 0xC0:
            f.write(f"in {head} {st['len']} {gap}\n")
        else:
            sys.exit(f"{src}: unexpected bmRequestType {st['bm']:#x}")
print(f"{len(steps)} steps -> {dst}")
