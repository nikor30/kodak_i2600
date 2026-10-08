#!/usr/bin/env python3
"""First native scan: replay the vendor's captured scan-start sequence, read both image pipes.

  native_scan.py CAPTURE.pcap OUTDIR [--t-start 119.36 --t-end 120.91]

Experimental. It replays the control requests of one captured scan start verbatim
(docs/protocol/commands.md section 8: color 300 dpi duplex), with the captured timing,
then reads front and rear image data until the scanner reports End of Operation.
Only the request codes seen in that window are allowed; IN replies are compared with
the capture and differences are logged. The scan services must be stopped.
"""
import argparse
import ctypes
import os
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "usbcap"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pi", "scan-station"))
import kds_usb as k
from usbmon_decode import transfers

OUT_ALLOWED = {0x3A, 0x1B, 0x32, 0x31, 0x11, 0x45, 0x37, 0xA3, 0xE0, 0x30, 0x10, 0x17}
IN_ALLOWED = {0x00, 0x32, 0x35, 0x37, 0xA3, 0xE0}
LINE = 7740
EP_FRONT, EP_REAR = 0x82, 0x86


class Raw(k.Device):
    def __init__(self):
        super().__init__()
        self.lib.libusb_bulk_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_char_p, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int), ctypes.c_uint]

    def ctrl_out(self, req, value, index, data=b""):
        if req not in OUT_ALLOWED:
            raise PermissionError(f"OUT request 0x{req:02x} not allowed in the replay")
        self._check(self.lib.libusb_control_transfer(
            self.h, 0x40, req, value, index, bytes(data), len(data), 2000), f"set 0x{req:02x}")

    def ctrl_in(self, req, value, index, length):
        if req not in IN_ALLOWED:
            raise PermissionError(f"IN request 0x{req:02x} not allowed in the replay")
        buf = ctypes.create_string_buffer(max(length, 1))
        n = self._check(self.lib.libusb_control_transfer(
            self.h, 0xC0, req, value, index, buf, length, 2000), f"get 0x{req:02x}")
        return buf.raw[:n]

    def bulk_in(self, ep, size, timeout):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(self.h, ep, buf, size, ctypes.byref(got), timeout)
        if rc not in (0, k.LIBUSB_ERROR_TIMEOUT):
            self._check(rc, f"bulk ep 0x{ep:02x}")
        return buf.raw[:got.value]


def log(t0, msg):
    print(f"{time.time() - t0:8.3f} {msg}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("outdir")
    ap.add_argument("--t-start", type=float, default=119.36)
    ap.add_argument("--t-end", type=float, default=120.91)
    ap.add_argument("--max-seconds", type=float, default=40)
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    xs = list(transfers(args.capture))
    c0 = xs[0]["t0"]
    steps = [x for x in xs if args.t_start <= x["t"] - c0 <= args.t_end
             and x["xfer"] == 2 and x["setup"] and x["setup"][0] & 0x60]
    for x in steps:
        bm, req = x["setup"][0], x["setup"][1]
        if req not in (OUT_ALLOWED if bm == 0x40 else IN_ALLOWED):
            sys.exit(f"capture window contains request {bm:02x} {req:02x}, which is not allowed")
    print(f"{len(steps)} control requests to replay")

    dev = Raw()
    t0 = time.time()
    st = k.parse_status(dev.get(k.GET_STATUS, 32))
    if st["tray"] != 2 or st["interlock"] != 1 or st["power"] != 3 or st["error"]:
        sys.exit(f"not ready (need paper in the feeder, cover closed): {st}")
    dev.claim()

    stop = threading.Event()
    end_of_op = threading.Event()
    counts = {"eoo": 0}

    def event_thread():
        while not stop.is_set():
            try:
                ev = dev.read_interrupt(k.EP_INT_B, timeout=200)
            except k.UsbError as e:
                log(t0, f"event read error: {e}")
                return
            if ev:
                log(t0, f"event {ev.hex(' ')}  {k.EVENT_NAMES.get(ev[0], '?')}")
                if ev[0] == 0x01:
                    counts["eoo"] += 1
                    if counts["eoo"] >= 2:
                        end_of_op.set()
                if ev[0] in (0x30, 0x31, 0x32, 0x34):
                    end_of_op.set()

    data = {EP_FRONT: bytearray(), EP_REAR: bytearray()}

    def image_thread(ep):
        idle = 0
        while not stop.is_set():
            chunk = dev.bulk_in(ep, 1 << 18, 500)
            data[ep] += chunk
            idle = 0 if chunk else idle + 1
            if end_of_op.is_set() and idle >= 2:
                return

    def read_block(ep, name):
        """Pre-scan capture: read until a short packet."""
        blob = bytearray()
        while True:
            chunk = dev.bulk_in(ep, 16384, 2000)
            blob += chunk
            if len(chunk) < 16384:
                break
        open(os.path.join(args.outdir, name), "wb").write(blob)
        log(t0, f"pre-scan block ep 0x{ep:02x}: {len(blob)} bytes")

    ev_thr = threading.Thread(target=event_thread, daemon=True)
    started = False
    ok = False
    try:
        prev_end = None
        diffs = 0
        for i, x in enumerate(steps):
            bm, req, val, idx, ln = struct.unpack("<BBHHH", x["setup"])
            if prev_end is not None:
                gap = (x["t0"] - c0) - prev_end
                if gap > 0.02:
                    time.sleep(min(gap, 1.0))
            prev_end = x["t"] - c0
            if bm == 0x40:
                dev.ctrl_out(req, val, idx, x["data"])
                if req in (0x10, 0x17, 0x30, 0x3A, 0x1B, 0x11):
                    log(t0, f"sent {req:02x} v={val:04x} i={idx:04x} len={len(x['data'])}")
                if req == 0x3A and not ev_thr.is_alive():
                    ev_thr.start()
                if req == 0x10 and val == 1:
                    started = True
                if req == 0x10 and val == 0:
                    started = False
                if req == 0x17:
                    read_block(EP_FRONT, "prescan-front.bin")
                    read_block(EP_REAR, "prescan-rear.bin")
            else:
                got = dev.ctrl_in(req, val, idx, ln)
                want = x["data"]
                same = got == want if req != 0 else got[12:] == want[12:]
                if not same:
                    diffs += 1
                    log(t0, f"reply differs: {req:02x} v={val:04x} i={idx:04x} got {got[:24].hex()} captured {want[:24].hex()}")
        log(t0, f"replay done, {diffs} replies differ; reading images")
        thr = [threading.Thread(target=image_thread, args=(ep,), daemon=True) for ep in data]
        for t in thr:
            t.start()
        deadline = time.time() + args.max_seconds
        last = 0
        while any(t.is_alive() for t in thr) and time.time() < deadline:
            time.sleep(0.5)
            total = sum(len(d) for d in data.values())
            if total != last:
                log(t0, f"front {len(data[EP_FRONT])} rear {len(data[EP_REAR])} bytes")
                last = total
        ok = end_of_op.is_set()
        started = not ok
        log(t0, "end of operation" if ok else "TIMEOUT waiting for end of operation")
    finally:
        stop.set()
        time.sleep(0.6)
        try:
            if started:
                dev.ctrl_out(0x10, 0, 0)
                log(t0, "sent OperationStop")
            dev.ctrl_out(0x11, 0, 0)
            dev.ctrl_out(0x45, 0, 0, bytes([2, 0, 0]))
            dev.ctrl_out(0x3A, 1, 0)
            log(t0, "lamp off, batch end, events off")
        except Exception as e:
            log(t0, f"cleanup failed: {e}")
        for ep, name in ((EP_FRONT, "front"), (EP_REAR, "rear")):
            open(os.path.join(args.outdir, f"{name}.raw"), "wb").write(data[ep])
        st = dev.get(k.GET_STATUS, 32)
        log(t0, f"status {st[12:28].hex(' ')}")
        dev.close()

    try:
        from PIL import Image
        for name, ep in (("front", EP_FRONT), ("rear", EP_REAR)):
            n = len(data[ep]) // LINE
            if n:
                Image.frombytes("RGB", (LINE // 3, n), bytes(data[ep][:n * LINE])).save(
                    os.path.join(args.outdir, f"{name}.png"))
                print(f"{name}: {n} lines (+{len(data[ep]) % LINE} bytes) -> {name}.png")
    except ImportError:
        pass
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
