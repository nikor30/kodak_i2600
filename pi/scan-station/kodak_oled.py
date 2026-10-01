#!/usr/bin/env python3
"""kodak-oled: status display on the Waveshare PoE HAT (B) OLED (SSD1306, 128x32, I2C 0x3c).

Top line: the Pi's IP address. Bottom line: what the scan station is doing,
read from the status file kodak-scand keeps in /run/kodak-scan/status.json
(starting / ready / scanning page N / error, plus the upload queue).

While the station is idle ("Ready", nothing to upload) a screensaver takes
over: a starfield with the IP address, CPU temperature and fan state bouncing
around, which also spares the OLED from burn-in.

The HAT's fan is switched by one pin of the PCF8574 at 0x20 (on/off only, no
PWM). This service also runs it as a thermostat: on above fan.on_temp, off
below fan.off_temp, and on again when the service stops.
"""
import argparse
import json
import logging
import random
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
            "contrast": 0x60, "status_file": "/run/kodak-scan/status.json",
            "screensaver": True, "screensaver_after": 30}
FAN_DEFAULTS = {"enabled": True, "address": 0x20, "on_temp": 60, "off_temp": 50}
CPU_TEMP = "/sys/class/thermal/thermal_zone0/temp"


BIT_REVERSE = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))


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
        # The panel wants one byte per column and 8-pixel page, top pixel = bit 0. Transposed,
        # each image column is a row of height/8 bytes with the top pixel in bit 7.
        cols, pages = im.transpose(Image.Transpose.TRANSPOSE).tobytes(), self.height // 8
        buf = list(b"".join(cols[page::pages].translate(BIT_REVERSE) for page in range(pages)))
        self.cmd(0x21, 0, self.width - 1, 0x22, 0, self.height // 8 - 1)
        for i in range(0, len(buf), 32):       # SMBus block limit
            self.bus.write_i2c_block_data(self.addr, 0x40, buf[i:i + 32])

    def off(self):
        self.cmd(0xAE)
        self.bus.close()


class Fan:
    """Thermostat for the HAT fan: PCF8574 pin P0, low = fan on."""

    def __init__(self, bus, cfg):
        self.bus_no, self.cfg, self.on, self.checked, self.temp = bus, cfg, None, 0.0, None

    def switch(self, on):
        with SMBus(self.bus_no) as bus:
            pins = bus.read_byte(self.cfg["address"])
            bus.write_byte(self.cfg["address"], pins & 0xFE if on else pins | 0x01)
        if on != self.on:
            log.info("fan %s", "on" if on else "off")
        self.on = on

    def update(self):
        if time.monotonic() - self.checked < 5:
            return
        self.checked = time.monotonic()
        try:
            with open(CPU_TEMP) as f:
                temp = int(f.read()) / 1000
        except (OSError, ValueError):
            temp = None                        # unknown temperature: keep it cooled
        self.temp = temp
        if not self.cfg["enabled"]:
            return
        try:
            if temp is None or temp >= self.cfg["on_temp"] or self.on is None and temp > self.cfg["off_temp"]:
                self.switch(True)
            elif temp <= self.cfg["off_temp"]:
                self.switch(False)
        except OSError as e:
            log.error("fan: %s", e)

    def text(self):
        """For the display, e.g. "52°C · fan off"."""
        parts = [] if self.temp is None else [f"{self.temp:.0f}°C"]
        if self.on is not None:
            parts.append("fan on" if self.on else "fan off")
        return " · ".join(parts)

    def release(self):
        """Leave the fan running when nobody watches the temperature."""
        if self.cfg["enabled"]:
            try:
                self.switch(True)
            except OSError:
                pass


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


class Screensaver:
    """Starfield flying right to left; IP address, temperature and fan state bounce over it."""

    def __init__(self, cfg):
        self.cfg, self.w, self.h = cfg, cfg["width"], cfg["height"]
        self.stars = [self.star(random.uniform(0, self.w)) for _ in range(28)]
        self.font, self.small = font(True, 10), font(False, 9)
        self.x, self.y, self.dx, self.dy = 3.0, 2.0, 1.0, 0.5

    def star(self, x):
        return [x, random.randrange(self.h), random.choice((0.4, 0.4, 0.8, 0.8, 1.6, 2.6))]

    def frame(self, ip, info=""):
        im = Image.new("1", (self.w, self.h))
        d = ImageDraw.Draw(im)
        for i, (x, y, v) in enumerate(self.stars):
            x -= v
            self.stars[i] = [x, y, v] if x > -4 else self.star(self.w)
            d.line((x, y, x + (v if v > 1 else 0), y), fill=1)   # fast stars leave a streak
        text = ip or "no network"
        l, t, r, b = d.textbbox((0, 0), text, font=self.font)
        tw, th = r - l, b - t
        if info:
            il, it, ir, ib = d.textbbox((0, 0), info, font=self.small)
            ih = th + 3                        # where the second line starts
            tw, th = max(tw, ir - il), ih + ib - it
        self.x += self.dx
        self.y += self.dy
        if not 0 <= self.x <= self.w - tw - 1:
            self.dx = -self.dx
            self.x = min(max(self.x, 0), self.w - tw - 1)
        if not 0 <= self.y <= self.h - th - 1:
            self.dy = -self.dy
            self.y = min(max(self.y, 0), self.h - th - 1)
        x, y = int(self.x), int(self.y)
        d.rectangle((x - 2, y - 2, x + tw + 1, y + th + 1), fill=0)
        d.text((x - l, y - t), text, font=self.font, fill=1)
        if info:
            d.text((x - il, y + ih - it), info, font=self.small, fill=1)
        return im.rotate(180) if self.cfg["rotate"] == 180 else im


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default="/etc/kodak-scan/config.yaml")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    try:
        with open(args.config) as f:
            conf = yaml.safe_load(f) or {}
    except OSError:
        conf = {}
    cfg = {**DEFAULTS, **(conf.get("oled") or {})}
    fan = Fan(cfg["bus"], {**FAN_DEFAULTS, **(conf.get("fan") or {})})

    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())

    oled, last, tick, ip, ip_at = None, None, 0, None, 0.0
    saver, idle_since, saving = Screensaver(cfg), None, False
    while not stop.is_set():
        fan.update()
        try:
            if oled is None:
                oled = SSD1306(cfg["bus"], cfg["address"], cfg["width"], cfg["height"], cfg["contrast"])
                last = None
                log.info("OLED on i2c-%d at 0x%02x", cfg["bus"], cfg["address"])
            if time.monotonic() - ip_at > 5:
                ip, ip_at = ip_address(), time.monotonic()
            st = read_status(cfg["status_file"])
            if not (cfg["screensaver"] and st and st.get("state") == "ready" and not st.get("queued")):
                idle_since = None
            elif idle_since is None:
                idle_since = time.monotonic()
            saving = idle_since is not None and time.monotonic() - idle_since >= cfg["screensaver_after"]
            im = saver.frame(ip, fan.text()) if saving else render(cfg, ip, st, tick)
            data = im.tobytes()
            if data != last:
                oled.show(im)
                last = data
        except OSError as e:                   # no HAT, I2C off, bus glitch
            log.error("display: %s; retrying in 30 s", e)
            oled = None
            stop.wait(30)
        tick += 1
        stop.wait(0.1 if saving else 0.5)
    fan.release()
    if oled is not None:
        try:
            oled.off()
        except OSError:
            pass


if __name__ == "__main__":
    main()
