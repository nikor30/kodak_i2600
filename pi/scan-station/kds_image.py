"""Turn the scanner's raw image streams into finished pages (numpy + PIL only).

Raw format (docs/protocol/commands.md section 8): per side one stream of pages, each a
whole number of 7,740-byte RGB lines followed by a tagged trailer. The scanner sends
the full sensor width with black background around the sheet and uncorrected colour,
so this module does what the vendor's host software does: split, find the sheet,
deskew, crop, colour-correct, detect blank sides.

Calibration constants below come from one comparison with the vendor driver's output
of the same sheet (color 300 dpi, owner's unit, 2026-10-08); see docs/protocol/image-processing.md.
"""
import math
import re

import numpy as np
from PIL import Image

LINE_PX = 2580
LINE = LINE_PX * 3

# One raw pixel is 1/296.1 inch in both directions (an A4 sheet measures 2448 x 3464 raw
# pixels); the vendor resamples to exactly 300 dpi, we just label the output correctly.
DPI = 296.1

# Background stays below ~11 after 4x4 averaging, black print on paper is above ~20.
POOL = 4
PAPER_THRESHOLD = 14

# out = 255 * clip(M @ raw/255 + OFFSET) ** (1 / GAMMA)
GAMMA = 1.6
COLOR_MATRIX = np.array([[2.734, -1.091, -0.309],
                         [-0.294, 1.694, -0.036],
                         [-0.025, -0.110, 1.377]], dtype=np.float32)
COLOR_OFFSET = np.array([-0.037, -0.040, -0.041], dtype=np.float32)

_TRAILER = re.compile(rb"\x00\x01\x00\x00(..)\x02\xf5..\x01\xf6..\x01\xf9..\x01\xfa..\x01\xfc..\x01\xfd..\x01\xfe", re.S)


def split_pages(stream):
    """Raw side stream -> list of (image number, HxWx3 uint8 array)."""
    pages, pos = [], 0
    view = np.frombuffer(stream, dtype=np.uint8)
    for m in _TRAILER.finditer(stream):
        start = m.start()
        if start < pos or (start - pos) % LINE:
            continue                        # pixel data that happens to look like the tags
        end = None
        for k in range(256):
            q = m.end() + 2 * k
            if stream[q:q + 4] == bytes((0, k, 1, 0xFF)):
                end = q + 4
                break
        if end is None:
            continue
        lines = (start - pos) // LINE
        if lines:
            pages.append((int.from_bytes(m.group(1), "big"), view[pos:start].reshape(lines, LINE_PX, 3)))
        pos = end
    return pages


def _pool_mask(img):
    h, w = img.shape[0] // POOL * POOL, img.shape[1] // POOL * POOL
    m = img[:h, :w].max(axis=2).reshape(h // POOL, POOL, w // POOL, POOL).mean(axis=(1, 3), dtype=np.float32)
    return m > PAPER_THRESHOLD


def _fit_edge(pos, limit):
    """Robust line through edge positions pos[i] (NaN = no paper in that row/column).

    Positions on the image border are ignored (sheet runs out of view there).
    Returns (slope per index, number of inliers).
    """
    idx = np.where(~np.isnan(pos) & (pos > 0) & (pos < limit - 1))[0]
    if len(idx) < 40:
        return 0.0, 0
    idx = idx[len(idx) // 20: len(idx) - len(idx) // 20]     # corners are rounded or dog-eared
    y = pos[idx]
    lag = max(len(idx) // 4, 1)
    slope = float(np.median((y[lag:] - y[:-lag]) / (idx[lag:] - idx[:-lag])))
    icpt = float(np.median(y - slope * idx))
    inl = np.abs(y - (slope * idx + icpt)) < 2
    if inl.sum() < 20:
        return 0.0, 0
    slope, icpt = np.polyfit(idx[inl], y[inl], 1)
    return float(slope), int(inl.sum())


def _edges(mask):
    h, w = mask.shape
    cols, rows = mask.any(axis=0), mask.any(axis=1)
    top = np.where(cols, mask.argmax(axis=0), np.nan)
    bottom = np.where(cols, h - 1 - mask[::-1].argmax(axis=0), np.nan)
    left = np.where(rows, mask.argmax(axis=1), np.nan)
    right = np.where(rows, w - 1 - mask[:, ::-1].argmax(axis=1), np.nan)
    return top, bottom, left, right


def find_skew(mask):
    """Skew of the sheet in degrees (positive = sheet turned clockwise in the image)."""
    h, w = mask.shape
    top, bottom, left, right = _edges(mask)
    fits = [_fit_edge(top, h), _fit_edge(bottom, h)]
    fits += [(-s, n) for s, n in (_fit_edge(left, w), _fit_edge(right, w))]
    fits = sorted((s, n) for s, n in fits if n)
    if not fits:
        return 0.0
    total, acc = sum(n for _, n in fits), 0
    for s, n in fits:                       # weighted median
        acc += n
        if acc * 2 >= total:
            return math.degrees(math.atan(s))
    return 0.0


def _bounds(mask):
    """Box (top, bottom, left, right; pooled units, end exclusive) of the rows/columns that are mostly paper."""
    rows = np.where(mask.mean(axis=1) > 0.5)[0]
    cols = np.where(mask.mean(axis=0) > 0.5)[0]
    if not len(rows) or not len(cols):
        return None
    return rows[0], rows[-1] + 1, cols[0], cols[-1] + 1


def extract_sheet(img, min_skew=0.15, inset=3):
    """Raw page -> (deskewed, cropped HxWx3 array, info dict), or (None, info) if no sheet is found."""
    mask = _pool_mask(img)
    box = _bounds(mask)
    if box is None:
        return None, {"sheet": False}
    angle = find_skew(mask)
    info = {"sheet": True, "skew": round(angle, 2)}
    if abs(angle) >= min_skew:
        # cut out the tilted sheet with a margin, rotate only that
        t, b, l, r = (int(v) * POOL for v in _edges_box(mask))
        pad = 16
        sub = img[max(t - pad, 0):b + pad, max(l - pad, 0):r + pad]
        rot = Image.fromarray(sub).rotate(angle, resample=Image.BICUBIC, expand=True)
        img = np.asarray(rot)
        mask = _pool_mask(img)
        box = _bounds(mask)
        if box is None:
            return None, {"sheet": False}
    t, b, l, r = (int(v) * POOL for v in box)
    out = img[t + inset:b - inset, l + inset:r - inset]
    info["size"] = (out.shape[1], out.shape[0])
    return out, info


def _edges_box(mask):
    rows = np.where(mask.any(axis=1))[0]
    cols = np.where(mask.any(axis=0))[0]
    return rows[0], rows[-1] + 1, cols[0], cols[-1] + 1


_GAMMA_LUT = (255.0 * (np.arange(4097, dtype=np.float32) / 4096.0) ** (1.0 / GAMMA) + 0.5).astype(np.uint8)


def color_correct(img):
    """Raw sensor RGB -> display RGB (matrix + gamma), processed in strips to bound memory."""
    out = np.empty_like(img)
    m = (COLOR_MATRIX.T * (4096.0 / 255.0)).astype(np.float32)
    off = COLOR_OFFSET * 4096.0
    for y in range(0, img.shape[0], 256):
        lin = img[y:y + 256].astype(np.float32) @ m
        lin += off
        np.clip(lin, 0, 4096, out=lin)
        out[y:y + 256] = _GAMMA_LUT[lin.astype(np.uint16)]
    return out


def content_fraction(raw, box, drop=40, border=0.03):
    """Fraction of the sheet that is clearly darker than its paper.

    Measured on colour-corrected 4x4 averages of the raw page inside box (pooled units,
    from _bounds), before any deskewing, so blank sides cost almost nothing.
    Printed text pages give 4 % and more, empty backs 0.001 % or less.
    """
    t, b, l, r = (int(v) * POOL for v in box)
    sub = raw[t:b, l:r]
    small = sub.reshape(sub.shape[0] // POOL, POOL, sub.shape[1] // POOL, POOL, 3).mean(axis=(1, 3), dtype=np.float32)
    lum = color_correct(small.astype(np.uint8)).mean(axis=2, dtype=np.float32)
    by, bx = max(int(lum.shape[0] * border), 1), max(int(lum.shape[1] * border), 1)
    lum = lum[by:-by, bx:-bx]
    return float((lum < np.median(lum) - drop).mean())


def process_page(raw, blank_below=0.0005, keep_blank=False):
    """Raw page array -> (PIL image or None, info).

    info['blank'] tells whether the side is empty; blank sides are not rendered
    (image None) unless keep_blank is set.
    """
    mask = _pool_mask(raw)
    box = _bounds(mask)
    if box is None:
        return None, {"sheet": False}
    content = content_fraction(raw, box)
    blank = content < blank_below
    if blank and not keep_blank:
        return None, {"sheet": True, "content": round(content, 5), "blank": True}
    sheet, info = extract_sheet(raw)
    if sheet is None:
        return None, info
    info.update(content=round(content, 5), blank=blank)
    return Image.fromarray(color_correct(sheet)), info


def interleave(front, rear):
    """[(number, page)] per side -> [(number, 'front'|'rear', page)] in reading order."""
    rear = dict(rear)
    out = []
    for n, page in front:
        out.append((n, "front", page))
        if n in rear:
            out.append((n, "rear", rear.pop(n)))
    out += [(n, "rear", p) for n, p in sorted(rear.items())]
    return out
