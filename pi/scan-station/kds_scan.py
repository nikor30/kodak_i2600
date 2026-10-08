"""Native scanning on the Kodak i2x00: events, scan start by sequence replay, page streaming.

Protocol: docs/protocol/commands.md (section 8). A scan is started by replaying a
captured request sequence (sequences/*.json); image data is read from both bulk pipes
in parallel and cut into pages at the trailers while the scan is still running.
"""
import ctypes
import json
import logging
import pathlib
import queue
import re
import threading
import time

import numpy as np

import kds_usb as k

log = logging.getLogger("kds-scan")

LINE_PX = 2580
LINE = LINE_PX * 3
EP_FRONT, EP_REAR = 0x82, 0x86
SIDES = {EP_FRONT: "front", EP_REAR: "rear"}

# Requests a scan sequence may contain (all seen in the vendor's scan start).
SEQ_OUT = {0x3A, 0x1B, 0x32, 0x31, 0x11, 0x45, 0x37, 0xA3, 0xE0, 0x30, 0x10, 0x17}
SEQ_IN = {0x00, 0x32, 0x35, 0x37, 0xA3, 0xE0}

# Requests of the power-up initialisation (docs/protocol/commands.md section 6). Everything here
# is volatile: firmware and FPGA image go into RAM and are gone at power-off. Requests that write
# permanent storage (NVRam 35, EEPROM a2, flash, firmware update 23/24) are not in the lists, so a
# power-up file containing one is refused.
PWR_OUT = {0x21, 0xA0, 0x20, 0xF1, 0xA3, 0x37, 0x1F, 0x18, 0x11, 0xE0, 0x30, 0x17}
PWR_IN = {0x00, 0xF2, 0xA3, 0x02, 0x34, 0x36, 0x37, 0x03, 0x35, 0xE3, 0xE2, 0xE0, 0x32, 0x33}
EP_BULK_OUT = 0x02
KODAK_EPOCH = 978307200          # 2001-01-01 00:00:00 UTC, for SetTime

EV_END_OF_OPERATION, EV_BUTTON, EV_TRAY, EV_INTERLOCK, EV_FUNCTION = 0x01, 0x20, 0x13, 0x16, 0x60
EV_IMAGING_COMPLETE, EV_PAGE_EXIT = 0x42, 0x41
EV_ERRORS = {0x30: "paper jam", 0x31: "multifeed", 0x32: "buffer overflow", 0x34: "scanner error"}

_TAGS = re.compile(rb"\x00\x01\x00\x00(..)\x02\xf5..\x01\xf6..\x01\xf9..\x01\xfa..\x01\xfc..\x01\xfd..\x01\xfe", re.S)


class NotReady(Exception):
    pass


class ScanError(Exception):
    pass


def load_sequence(path):
    seq = json.loads(pathlib.Path(path).read_text())
    for bm, req, *_ in seq["steps"]:
        if req not in (SEQ_OUT if bm == 0x40 else SEQ_IN):
            raise ValueError(f"{path}: request {bm:02x} {req:02x} is not allowed in a scan sequence")
    return seq


class PageSplitter:
    """Collects one side's stream and hands out complete pages."""

    def __init__(self, side, on_page):
        self.side, self.on_page = side, on_page
        self.buf = bytearray()
        self.line = 0            # next line boundary to test for a trailer
        self.pages = 0

    def feed(self, chunk):
        self.buf += chunk
        while True:
            pos = self.line * LINE
            if pos + 32 > len(self.buf):
                return
            m = _TAGS.match(self.buf, pos)
            if not m:
                self.line += 1
                continue
            end = None
            for kk in range(256):
                q = m.end() + 2 * kk
                if q + 4 > len(self.buf):
                    return               # trailer not complete yet
                if self.buf[q:q + 4] == bytes((0, kk, 1, 0xFF)):
                    end = q + 4
                    break
            if end is None:              # tags by coincidence in pixel data
                self.line += 1
                continue
            if self.line:
                page = np.frombuffer(bytes(self.buf[:pos]), dtype=np.uint8).reshape(self.line, LINE_PX, 3)
                self.pages += 1
                self.on_page(int.from_bytes(m.group(1), "big"), self.side, page)
            del self.buf[:end]
            self.line = 0

    def leftover_lines(self):
        return len(self.buf) // LINE


class Scanner(k.Device):
    """An opened scanner with interrupt events delivered to self.events (queue of 8-byte messages)."""

    def __init__(self):
        super().__init__()
        self.lib.libusb_bulk_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_char_p, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
        self.events = queue.Queue()
        self.dead = None                 # set to an exception when the event reader loses the device
        self._stop = threading.Event()
        self._reader = None

    # ---- low level -----------------------------------------------------------
    def _out(self, req, value, index, data=b""):
        if req not in SEQ_OUT:
            raise PermissionError(f"OUT request 0x{req:02x} not allowed")
        self._check(self.lib.libusb_control_transfer(
            self.h, 0x40, req, value, index, bytes(data), len(data), 2000), f"set 0x{req:02x}")

    def _in(self, req, value, index, length):
        if req not in SEQ_IN:
            raise PermissionError(f"IN request 0x{req:02x} not allowed")
        buf = ctypes.create_string_buffer(max(length, 1))
        n = self._check(self.lib.libusb_control_transfer(
            self.h, 0xC0, req, value, index, buf, length, 2000), f"get 0x{req:02x}")
        return buf.raw[:n]

    def _bulk(self, ep, size, timeout):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(self.h, ep, buf, size, ctypes.byref(got), timeout)
        if rc not in (0, k.LIBUSB_ERROR_TIMEOUT):
            self._check(rc, f"bulk ep 0x{ep:02x}")
        return buf.raw[:got.value]

    def status(self):
        return k.parse_status(self.get(k.GET_STATUS, 32))

    # ---- events --------------------------------------------------------------
    def start_events(self):
        self.claim()
        self.events_on()
        self._reader = threading.Thread(target=self._read_events, name="events", daemon=True)
        self._reader.start()

    def events_on(self):
        self.set(k.INTERRUPT_EVENT_CONTROL, 1, 1)

    def _read_events(self):
        while not self._stop.is_set():
            try:
                ev = self.read_interrupt(k.EP_INT_B, timeout=250)
            except k.UsbError as e:
                if not self._stop.is_set():
                    self.dead = e
                return
            if ev and len(ev) == 8:
                self.events.put(ev)

    def close(self):
        self._stop.set()
        if self._reader:
            self._reader.join(timeout=2)
        if self.h:
            try:
                self.set(k.INTERRUPT_EVENT_CONTROL, 1, 0)
            except Exception:
                pass
        super().close()

    def drain_events(self):
        while True:
            try:
                self.events.get_nowait()
            except queue.Empty:
                return

    # ---- scanning ------------------------------------------------------------
    def _replay(self, seq):
        vram = None
        for bm, req, val, idx, ln, data, gap in seq["steps"]:
            if gap:
                time.sleep(min(gap, 1.0))
            if bm == 0x40:
                payload = bytes.fromhex(data)
                if req == 0x37 and vram is not None:
                    payload = vram            # write back what this scanner just reported
                self._out(req, val, idx, payload)
                if req == 0x17:               # pre-scan capture: one short block per side, discarded
                    for ep in (EP_FRONT, EP_REAR):
                        while len(self._bulk(ep, 16384, 2000)) == 16384:
                            pass
            else:
                reply = self._in(req, val, idx, ln)
                if req == 0x37:
                    vram = reply

    def scan(self, seq, on_page, on_progress=None, max_idle=30):
        """Scan everything in the feeder. on_page(number, side, array) is called from reader threads.

        Returns {"sheets": n, "error": text or None}. Raises NotReady before anything was started.
        """
        st = self.status()
        if st["interlock"] != 1:
            raise NotReady("cover open")
        if st["tray"] != 2:
            raise NotReady("no paper")
        if st["fw_id"] != 3 or st["error"]:
            raise NotReady(f"scanner not initialised (firmware id {st['fw_id']}, error {st['error']})")
        self.drain_events()

        done = threading.Event()
        failure = []
        splitters = {ep: PageSplitter(side, on_page) for ep, side in SIDES.items()}

        def reader(ep):
            idle = 0
            try:
                while idle < 2 or not done.is_set():
                    chunk = self._bulk(ep, 1 << 18, 500)
                    idle = 0 if chunk else idle + 1
                    if chunk:
                        splitters[ep].feed(chunk)
            except Exception as e:      # noqa: BLE001 (report through the main thread)
                failure.append(e)
                done.set()

        result = {"sheets": 0, "error": None}
        started = False
        threads = []
        try:
            self._replay(seq)
            started = True
            threads = [threading.Thread(target=reader, args=(ep,), name=SIDES[ep], daemon=True) for ep in SIDES]
            for t in threads:
                t.start()
            ends, last = 0, time.time()
            while not done.is_set():
                if self.dead:
                    raise ScanError(f"scanner lost: {self.dead}")
                try:
                    ev = self.events.get(timeout=0.5)
                except queue.Empty:
                    if time.time() - last > max_idle:
                        raise ScanError("no event from the scanner for too long")
                    continue
                last = time.time()
                if ev[0] == EV_END_OF_OPERATION:
                    ends += 1
                    if ends >= 2:            # the first one ends the pre-scan capture
                        result["sheets"] = ev[4]
                        started = False
                        done.set()
                elif ev[0] == EV_PAGE_EXIT and on_progress:
                    on_progress(ev[1])
                elif ev[0] in EV_ERRORS:
                    result["error"] = EV_ERRORS[ev[0]]
                elif ev[0] == EV_INTERLOCK and ev[2] == 2:
                    result["error"] = "cover opened"
                    done.set()
        except (k.UsbError, ScanError) as e:
            result["error"] = str(e)
        finally:
            done.set()
            for t in threads:
                t.join(timeout=5)
            try:
                if started:
                    self._out(0x10, 0, 0)                    # OperationStop
                self._out(0x11, 0, 0)                        # lamp off
                self._out(0x45, 0, 0, bytes((2, 0, 0)))      # batch end (as the vendor)
            except Exception as e:  # noqa: BLE001
                log.warning("scan cleanup: %s", e)
        if failure and not result["error"]:
            result["error"] = str(failure[0])
        cut = max(s.leftover_lines() for s in splitters.values())
        if cut > 50 and not result["error"]:
            result["error"] = "image data ended inside a page"
        result["pages"] = {s.side: s.pages for s in splitters.values()}
        return result

    # ---- power-up ------------------------------------------------------------
    def _ctrl(self, bm, req, value, index, data_or_len, timeout=5000):
        if bm == 0x40:
            data = bytes(data_or_len)
            self._check(self.lib.libusb_control_transfer(
                self.h, 0x40, req, value, index, data, len(data), timeout), f"set 0x{req:02x}")
            return b""
        buf = ctypes.create_string_buffer(max(data_or_len, 1))
        n = self._check(self.lib.libusb_control_transfer(
            self.h, 0xC0, req, value, index, buf, data_or_len, timeout), f"get 0x{req:02x}")
        return buf.raw[:n]

    def power_up(self, json_path):
        """Load firmware and FPGA image into a freshly powered scanner by replaying the vendor's
        own initialisation (extracted locally with tools/usbcap/extract_powerup.py)."""
        json_path = pathlib.Path(json_path)
        steps = json.loads(json_path.read_text())["steps"]
        blob = json_path.with_suffix(".bin").read_bytes()
        for st in steps:                       # check everything before sending anything
            if "bulk" in st:
                if st["bulk"] != EP_BULK_OUT & 0x7F:
                    raise ValueError("power-up file: unexpected bulk endpoint")
            elif st["req"] not in (PWR_OUT if st["bm"] == 0x40 else PWR_IN) or st["bm"] not in (0x40, 0xC0):
                raise ValueError(f"power-up file: request {st['bm']:02x} {st['req']:02x} is not allowed")
            elif st["req"] == 0xF1 and st["val"] != 3:
                raise ValueError("power-up file: unexpected diagnostic request")
        if self.status()["fw_id"] != 1:
            raise NotReady("scanner is not in its power-up state")
        self.claim()
        for st in steps:
            if st["gap"]:
                time.sleep(min(st["gap"], 2.0))
            if "bulk" in st:
                off, ln = st["blob"]
                data = blob[off:off + ln]
                got = ctypes.c_int(0)
                self._check(self.lib.libusb_bulk_transfer(self.h, EP_BULK_OUT, data, ln, ctypes.byref(got), 5000), "bulk out")
                if got.value != ln:
                    raise k.UsbError("bulk out: short write")
                continue
            bm, req, val, idx = st["bm"], st["req"], st["val"], st["idx"]
            if bm == 0x40:
                data = blob[st["blob"][0]:st["blob"][0] + st["blob"][1]] if "blob" in st else bytes.fromhex(st["data"])
                if req == 0x1F:                # SetTime: now, not the captured moment
                    secs = int(time.time()) - KODAK_EPOCH
                    val, idx = secs >> 16, secs & 0xFFFF
                self._ctrl(0x40, req, val, idx, data)
                if req == 0x17:                # calibration capture: one short block per side, discarded
                    for ep in (EP_FRONT, EP_REAR):
                        while len(self._bulk(ep, 16384, 3000)) == 16384:
                            pass
            elif req == 0x00:
                # The vendor waits here for the just-loaded firmware to come up: poll until the
                # running firmware id matches the recorded one.
                want = bytes.fromhex(st["data"])[0]
                deadline = time.time() + 15
                while True:
                    try:
                        got = self._ctrl(0xC0, 0, 0, 0, 32, timeout=3000)
                    except k.UsbError:
                        got = b""
                    if got[:1] == bytes((want,)):
                        break
                    if time.time() > deadline:
                        raise ScanError(f"power-up: firmware id {got[:1].hex() or '?'} instead of {want:02x}")
                    time.sleep(0.1)
            else:
                self._ctrl(0xC0, req, val, idx, st["len"])
        st = self.status()
        if st["fw_id"] != 3:
            raise ScanError(f"power-up finished but firmware id is {st['fw_id']}")
        return st

    # ---- panel ---------------------------------------------------------------
    def set_label(self, number, text):
        self.lcd_populate(k.lcd_text(text), (k.LCD_TYPE_LABEL, number))
