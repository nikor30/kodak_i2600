#!/usr/bin/env python3
"""Convert a scan start sequence (pi/scan-station/sequences/*.json) to the text format the
C backend reads (docs/protocol/commands.md section 8, "Sequence file for the C backend").

  json2seq.py IN.json OUT.seq
"""
import json
import sys

src, dst = sys.argv[1:3]
seq = json.load(open(src))
with open(dst, "w") as f:
    f.write(f"# {seq.get('note', '')}\n# generated from {src.rsplit('/', 1)[-1]} by json2seq.py\n")
    for bm, req, val, idx, length, data, gap in seq["steps"]:
        if bm == 0x40:
            f.write(f"out {req:02x} {val:04x} {idx:04x} {data or '-'} {gap:g}\n")
        elif bm == 0xC0:
            f.write(f"in {req:02x} {val:04x} {idx:04x} {length} {gap:g}\n")
        else:
            sys.exit(f"{src}: unexpected bmRequestType {bm:#x}")
