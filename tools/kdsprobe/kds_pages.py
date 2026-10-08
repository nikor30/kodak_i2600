#!/usr/bin/env python3
"""Raw native scan (front.raw / rear.raw from native_scan.py) -> page JPEGs and one PDF.

  kds_pages.py SCANDIR [--keep-blank] [--quality 85] [--title TEXT] [--outbox DIR]

Writes SCANDIR/pages/page-NNNN.jpg and SCANDIR/scan.pdf. With --outbox the PDF and its
metadata are also placed in the scan station's upload spool (same files kodak-scand
writes), so the running uploader sends it to Paperless.
"""
import argparse
import datetime as dt
import io
import json
import multiprocessing
import os
import pathlib
import shutil
import sys
import time
import uuid

import img2pdf

import kds_image as ki


def work(job):
    """Pool worker: one raw page -> (JPEG bytes or None, info, seconds)."""
    raw, blank_below, keep_blank, quality = job
    t = time.time()
    im, info = ki.process_page(raw, blank_below, keep_blank)
    jpeg = None
    if im is not None:
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=quality, dpi=(ki.DPI, ki.DPI))
        jpeg = buf.getvalue()
    return jpeg, info, time.time() - t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scandir", type=pathlib.Path)
    ap.add_argument("--keep-blank", action="store_true")
    ap.add_argument("--blank-below", type=float, default=0.0005, help="content fraction under which a side is blank")
    ap.add_argument("--quality", type=int, default=85)
    ap.add_argument("--title")
    ap.add_argument("--outbox", type=pathlib.Path)
    args = ap.parse_args()

    t0 = time.time()
    sides = {}
    for side in ("front", "rear"):
        f = args.scandir / f"{side}.raw"
        sides[side] = ki.split_pages(f.read_bytes()) if f.exists() else []
    pages_dir = args.scandir / "pages"
    shutil.rmtree(pages_dir, ignore_errors=True)
    pages_dir.mkdir()
    files = []
    order = ki.interleave(sides["front"], sides["rear"])
    with multiprocessing.Pool(min(3, len(order) or 1)) as pool:
        results = pool.imap(work, [(raw, args.blank_below, args.keep_blank, args.quality) for _, _, raw in order])
        for (number, side, raw), (jpeg, info, secs) in zip(order, results):
            note = "blank, dropped" if info.get("blank") and jpeg is None else "no sheet found" if jpeg is None else ""
            if jpeg is not None and not note:
                path = pages_dir / f"page-{len(files) + 1:04d}.jpg"
                path.write_bytes(jpeg)
                files.append(path)
            print(f"image {number} {side:5}: raw {raw.shape[1]}x{raw.shape[0]} {info} {note} ({secs:.1f} s)")
    if not files:
        sys.exit("no pages")
    pdf = args.scandir / "scan.pdf"
    pdf.write_bytes(img2pdf.convert([str(p) for p in files]))
    print(f"{len(files)} pages -> {pdf} ({pdf.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.1f} s")

    if args.outbox:
        created = dt.datetime.now().replace(microsecond=0)
        job = created.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
        meta = {"title": args.title or f"Scan {created:%Y-%m-%d %H:%M} (native)", "created": created.isoformat(),
                "tags": [], "pages": len(files)}
        owner = os.stat(args.outbox)
        tmp = args.outbox / f".{job}.pdf.tmp"
        shutil.copyfile(pdf, tmp)
        (args.outbox / f"{job}.json").write_text(json.dumps(meta))
        for p in (tmp, args.outbox / f"{job}.json"):
            os.chown(p, owner.st_uid, owner.st_gid)
        tmp.rename(args.outbox / f"{job}.pdf")      # the PDF appears last: the uploader keys on it
        print(f"queued for upload as {job} ({meta['title']})")


if __name__ == "__main__":
    main()
