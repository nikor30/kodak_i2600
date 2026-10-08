"""Where finished scans go, and the station's statistics.

Destinations: Paperless-ngx (in kodak_scand.py), an SMB share (through `smbclient`) and
e-mail (SMTP). A profile names its destination; the job's metadata file carries it to the
uploader. Passwords are read from root-only files, never from the config.
"""
import datetime as dt
import email.message
import email.utils
import json
import logging
import os
import pathlib
import re
import smtplib
import ssl
import subprocess
import tempfile
import threading

log = logging.getLogger("kodak-scand")
DESTINATIONS = ("paperless", "smb", "email")
SMB_DEFAULTS = {"share": None, "folder": "", "username": "", "domain": "",
                "password_file": "/etc/kodak-scan/smb-password", "timeout": 120}
EMAIL_DEFAULTS = {"host": None, "port": 587, "security": "starttls", "username": "",
                  "password_file": "/etc/kodak-scan/smtp-password", "sender": "", "to": "",
                  "subject": "{title}", "timeout": 120, "max_mb": 20}


class PermanentError(Exception):
    """The destination refuses this file for good: do not retry."""


def read_secret(path):
    try:
        return pathlib.Path(path).read_text().strip()
    except OSError:
        return ""


def safe_filename(title, job):
    """A file name for the share/attachment: the document title, made harmless."""
    name = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "-", title).strip(" .")[:100] or job
    return f"{name}.pdf"


# ----------------------------------------------------------------------- SMB --
class Smb:
    def __init__(self, cfg):
        self.cfg = {**SMB_DEFAULTS, **(cfg or {})}

    def configured(self):
        return bool(self.cfg["share"])

    def _run(self, command):
        share = str(self.cfg["share"]).replace("\\", "/")
        if not re.fullmatch(r"//[^/\s]+/[^/]+", share):
            raise PermanentError(f"share {share!r} must look like //server/share")
        # The password goes through a private file, never through the command line.
        with tempfile.NamedTemporaryFile("w", prefix="smb-auth-", dir=os.environ.get("RUNTIME_DIRECTORY", "").split(":")[0] or None) as auth:
            os.chmod(auth.name, 0o600)
            user = self.cfg["username"] or "guest"
            auth.write(f"username = {user}\npassword = {read_secret(self.cfg['password_file'])}\n")
            if self.cfg["domain"]:
                auth.write(f"domain = {self.cfg['domain']}\n")
            auth.flush()
            try:
                r = subprocess.run(["smbclient", share, "-A", auth.name, "-c", command], capture_output=True,
                                   text=True, timeout=int(self.cfg["timeout"]))
            except FileNotFoundError:
                raise RuntimeError("smbclient is not installed (apt install smbclient)") from None
            except subprocess.TimeoutExpired:
                raise RuntimeError("the share did not answer in time") from None
        out = (r.stdout + r.stderr).strip()
        if r.returncode != 0 or "NT_STATUS_" in out:
            status = re.search(r"NT_STATUS_\w+", out)
            raise RuntimeError(status.group(0) if status else (out.splitlines() or ["smbclient failed"])[-1][:200])
        return out

    @staticmethod
    def _quote(name):
        if '"' in name:
            raise PermanentError("file or folder names must not contain a double quote")
        return f'"{name}"'

    def _cd(self):
        folder = str(self.cfg["folder"] or "").replace("\\", "/").strip("/")
        return f"cd {self._quote(folder)}; " if folder else ""

    def upload(self, pdf, meta):
        name = safe_filename(meta["title"], pdf.stem)
        listing = self._run(self._cd() + "ls")
        if re.search(rf"^\s*{re.escape(name)}\s", listing, re.M):      # never overwrite a document
            name = f"{name[:-4]} {pdf.stem[-6:]}.pdf"
        self._run(self._cd() + f"put {self._quote(str(pdf))} {self._quote(name)}")
        return name

    def test(self):
        self._run(self._cd() + "ls")
        return f"The share answers; scans go to {self.cfg['share']}/{str(self.cfg['folder'] or '').strip('/')}".rstrip("/")


# --------------------------------------------------------------------- e-mail --
class Email:
    def __init__(self, cfg):
        self.cfg = {**EMAIL_DEFAULTS, **(cfg or {})}

    def configured(self):
        return bool(self.cfg["host"] and self.cfg["sender"])

    def _connect(self):
        c, timeout = self.cfg, int(self.cfg["timeout"])
        if c["security"] == "ssl":
            s = smtplib.SMTP_SSL(c["host"], int(c["port"]), timeout=timeout, context=ssl.create_default_context())
        else:
            s = smtplib.SMTP(c["host"], int(c["port"]), timeout=timeout)
            if c["security"] == "starttls":
                s.starttls(context=ssl.create_default_context())
        if c["username"]:
            s.login(c["username"], read_secret(c["password_file"]))
        return s

    def _send(self, to, subject, text, attachment=None):
        recipients = [a for _, a in email.utils.getaddresses([to]) if "@" in a]
        if not recipients:
            raise PermanentError("no e-mail recipient set")
        msg = email.message.EmailMessage()
        msg["From"], msg["To"], msg["Subject"] = self.cfg["sender"], ", ".join(recipients), subject
        msg["Date"] = email.utils.formatdate(localtime=True)
        msg["Message-ID"] = email.utils.make_msgid(domain=self.cfg["sender"].rpartition("@")[2] or None)
        msg.set_content(text)
        if attachment:
            name, data = attachment
            msg.add_attachment(data, maintype="application", subtype="pdf", filename=name)
        try:
            with self._connect() as s:
                s.send_message(msg, from_addr=email.utils.parseaddr(self.cfg["sender"])[1], to_addrs=recipients)
        except smtplib.SMTPRecipientsRefused as e:
            raise PermanentError(f"recipient refused: {list(e.recipients)}") from None
        except smtplib.SMTPResponseException as e:
            if 500 <= e.smtp_code < 600 and not isinstance(e, smtplib.SMTPAuthenticationError):
                raise PermanentError(f"mail server refused the message: {e.smtp_code}") from None
            raise
        return recipients

    def upload(self, pdf, meta):
        size = pdf.stat().st_size
        if size > float(self.cfg["max_mb"]) * 1e6:
            raise PermanentError(f"{size / 1e6:.1f} MB is more than the e-mail limit of {self.cfg['max_mb']} MB")
        subject = str(self.cfg["subject"] or "{title}").replace("{title}", meta["title"])
        text = f"{meta['title']}\n{meta['pages']} page(s), scanned {meta['created'].replace('T', ' ')[:16]}.\n"
        to = meta.get("email_to") or self.cfg["to"]
        return ", ".join(self._send(to, subject, text, (safe_filename(meta["title"], pdf.stem), pdf.read_bytes())))

    def test(self, to=None):
        sent = self._send(to or self.cfg["to"], "Test from the scan station", "The scan station can send e-mail.\n")
        return f"Test message sent to {', '.join(sent)}"


# ------------------------------------------------------------------ statistics --
class Stats:
    """Counters in <spool>/stats.json: totals, per profile, per destination, per day."""

    def __init__(self, spool_root):
        self.path = pathlib.Path(spool_root) / "stats.json"
        self.lock = threading.Lock()
        try:
            self.data = json.loads(self.path.read_text())
        except (OSError, ValueError):
            self.data = self._from_spool(pathlib.Path(spool_root))
            self._write()

    @staticmethod
    def _empty():
        return {"since": dt.date.today().isoformat(),
                "totals": {"jobs": 0, "sheets": 0, "sides": 0, "pages": 0, "blank": 0, "scan_seconds": 0.0,
                           "delivered": 0, "failed": 0, "bytes": 0},
                "profiles": {}, "destinations": {}, "days": {}}

    def _from_spool(self, root):
        """First start: count what the spool still knows (jobs and pages only)."""
        d = self._empty()
        for kind in ("sent", "outbox", "failed"):
            for meta in sorted((root / kind).glob("*.json")):
                try:
                    m = json.loads(meta.read_text())
                    day = m["created"][:10]
                    pages = int(m.get("pages", 0))
                except (OSError, ValueError, KeyError):
                    continue
                d["totals"]["jobs"] += 1
                d["totals"]["pages"] += pages
                d["totals"]["delivered"] += kind == "sent"
                rec = d["days"].setdefault(day, {"jobs": 0, "sheets": 0, "pages": 0})
                rec["jobs"] += 1
                rec["pages"] += pages
                d["since"] = min(d["since"], day)
        return d

    def _write(self):
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data))
        tmp.rename(self.path)

    def scanned(self, profile, sheets, sides, pages, seconds):
        with self.lock:
            t = self.data["totals"]
            t["jobs"] += 1
            t["sheets"] += sheets
            t["sides"] += sides
            t["pages"] += pages
            t["blank"] += sides - pages
            t["scan_seconds"] = round(t["scan_seconds"] + seconds, 1)
            p = self.data["profiles"].setdefault(profile, {"jobs": 0, "pages": 0})
            p["jobs"] += 1
            p["pages"] += pages
            day = self.data["days"].setdefault(dt.date.today().isoformat(), {"jobs": 0, "sheets": 0, "pages": 0})
            day["jobs"] += 1
            day["sheets"] += sheets
            day["pages"] += pages
            for old in sorted(self.data["days"])[:-400]:
                del self.data["days"][old]
            self._write()

    def delivered(self, destination, size, ok=True):
        with self.lock:
            t = self.data["totals"]
            if ok:
                t["delivered"] += 1
                t["bytes"] += size
                self.data["destinations"][destination] = self.data["destinations"].get(destination, 0) + 1
            else:
                t["failed"] += 1
            self._write()

    def summary(self):
        with self.lock:
            today = self.data["days"].get(dt.date.today().isoformat(), {"jobs": 0, "sheets": 0, "pages": 0})
            return {"today": dict(today), "totals": dict(self.data["totals"])}
