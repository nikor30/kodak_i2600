#!/usr/bin/env python3
"""Kodak i2600 scan station on our own SANE backend: Start button → scan → PDF → Paperless-ngx.

  kodak_sane.py [--config /etc/kodak-scan/config.yaml]

This daemon is a plain SANE client (python-sane). Everything scanner-specific is in the
`kodak_i2x00` backend (backend/ in the repository): power-up after a power cycle, the
Start button and function number as sensor options, LCD labels, scanning, deskew, crop
and colour correction. The station adds profiles, blank-side removal, PDF, spool and upload
(shared with kodak_scand.py).

The function number chosen with ▲/▼ selects the profile (`native.functions` in the config);
a profile's `label:` is shown on the scanner's LCD.
"""
import argparse
import concurrent.futures
import io
import logging
import os
import pathlib
import signal
import subprocess
import threading
import time

import numpy as np
import sane

import kodak_scand as station

log = logging.getLogger("kodak-sane")
BACKEND = "kodak_i2x00"
DPI = 296.1                 # true scale of the scanner's "300 dpi" data: A4 comes out as 209.5 x 297 mm
NO_DOCS = "Document feeder out of documents"
BLANK_BELOW = 0.0005        # share of dark cells under which a side counts as blank
VENDOR_OPEN = ["/usr/local/sbin/kodak-x86", "/usr/bin/env", "BOX64_LOG=0", "BOX64_NOBANNER=1",
               "/usr/local/bin/box64", "/usr/bin/scanimage", "-d", "kds_i2000:i2000", "-A"]
DEFAULTS = {"trigger": "button", "functions": {1: None},
            "powerup_seq": "/etc/kodak-scan/firmware/powerup.seq",
            "workers": 3, "memory_pages": 4, "spool_dir": "/var/tmp"}


# ---------------------------------------------------------------- processing --
def content_fraction(im):
    """Share of 4x4 cells clearly darker than the paper, ignoring a 3 % border."""
    a = np.asarray(im.convert("L"), dtype=np.float32)
    h, w = a.shape[0] // 4 * 4, a.shape[1] // 4 * 4
    a = a[:h, :w].reshape(h // 4, 4, w // 4, 4).mean(axis=(1, 3))
    by, bx = int(a.shape[0] * 0.03), int(a.shape[1] * 0.03)
    a = a[by:a.shape[0] - by, bx:a.shape[1] - bx]
    return float((a < np.median(a) - 40).mean()) if a.size else 1.0


def encode(im, mode, keep_blank, quality, threshold):
    """Worker thread: one scanned side -> (file extension, bytes, content), bytes None if blank."""
    content = content_fraction(im)
    if content < BLANK_BELOW and not keep_blank:
        return None, None, content
    buf = io.BytesIO()
    if mode == "lineart":
        im.convert("L").point(lambda v: 255 if v > threshold else 0).convert("1").save(
            buf, "TIFF", compression="group4", dpi=(DPI, DPI))
        return ".tif", buf.getvalue(), content
    im.save(buf, "JPEG", quality=quality, dpi=(DPI, DPI))
    return ".jpg", buf.getvalue(), content


def profile_settings(profile):
    """What the station takes from a profile's `sane:` options."""
    opts = profile.get("sane") or {}
    mode = {"color": "color", "gray": "gray", "lineart": "lineart"}.get(str(opts.get("mode", "Color")).lower(), "color")
    duplex = str(opts.get("duplex", "both")).lower() in ("both", "duplex")
    keep_blank = str(opts.get("blankimagemode", "none")).lower() == "none"
    # bw_threshold: gray level (0-255) up to which a pixel prints black; paper white is 255.
    return mode, duplex, keep_blank, int(profile.get("jpeg_quality", 85)), int(profile.get("bw_threshold", 200))


# ------------------------------------------------------------------- scanner --
def vendor_open():
    """Fallback: let the vendor driver open the scanner once (loads firmware after power-up)."""
    if not os.access(VENDOR_OPEN[0], os.X_OK):
        return
    log.info("running the vendor driver once to initialise the scanner")
    try:
        r = subprocess.run(VENDOR_OPEN, capture_output=True, timeout=240)
        log.info("vendor open: exit %d", r.returncode)
    except (OSError, subprocess.TimeoutExpired) as e:
        log.error("vendor open failed: %s", e)
    finally:
        subprocess.run([VENDOR_OPEN[0], "umount"], capture_output=True)


class Station:
    def __init__(self, cfg, spool, status, wake_uploader, stop):
        self.cfg, self.spool, self.status, self.wake, self.stop = cfg, spool, status, wake_uploader, stop
        self.scfg = {**DEFAULTS, **(cfg.get("native") or {})}
        self.functions = {int(n): (p or cfg["profile"]) for n, p in self.scfg["functions"].items()}
        for n, p in self.functions.items():
            if p not in cfg["profiles"]:
                raise SystemExit(f"native.functions: {n} -> unknown profile {p!r}")
        self.pool = concurrent.futures.ThreadPoolExecutor(int(self.scfg["workers"]), thread_name_prefix="encode")
        self.function = 1
        self.dev = None
        self.sensors = {}
        self.last_vendor_open = 0
        self.write_sane_config()

    # ---- configuration of the backend -------------------------------------------
    def label(self, number):
        name = self.functions.get(number)
        if name is None:
            return f"{number}: not used"
        return self.cfg["profiles"][name].get("label") or name

    def write_sane_config(self):
        """Our own SANE configuration directory: only the kodak_i2x00 backend, with the labels
        and the power-up file from the station's config."""
        d = pathlib.Path(os.environ.get("RUNTIME_DIRECTORY", "/run/kodak-scan").split(":")[0]) / "sane.d"
        d.mkdir(parents=True, exist_ok=True)
        lines = ["usb 0x040a 0x601d", f"memory-pages {int(self.scfg['memory_pages'])}",
                 f"spool-dir {self.scfg['spool_dir']}"]
        if self.scfg.get("sequence_file"):
            lines.append(f"sequence {self.scfg['sequence_file']}")
        if pathlib.Path(self.scfg["powerup_seq"]).is_file():
            lines.append(f"powerup {self.scfg['powerup_seq']}")
        else:
            log.warning("no power-up file %s: after a scanner power cycle the vendor driver is needed", self.scfg["powerup_seq"])
        for n in sorted(self.functions):
            if 1 <= n <= 9:
                lines.append(f"label {n} {self.label(n).encode('ascii', 'replace').decode()}")
        (d / "dll.conf").write_text(BACKEND + "\n")
        (d / f"{BACKEND}.conf").write_text("\n".join(lines) + "\n")
        os.environ["SANE_CONFIG_DIR"] = str(d)

    # ---- connection ------------------------------------------------------------------
    def sensor(self, name):
        return self.dev.dev.get_option(self.dev[name].index)

    def show_ready(self, **extra):
        self.status.set(state="ready", error=None, label=f"{self.function}: {self.label(self.function)}", **extra)

    def connect(self):
        sane.init()
        names = [d[0] for d in sane.get_devices() if d[0].startswith(BACKEND + ":")]
        if not names:
            sane.exit()
            raise RuntimeError("scanner not found")
        try:
            self.dev = sane.open(names[0])
        except Exception:
            sane.exit()
            raise
        self.function = self.sensor("function_number") or 1
        self.sensors = {"paper": bool(self.sensor("page_loaded")), "cover": bool(self.sensor("cover_open"))}
        self.sensor("scan")                 # forget a press from before we were ready
        log.info("scanner %s ready (function %d, %s); labels: %s", names[0], self.function,
                 "paper loaded" if self.sensors["paper"] else "feeder empty",
                 ", ".join(f"{n} = {self.label(n)}" for n in sorted(self.functions)))
        self.show_ready()

    def disconnect(self):
        if self.dev is not None:
            try:
                self.dev.close()
            except Exception:  # noqa: BLE001
                pass
            self.dev = None
            try:
                sane.exit()
            except Exception:  # noqa: BLE001
                pass

    # ---- one job -----------------------------------------------------------------------
    def run_scan(self, number):
        name = self.functions.get(number)
        if name is None:
            log.info("Start pressed on function %d, which has no profile", number)
            self.status.set(state="error", error=f"{number}: not used")
            time.sleep(2)
            return self.show_ready()
        profile = self.cfg["profiles"][name]
        mode, duplex, keep_blank, quality, threshold = profile_settings(profile)
        self.dev.mode = "Color" if mode == "color" else "Gray"      # black/white is made here, from gray
        self.dev.source = "ADF Duplex" if duplex else "ADF Front"
        t0 = time.time()
        try:
            self.dev.start()
        except sane._sane.error as e:
            reason = "no paper" if str(e) == NO_DOCS else str(e)
            log.info("Start on function %d: not started: %s", number, reason)
            self.status.set(state="error", error=reason.capitalize())
            time.sleep(2)
            if str(e) != NO_DOCS and "cover" not in str(e).lower():
                raise
            return self.show_ready()

        job = self.spool.new_job()
        workdir = self.spool.work / job
        log.info("job %s: Start on function %d → profile %s", job, number, name)
        self.status.set(state="scanning", pages=0, error=None)
        results, error = [], None
        try:
            while True:
                im = self.dev.snap(no_cancel=True)
                while sum(not f.done() for f in results) >= 2 * int(self.scfg["workers"]):
                    time.sleep(0.05)        # do not pile up decoded pages in memory
                results.append(self.pool.submit(encode, im, mode, keep_blank, quality, threshold))
                del im
                self.status.set(state="scanning", pages=(len(results) + 1) // 2 if duplex else len(results))
                try:
                    self.dev.start()
                except sane._sane.error as e:
                    if str(e) != NO_DOCS:
                        error = str(e)
                    break
        except Exception as e:  # noqa: BLE001 (keep the pages we have)
            error = str(e)
        finally:
            try:
                self.dev.cancel()
            except Exception:  # noqa: BLE001
                pass
        t_scan = time.time() - t0
        kept = 0
        for f in results:
            try:
                ext, data, _content = f.result(timeout=600)
            except Exception as e:  # noqa: BLE001
                log.error("job %s: processing a page failed: %s", job, e)
                error = error or "page processing failed"
                continue
            if data is None:
                continue
            kept += 1
            (workdir / f"page-{kept:04d}{ext}").write_bytes(data)
        sides = len(results)
        log.info("job %s: %d sheets, %d of %d sides kept, scan %.1f s, total %.1f s%s", job,
                 (sides + 1) // 2 if duplex else sides, kept, sides, t_scan, time.time() - t0,
                 f", ERROR: {error}" if error else "")
        if station.finish_job(self.spool, job, profile, incomplete=error is not None):
            self.wake.set()
        if error:
            self.status.set(state="error", error=error.capitalize())
            time.sleep(4)
            if "I/O" in error:
                raise RuntimeError(error)
        self.show_ready()

    # ---- main loop ---------------------------------------------------------------------
    def loop(self):
        stop = self.stop
        while not stop.is_set():
            if self.dev is None:
                try:
                    self.connect()
                except Exception as e:  # noqa: BLE001
                    log.error("scanner: %s; retrying in 10 s", e)
                    self.status.set(state="error", error="Scanner not found")
                    self.disconnect()
                    if "I/O" in str(e) and time.time() - self.last_vendor_open > 300:
                        self.last_vendor_open = time.time()     # no usable power-up file: old way
                        vendor_open()
                    stop.wait(10)
                    continue
            try:
                pressed = self.sensor("scan")
                function = self.sensor("function_number") or self.function
                paper, cover = bool(self.sensor("page_loaded")), bool(self.sensor("cover_open"))
                if function != self.function:
                    log.info("function %d selected (%s)", function, self.label(function))
                    self.function = function
                    self.show_ready()
                if cover != self.sensors["cover"]:
                    log.info("cover %s", "opened" if cover else "closed")
                    if cover:
                        self.status.set(state="error", error="Cover open")
                    else:
                        self.show_ready()
                inserted = paper and not self.sensors["paper"]
                if paper != self.sensors["paper"]:
                    log.info("paper %s", "loaded" if paper else "removed")
                self.sensors = {"paper": paper, "cover": cover}
                if pressed:
                    self.run_scan(self.function)
                elif inserted and self.scfg["trigger"] == "paper":
                    time.sleep(1.5)          # let the stack settle
                    self.run_scan(self.function)
                else:
                    stop.wait(0.2)
            except Exception as e:  # noqa: BLE001 (unplugged, power-cycled, …)
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
    st = Station(cfg, spool, status, wake, stop)
    try:
        st.loop()
    finally:
        stop.set()
        wake.set()
        st.pool.shutdown(wait=False)
        up.join(timeout=10)


if __name__ == "__main__":
    main()
