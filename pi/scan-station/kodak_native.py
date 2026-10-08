#!/usr/bin/env python3
"""kodak-native: scan station on the native driver (no vendor driver, no box64 while scanning).

Press Start on the scanner: the stack in the feeder is scanned with the profile assigned
to the function number shown on the scanner's LCD (▲/▼), turned into one PDF and sent to
Paperless-ngx through the same spool and uploader as kodak-scand. The function numbers
get text labels on the LCD.

Driver: kds_usb / kds_scan / kds_image (protocol: docs/protocol/). The scan itself is
always color 300 dpi duplex (the one captured sequence); gray, black/white and simplex
profiles are derived from it in software.

After a power cycle the scanner needs its firmware. If /etc/kodak-scan/firmware/powerup.json
exists (made locally from a capture with tools/usbcap/extract_powerup.py) the service loads it
natively; otherwise, or if that fails, it runs the vendor driver once (box64, ~25 s).
"""
import argparse
import datetime as dt
import io
import json
import logging
import multiprocessing
import pathlib
import queue
import signal
import subprocess
import sys
import threading
import time

import numpy as np

import kds_image as ki
import kds_scan as ks
import kds_usb as k
import kodak_scand as station

log = logging.getLogger("kodak-native")
HERE = pathlib.Path(__file__).resolve().parent
VENDOR_OPEN = ["/usr/local/sbin/kodak-x86", "/usr/bin/env", "BOX64_LOG=0", "BOX64_NOBANNER=1",
               "/usr/local/bin/box64", "/usr/bin/scanimage", "-d", "kds_i2000:i2000", "-A"]
NATIVE_DEFAULTS = {"trigger": "button", "functions": {1: None}, "sequence": "color300-duplex",
                   "powerup": "/etc/kodak-scan/firmware/powerup.json",
                   "workers": 3, "max_pages_in_memory": 6}


# ---------------------------------------------------------------- processing --
def render(job):
    """Pool worker: raw page (array or .npy path) -> (file extension, encoded bytes) or None if dropped."""
    raw, mode, keep_blank, quality, threshold = job
    if isinstance(raw, str):
        path = pathlib.Path(raw)
        raw = np.load(path)
        path.unlink()
    im, info = ki.process_page(raw, keep_blank=keep_blank)
    if im is None:
        return None, info
    buf = io.BytesIO()
    if mode == "lineart":
        im.convert("L").point(lambda v: 255 if v > threshold else 0).convert("1").save(
            buf, "TIFF", compression="group4", dpi=(ki.DPI, ki.DPI))
        return (".tif", buf.getvalue()), info
    if mode == "gray":
        im = im.convert("L")
    im.save(buf, "JPEG", quality=quality, dpi=(ki.DPI, ki.DPI))
    return (".jpg", buf.getvalue()), info


def profile_settings(profile):
    """What the native path takes from a profile's `sane:` options."""
    opts = profile.get("sane") or {}
    mode = {"color": "color", "gray": "gray", "lineart": "lineart"}.get(str(opts.get("mode", "Color")).lower(), "color")
    duplex = str(opts.get("duplex", "both")).lower() in ("both", "duplex")
    keep_blank = str(opts.get("blankimagemode", "none")).lower() == "none"
    # bw_threshold: gray level (0-255, after colour correction) up to which a pixel prints black.
    # 200 keeps light gray print that 150 loses; paper white is 255.
    return mode, duplex, keep_blank, int(profile.get("jpeg_quality", 85)), int(profile.get("bw_threshold", 200))


# ------------------------------------------------------------------- scanner --
def vendor_open():
    """Let the vendor driver open the scanner once (loads firmware after power-up)."""
    log.info("running the vendor driver once to initialise the scanner")
    try:
        r = subprocess.run(VENDOR_OPEN, capture_output=True, timeout=240)
        log.info("vendor open: exit %d", r.returncode)
    except (OSError, subprocess.TimeoutExpired) as e:
        log.error("vendor open failed: %s", e)
    finally:
        subprocess.run(["/usr/local/sbin/kodak-x86", "umount"], capture_output=True)


def open_scanner(powerup_file):
    dev = ks.Scanner()
    try:
        st = dev.status()
        if st["fw_id"] != 3 and pathlib.Path(powerup_file).is_file():
            log.info("scanner was power-cycled: loading its firmware natively")
            t = time.time()
            try:
                st = dev.power_up(powerup_file)
                log.info("native power-up done in %.1f s", time.time() - t)
            except Exception as e:  # noqa: BLE001 (anything here falls back to the vendor driver)
                log.error("native power-up failed: %s", e)
                st = {"fw_id": 0}
        if st["fw_id"] != 3:
            dev.close()
            vendor_open()
            dev = ks.Scanner()
            st = dev.status()
            if st["fw_id"] != 3:
                raise k.UsbError(f"scanner firmware not running (id {st['fw_id']})")
        if st["button"] == 0:
            # After the native power-up the panel has no function number (blank LCD, Start reports
            # function 0). The vendor driver sets it on every open; we do it only when it is missing.
            dev.set_function_numbers()
            st = dev.status()
            log.info("function number set on the panel (now %d)", st["button"])
        dev.start_events()
        return dev, st
    except Exception:
        dev.close()
        raise


def write_labels(dev, labels):
    """Upload the function labels. The scanner forgets them when it is switched off (F-065)."""
    for number, text in sorted(labels.items()):
        dev.set_label(number, text)
    log.info("LCD labels: %s", ", ".join(f"{n} = {t}" for n, t in sorted(labels.items())))


class Station:
    def __init__(self, cfg, spool, status, wake_uploader, pool, stop):
        self.cfg, self.spool, self.status, self.wake, self.pool = cfg, spool, status, wake_uploader, pool
        self.stop = stop
        self.ncfg = {**NATIVE_DEFAULTS, **(cfg.get("native") or {})}
        self.functions = {int(n): (p or cfg["profile"]) for n, p in self.ncfg["functions"].items()}
        for n, p in self.functions.items():
            if p not in cfg["profiles"]:
                raise SystemExit(f"native.functions: {n} -> unknown profile {p!r}")
        self.seq = ks.load_sequence(HERE / "sequences" / f"{self.ncfg['sequence']}.json")
        self.function = 1
        self.dev = None

    def label(self, number):
        name = self.functions.get(number)
        if name is None:
            return f"{number}: not used"
        return self.cfg["profiles"][name].get("label") or name

    def show_ready(self, **extra):
        self.status.set(state="ready", error=None, label=f"{self.function}: {self.label(self.function)}"
                        if self.function in self.functions else f"{self.function}: not used", **extra)

    def connect(self):
        self.dev, st = open_scanner(self.ncfg["powerup"])
        self.function = st["button"] or 1
        labels = {n: self.label(n) for n in self.functions}
        try:
            write_labels(self.dev, labels)
        except k.UsbError as e:
            log.warning("LCD labels not written: %s", e)
        log.info("scanner ready (function %d, %s)", self.function, "paper loaded" if st["tray"] == 2 else "feeder empty")
        self.show_ready()

    def disconnect(self):
        if self.dev:
            try:
                self.dev.close()
            except Exception:  # noqa: BLE001
                pass
            self.dev = None

    def run_scan(self, number):
        name = self.functions.get(number)
        if name is None:
            log.info("Start pressed on function %d, which has no profile", number)
            self.status.set(state="error", error=f"{number}: not used")
            time.sleep(2)
            return self.show_ready()
        profile = self.cfg["profiles"][name]
        mode, duplex, keep_blank, quality, threshold = profile_settings(profile)
        job = self.spool.new_job()
        workdir = self.spool.work / job
        results, pending, lock = {}, [0], threading.Lock()
        limit = int(self.ncfg["max_pages_in_memory"])

        def on_page(image_number, side, raw):
            if side == "rear" and not duplex:
                return
            with lock:
                spill = pending[0] >= limit
                pending[0] += 1
            if spill:                           # long stacks: park raw pages on disk, not in RAM
                path = workdir / f"raw-{image_number:04d}-{side}.npy"
                np.save(path, raw)
                raw = str(path)

            def finished(res, key=(image_number, side)):
                with lock:
                    pending[0] -= 1
                    results[key] = res

            def failed(exc, key=(image_number, side)):
                log.error("job %s: processing image %d %s failed: %s", job, key[0], key[1], exc)
                finished((None, {"failed": True}), key)

            with lock:
                results[(image_number, side)] = None
            self.pool.apply_async(render, ((raw, mode, keep_blank, quality, threshold),), callback=finished, error_callback=failed)

        self.status.set(state="scanning", pages=0, error=None)
        log.info("job %s: Start on function %d → profile %s", job, number, name)
        t0 = time.time()
        try:
            res = self.dev.scan(self.seq, on_page, on_progress=lambda sheets: self.status.set(state="scanning", pages=sheets))
        except ks.NotReady as e:
            log.info("job %s: not started: %s", job, e)
            workdir.rmdir()
            self.status.set(state="error", error=str(e).capitalize())
            time.sleep(2)
            return self.show_ready()
        t_scan = time.time() - t0
        deadline = time.time() + 600
        while time.time() < deadline and not self.stop.is_set():   # on shutdown: keep what is finished
            with lock:
                if all(v is not None for v in results.values()):
                    break
            time.sleep(0.1)
        kept = 0
        for (image_number, side) in sorted(results, key=lambda key: (key[0], key[1] != "front")):
            out, info = results[(image_number, side)] or (None, {})
            if out is None:
                continue
            kept += 1
            (workdir / f"page-{kept:04d}{out[0]}").write_bytes(out[1])
        log.info("job %s: %d sheets, %d of %d sides kept, scan %.1f s, total %.1f s%s", job, res["sheets"], kept,
                 len(results), t_scan, time.time() - t0, f", ERROR: {res['error']}" if res["error"] else "")
        if station.finish_job(self.spool, job, profile, incomplete=res["error"] is not None):
            self.wake.set()
        if res["error"]:
            self.status.set(state="error", error=res["error"].capitalize())
            time.sleep(4)
            if self.dev.dead:
                raise k.UsbError(str(self.dev.dead))
        self.show_ready()

    def loop(self, stop):
        next_poll = 0
        while not stop.is_set():
            if self.dev is None:
                try:
                    self.connect()
                except (k.UsbError, OSError) as e:
                    log.error("scanner: %s; retrying in 10 s", e)
                    self.status.set(state="error", error="Scanner not found")
                    stop.wait(10)
                    continue
            try:
                if self.dev.dead:
                    raise k.UsbError(str(self.dev.dead))
                try:
                    ev = self.dev.events.get(timeout=0.5)
                except queue.Empty:
                    ev = None
                if ev is not None:
                    log.info("event %s (%s)", ev.hex(" "), k.EVENT_NAMES.get(ev[0], "?"))
                    if ev[0] == ks.EV_FUNCTION:
                        self.function = ev[2]
                        self.show_ready()
                    elif ev[0] == ks.EV_BUTTON:
                        self.function = ev[2] or self.function
                        self.run_scan(self.function)
                    elif ev[0] == ks.EV_TRAY and ev[2] == 2 and self.ncfg["trigger"] == "paper":
                        time.sleep(1.5)          # let the stack settle
                        self.run_scan(self.function)
                    elif ev[0] == ks.EV_INTERLOCK:
                        if ev[2] == 2:
                            self.status.set(state="error", error="Cover open")
                        else:
                            self.show_ready()
                if time.time() >= next_poll:     # notices an unplugged or power-cycled scanner
                    next_poll = time.time() + 5
                    st = self.dev.status()
                    states = {f: st[f] for f in ("power", "tray", "interlock", "lamp", "udds", "error")}
                    if states != getattr(self, "_states", None):     # for the log: sleep, paper, cover
                        log.info("status %s", " ".join(f"{f}={v}" for f, v in states.items()))
                        self._states = states
                    if st["fw_id"] != 3:
                        raise k.UsbError("scanner lost its firmware (power cycle?)")
                    # Cheap insurance: should anything switch the events off (another program
                    # using the scanner, a scanner-side reset), they come back within 5 s.
                    self.dev.events_on()
                    if st["button"] and st["button"] != self.function:
                        self.function = st["button"]
                        self.show_ready()
            except k.UsbError as e:
                log.error("scanner connection lost: %s", e)
                self.status.set(state="starting", error=None)
                self.disconnect()
                stop.wait(3)
        self.disconnect()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default="/etc/kodak-scan/config.yaml")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(threadName)s: %(message)s")
    cfg = station.load_config(args.config)
    ncfg = {**NATIVE_DEFAULTS, **(cfg.get("native") or {})}
    # Start the worker processes before any thread or USB handle exists.
    pool = multiprocessing.Pool(int(ncfg["workers"]), initializer=signal.signal, initargs=(signal.SIGINT, signal.SIG_IGN))
    spool = station.Spool(cfg["spool_dir"])
    station.recover_work(spool, cfg["profiles"][cfg["profile"]])
    status = station.Status(spool)
    status.set()

    stop, wake = threading.Event(), threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: (stop.set(), wake.set()))
    up = threading.Thread(target=station.uploader_loop, args=(cfg, spool, stop, wake, status), name="upload", daemon=True)
    up.start()
    wake.set()
    threading.current_thread().name = "scan"
    try:
        Station(cfg, spool, status, wake, pool, stop).loop(stop)
    finally:
        stop.set()
        wake.set()
        pool.terminate()
        up.join(timeout=10)


if __name__ == "__main__":
    main()
