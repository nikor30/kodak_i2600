#!/usr/bin/env python3
"""kodak-scand: scan whatever is put into the feeder and send it to Paperless-ngx.

Talks only to SANE (here: the Kodak vendor backend behind saned on localhost,
see ADR-009/ADR-010), so the driver underneath can be swapped.

Flow: keep the device open and poll sane_start() every few seconds. With an
empty feeder the backend answers "out of documents" at once (F-035). When a
start succeeds, read the whole stack (one frame per side) into
spool/work/<job>/, turn it into one PDF in spool/outbox/, and let the upload
thread POST it to /api/documents/post_document/. Jobs survive restarts and
Paperless outages: anything left in work/ is turned into a PDF on start, and
the outbox is retried until the upload succeeds.
"""
import argparse
import datetime as dt
import json
import logging
import os
import pathlib
import shutil
import signal
import sys
import threading
import time
import uuid

import img2pdf
import requests
import sane
import yaml

log = logging.getLogger("kodak-scand")
NO_DOCS = "Document feeder out of documents"
# Exit status that asks systemd to restart kodak-saned (ExecStopPost in the unit): the
# vendor stack inside a long-running saned can get stuck after a device I/O error (F-038).
EXIT_RESTART_SANED = 75
RESTART_SANED_AFTER = 3        # scanner errors in a row
RESTART_SANED_INTERVAL = 600   # seconds between two requests (scanner switched off …)

DEFAULTS = {
    "device": "net:127.0.0.1:kds_i2000:i2000",
    "poll_interval": 2,
    "spool_dir": "/var/lib/kodak-scan",
    "keep_sent_days": 7,
    "profile": "color300",
    "profiles": {},
    "paperless": {"url": None, "token_file": "/etc/kodak-scan/paperless-token",
                  "timeout": 300, "retry_interval": 60},
}


def load_config(path):
    with open(path) as f:
        cfg = yaml.safe_load(f) or {}
    merged = {**DEFAULTS, **cfg}
    merged["paperless"] = {**DEFAULTS["paperless"], **(cfg.get("paperless") or {})}
    if merged["profile"] not in merged["profiles"]:
        raise SystemExit(f"profile {merged['profile']!r} not defined in {path}")
    return merged


class Spool:
    def __init__(self, root):
        self.root = pathlib.Path(root)
        self.work, self.outbox = self.root / "work", self.root / "outbox"
        self.sent, self.failed = self.root / "sent", self.root / "failed"
        for d in (self.work, self.outbox, self.sent, self.failed):
            d.mkdir(parents=True, exist_ok=True)

    def new_job(self):
        job = dt.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
        (self.work / job).mkdir()
        return job


class Status:
    """What the service is doing, for the OLED (kodak-oled): $RUNTIME_DIRECTORY/status.json."""

    def __init__(self, spool):
        run = os.environ.get("RUNTIME_DIRECTORY")
        self.path = pathlib.Path(run.split(":")[0], "status.json") if run else None
        self.spool, self.lock, self.data = spool, threading.Lock(), {"state": "starting"}

    def set(self, **changes):
        if self.path is None:
            return
        with self.lock:
            self.data.update(changes)
            self.data["queued"] = len(list(self.spool.outbox.glob("*.pdf")))
            try:
                tmp = self.path.with_suffix(".tmp")
                tmp.write_text(json.dumps(self.data))
                tmp.rename(self.path)
            except OSError as e:
                log.warning("status file: %s", e)


# ------------------------------------------------------------------ scanning --
def open_device(cfg, profile):
    dev = sane.open(cfg["device"])
    for name, value in (profile.get("sane") or {}).items():
        # Order matters: e.g. blankimagemode must be set before blankimagecontent.
        setattr(dev, name.replace("-", "_"), value)
    log.info("opened %s with %s", cfg["device"], profile.get("sane"))
    return dev


def save_page(im, path, profile):
    dpi = int((profile.get("sane") or {}).get("resolution", 300))
    if im.mode == "1":
        path = path.with_suffix(".tif")
        im.save(path, compression="group4", dpi=(dpi, dpi))
    elif profile.get("image_format", "jpeg") == "png":
        path = path.with_suffix(".png")
        im.save(path, dpi=(dpi, dpi))
    else:
        path = path.with_suffix(".jpg")
        im.save(path, quality=int(profile.get("jpeg_quality", 85)), dpi=(dpi, dpi))
    return path


def scan_stack(dev, spool, job, profile, status):
    """First frame is already started. Returns (pages, error or None)."""
    workdir, pages = spool.work / job, 0
    try:
        while True:
            status.set(state="scanning", pages=pages)
            im = dev.snap(no_cancel=True)
            pages += 1
            save_page(im, workdir / f"page-{pages:04d}", profile)
            log.info("job %s: page %d (%dx%d %s)", job, pages, im.size[0], im.size[1], im.mode)
            try:
                dev.start()
            except sane._sane.error as e:
                if str(e) == NO_DOCS:
                    return pages, None
                raise
    except Exception as e:  # jam, cover open, connection lost … keep what we have
        return pages, e
    finally:
        try:
            dev.cancel()
        except Exception:
            pass


def finish_job(spool, job, profile, incomplete=False):
    """work/<job>/page-* → outbox/<job>.pdf + .json. Returns True if a PDF was made."""
    workdir = spool.work / job
    pages = sorted(p for p in workdir.iterdir() if p.name.startswith("page-"))
    if not pages:
        shutil.rmtree(workdir, ignore_errors=True)
        return False
    created = dt.datetime.strptime(job[:15], "%Y%m%d-%H%M%S")
    title = profile.get("title", "Scan {created:%Y-%m-%d %H:%M}").format(created=created)
    if incomplete:
        title += " (incomplete)"
    tmp = spool.outbox / f".{job}.pdf.tmp"
    with open(tmp, "wb") as f:
        f.write(img2pdf.convert([str(p) for p in pages]))
    meta = {"title": title, "created": created.isoformat(), "tags": profile.get("tags") or [],
            "pages": len(pages)}
    (spool.outbox / f"{job}.json").write_text(json.dumps(meta))
    tmp.rename(spool.outbox / f"{job}.pdf")   # the PDF appears last: the uploader keys on it
    shutil.rmtree(workdir)
    log.info("job %s: %d pages → outbox (%s)", job, len(pages), title)
    return True


def recover_work(spool, profile):
    for workdir in sorted(spool.work.iterdir()):
        log.warning("recovering unfinished job %s", workdir.name)
        finish_job(spool, workdir.name, profile, incomplete=True)


def saned_restart_due(spool):
    """True at most once per RESTART_SANED_INTERVAL; the time stamp survives our restart."""
    stamp = spool.root / "saned-restart"
    try:
        if time.time() - stamp.stat().st_mtime < RESTART_SANED_INTERVAL:
            return False
    except OSError:
        pass
    stamp.touch()
    return True


def scanner_loop(cfg, spool, stop, wake_uploader, status):
    """Returns the process exit status."""
    profile = cfg["profiles"][cfg["profile"]]
    dev, backoff, failures = None, 5, 0
    sane.init()
    while not stop.is_set():
        try:
            if dev is None:
                dev = open_device(cfg, profile)
                backoff = 5
                status.set(state="ready", error=None)
            try:
                dev.start()
            except sane._sane.error as e:
                if str(e) == NO_DOCS:
                    failures = 0
                    stop.wait(cfg["poll_interval"])
                    continue
                raise
            job = spool.new_job()
            log.info("job %s: paper detected, scanning", job)
            t0 = time.monotonic()
            pages, err = scan_stack(dev, spool, job, profile, status)
            log.info("job %s: %d frames in %.1f s", job, pages, time.monotonic() - t0)
            if finish_job(spool, job, profile, incomplete=err is not None):
                wake_uploader.set()
            if err is not None:
                raise err
            failures = 0
            status.set(state="ready", error=None)
        except Exception as e:
            failures += 1
            status.set(state="error", error=str(e))
            if failures >= RESTART_SANED_AFTER and saned_restart_due(spool):
                log.error("scanner: %s; %d errors in a row, asking for a kodak-saned restart", e, failures)
                return EXIT_RESTART_SANED
            log.error("scanner: %s; reopening in %d s", e, backoff)
            if dev is not None:
                try:
                    dev.close()
                except Exception:
                    pass
                dev = None
            stop.wait(backoff)
            backoff = min(backoff * 2, 60)
    if dev is not None:
        dev.close()
    return 0


# ------------------------------------------------------------------- upload --
class PermanentError(Exception):
    pass


class Paperless:
    def __init__(self, pcfg):
        self.url = (pcfg.get("url") or "").rstrip("/")
        self.timeout = pcfg["timeout"]
        cred = os.environ.get("CREDENTIALS_DIRECTORY")
        token_file = pathlib.Path(cred, "paperless-token") if cred else pathlib.Path(pcfg["token_file"])
        self.token = token_file.read_text().strip() if token_file.is_file() else ""
        self.tag_ids = {}

    def configured(self):
        return bool(self.url and self.token)

    def _headers(self):
        return {"Authorization": f"Token {self.token}"}

    def tag_id(self, tag):
        if isinstance(tag, int):
            return tag
        if tag not in self.tag_ids:
            r = requests.get(f"{self.url}/api/tags/", params={"name__iexact": tag},
                             headers=self._headers(), timeout=30)
            r.raise_for_status()
            res = r.json().get("results") or []
            self.tag_ids[tag] = res[0]["id"] if res else None
            if not res:
                log.warning("Paperless tag %r does not exist; skipping it", tag)
        return self.tag_ids[tag]

    def upload(self, pdf, meta):
        data = [("title", meta["title"]), ("created", meta["created"])]
        data += [("tags", str(i)) for i in map(self.tag_id, meta.get("tags", [])) if i is not None]
        with open(pdf, "rb") as f:
            r = requests.post(f"{self.url}/api/documents/post_document/", headers=self._headers(),
                              data=data, files={"document": (pdf.name, f, "application/pdf")},
                              timeout=self.timeout)
        if r.status_code in (400, 413, 415):
            raise PermanentError(f"HTTP {r.status_code}: {r.text[:300]}")
        r.raise_for_status()   # 401/403/5xx: keep it in the outbox and retry
        return r.text.strip().strip('"')   # consumption task id


def uploader_loop(cfg, spool, stop, wake, status):
    pl = Paperless(cfg["paperless"])
    if not pl.configured():
        log.error("Paperless url/token not configured: scans stay in %s", spool.outbox)
    while not stop.is_set():
        if pl.configured():
            for pdf in sorted(spool.outbox.glob("*.pdf")):
                meta_file = pdf.with_suffix(".json")
                meta = json.loads(meta_file.read_text())
                try:
                    task = pl.upload(pdf, meta)
                except PermanentError as e:
                    log.error("upload of %s rejected, moved to failed/: %s", pdf.name, e)
                    dest = spool.failed
                except Exception as e:
                    log.warning("upload of %s failed, retrying in %d s: %s",
                                pdf.name, cfg["paperless"]["retry_interval"], e)
                    break
                else:
                    log.info("uploaded %s (%d pages), Paperless task %s", pdf.name, meta["pages"], task)
                    dest = spool.sent
                shutil.move(str(pdf), dest / pdf.name)
                shutil.move(str(meta_file), dest / meta_file.name)
        status.set()   # refresh the queue count
        cutoff = time.time() - cfg["keep_sent_days"] * 86400
        for f in spool.sent.iterdir():
            if f.stat().st_mtime < cutoff:
                f.unlink()
        wake.wait(cfg["paperless"]["retry_interval"])
        wake.clear()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default="/etc/kodak-scan/config.yaml")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(threadName)s: %(message)s")
    cfg = load_config(args.config)
    spool = Spool(cfg["spool_dir"])
    recover_work(spool, cfg["profiles"][cfg["profile"]])
    status = Status(spool)
    status.set()

    stop, wake = threading.Event(), threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: (stop.set(), wake.set()))
    up = threading.Thread(target=uploader_loop, args=(cfg, spool, stop, wake, status), name="upload", daemon=True)
    up.start()
    wake.set()   # upload whatever is left in the outbox right away
    threading.current_thread().name = "scan"
    code = scanner_loop(cfg, spool, stop, wake, status)
    stop.set()
    wake.set()
    up.join(timeout=10)
    sys.exit(code)


if __name__ == "__main__":
    main()
