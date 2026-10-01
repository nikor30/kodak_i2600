#!/usr/bin/env python3
"""kodak-oled: status display on the Waveshare PoE HAT (B) OLED (SSD1306, 128x32, I2C 0x3c).

Top line: the Pi's IP address. Bottom line: what the scan station is doing,
read from the status file kodak-scand keeps in /run/kodak-scan/status.json
(starting / ready / scanning page N / error, plus the upload queue).

Only the display is touched. The HAT's fan controller (PCF8574 at 0x20) is
left as it is.
"""
import argparse
import json
import logging
import signal
import socket
import threading
import time

import yaml
from PIL import Image, ImageDraw, ImageFont
from smbus2 import SMBus

log = logging.getLogger("kodak-oled")
FONT_DIR = "/usr/share/fonts/truetype/dejavu/"
DEFAULTS = {"bus": 1, "address": 0x3C, "width": 128, "height": 32, "rotate": 0,
            "contrast": 0x60, "status_file": "/run/kodak-scan/status.json"}


class SSD1306:
    def __init__(self, bus, address, width, height, contrast):
        self.bus, self.addr, self.width, self.height = SMBus(bus), address, width, height
        self.cmd(0xAE,                         # display off
                 0xD5, 0x80,                   # clock divider
                 0xA8, height - 1,             # multiplex ratio
                 0xD3, 0x00, 0x40,             # no offset, start line 0
                 0x8D, 0x14,                   # charge pump on
                 0x20, 0x00,                   # horizontal addressing
                 0xA1, 0xC8,                   # segment remap, COM scan direction
                 0xDA, 0x02 if height == 32 else 0x12,
                 0x81, contrast,
                 0xD9, 0xF1, 0xDB, 0x40,
                 0xA4, 0xA6, 0xAF)             # show RAM, not inverted, display on

    def cmd(self, *values):
        for v in values:
            self.bus.write_byte_data(self.addr, 0x00, v)

    def show(self, im):
        px, buf = im.load(), []
        for page in range(self.height // 8):
            for x in range(self.width):
                byte = 0
                for bit in range(8):
                    if px[x, page * 8 + bit]:
                        byte |= 1 << bit
                buf.append(byte)
        self.cmd(0x21, 0, self.width - 1, 0x22, 0, self.height // 8 - 1)
        for i in range(0, len(buf), 32):       # SMBus block limit
            self.bus.write_i2c_block_data(self.addr, 0x40, buf[i:i + 32])

    def off(self):
        self.cmd(0xAE)
        self.bus.close()


def ip_address():
    """Source address of the default route (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def read_status(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def status_text(st):
    if st is None:
        return "Scan service off"
    state = st.get("state")
    if state == "scanning":
        return f"Scanning page {st.get('pages', 0) + 1}"
    if state == "starting":
        return "Scanner starting…"
    if state == "error":
        return st.get("error") or "Scanner error"
    if st.get("queued"):
        return f"Ready · {st['queued']} to upload"
    return "Ready"


def font(bold, size):
    return ImageFont.truetype(FONT_DIR + ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"), size)


def fit(draw, text, bold, size, width, min_size=9):
    """Largest font ≤ size in which text fits; at min_size the text is cut."""
    while True:
        f = font(bold, size)
        if draw.textlength(text, font=f) <= width or size <= min_size:
            break
        size -= 1
    while draw.textlength(text, font=f) > width and len(text) > 1:
        text = text[:-2] + "…"
    return text, f


def render(cfg, ip, st, tick):
    w, h = cfg["width"], cfg["height"]
    im = Image.new("1", (w, h))
    d = ImageDraw.Draw(im)
    x = int(time.time() // 60) % 3             # shift a little every minute against burn-in
    text, f = fit(d, ip or "no network", False, 12, w - 2)
    d.text((x, 0), text, font=f, fill=1)
    text, f = fit(d, status_text(st), True, 14, w - 2)
    d.text((x, 14), text, font=f, fill=1)
    if st and st.get("state") == "scanning":   # moving bar: a page is being processed
        pos = tick * 8 % (w + 24) - 24
        d.rectangle((pos, h - 2, pos + 24, h - 1), fill=1)
    return im.rotate(180) if cfg["rotate"] == 180 else im


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default="/etc/kodak-scan/config.yaml")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    try:
        with open(args.config) as f:
            cfg = {**DEFAULTS, **((yaml.safe_load(f) or {}).get("oled") or {})}
    except OSError:
        cfg = dict(DEFAULTS)

    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())

    oled, last, tick, ip, ip_at = None, None, 0, None, 0.0
    while not stop.is_set():
        try:
            if oled is None:
                oled = SSD1306(cfg["bus"], cfg["address"], cfg["width"], cfg["height"], cfg["contrast"])
                last = None
                log.info("OLED on i2c-%d at 0x%02x", cfg["bus"], cfg["address"])
            if time.monotonic() - ip_at > 5:
                ip, ip_at = ip_address(), time.monotonic()
            im = render(cfg, ip, read_status(cfg["status_file"]), tick)
            data = im.tobytes()
            if data != last:
                oled.show(im)
                last = data
        except OSError as e:                   # no HAT, I2C off, bus glitch
            log.error("display: %s; retrying in 30 s", e)
            oled = None
            stop.wait(30)
        tick += 1
        stop.wait(0.5)
    if oled is not None:
        try:
            oled.off()
        except OSError:
            pass


if __name__ == "__main__":
    main()
