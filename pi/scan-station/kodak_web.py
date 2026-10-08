#!/usr/bin/env python3
"""Settings page for the Kodak i2600 scan station.

  kodak_web.py [--config /etc/kodak-scan/config.yaml]

A small web server (standard library only, plus ruamel.yaml to keep the comments in the
config file) with one page: Paperless address and token, which profile is on which
function number of the scanner, the profiles themselves (LCD text, colour mode, sides, …)
and the station's state. It only edits /etc/kodak-scan/config.yaml and the token file;
kodak-sane notices the change and restarts itself.

Access needs the password in /etc/kodak-scan/web-password (user name: admin). The Paperless
token can be set here but is never sent back to the browser.
"""
import argparse
import base64
import datetime as dt
import hmac
import http.server
import io
import json
import logging
import os
import pathlib
import re
import secrets
import socket
import string
import urllib.parse

import requests
from ruamel.yaml import YAML
from ruamel.yaml.comments import CommentedMap

log = logging.getLogger("kodak-web")
HERE = pathlib.Path(__file__).resolve().parent
ETC = pathlib.Path("/etc/kodak-scan")
STATUS_FILE = pathlib.Path("/run/kodak-scan/status.json")
DEFAULTS = {"port": 2600, "bind": "0.0.0.0", "auth": True, "password_file": str(ETC / "web-password")}
FUNCTIONS = range(1, 8)                 # the panel offers function numbers 1..7
MODES = {"Color", "Gray", "Lineart"}
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$")
MAX_BODY = 256 * 1024


class Invalid(Exception):
    pass


# ------------------------------------------------------------------- config --
class Config:
    """Reads and writes the station's YAML file, keeping its comments and unknown keys."""

    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.yaml = YAML()
        self.yaml.preserve_quotes = True
        self.yaml.width = 4096

    def load(self):
        with open(self.path) as f:
            return self.yaml.load(f) or CommentedMap()

    def token_file(self, doc=None):
        doc = doc if doc is not None else self.load()
        return pathlib.Path((doc.get("paperless") or {}).get("token_file") or ETC / "paperless-token")

    def view(self):
        """The settings the page shows (never the token itself)."""
        doc = self.load()
        pl = doc.get("paperless") or {}
        native = doc.get("native") or {}
        tf = self.token_file(doc)
        profiles = []
        for name, p in (doc.get("profiles") or {}).items():
            p = p or {}
            opts = p.get("sane") or {}
            profiles.append({
                "name": str(name),
                "label": str(p.get("label") or ""),
                "mode": {"color": "Color", "gray": "Gray", "lineart": "Lineart"}.get(str(opts.get("mode", "Color")).lower(), "Color"),
                "duplex": str(opts.get("duplex", "both")).lower() in ("both", "duplex"),
                "drop_blank": str(opts.get("blankimagemode", "none")).lower() != "none",
                "jpeg_quality": int(p.get("jpeg_quality", 85)),
                "bw_threshold": int(p.get("bw_threshold", 200)),
                "title": str(p.get("title", "Scan {created:%Y-%m-%d %H:%M}")),
                "tags": [str(t) for t in (p.get("tags") or [])],
            })
        functions = {str(int(n)): (str(v) if v else str(doc.get("profile", "")))
                     for n, v in (native.get("functions") or {}).items()}
        return {
            "paperless": {"url": str(pl.get("url") or ""), "token_set": tf.is_file() and tf.stat().st_size > 0},
            "trigger": str(native.get("trigger", "button")),
            "default_profile": str(doc.get("profile", "")),
            "functions": functions,
            "profiles": profiles,
        }

    def save(self, new):
        """Validate the page's settings and write them into the file. Returns a list of notes."""
        doc = self.load()
        url = str((new.get("paperless") or {}).get("url") or "").strip().rstrip("/")
        if url:
            parts = urllib.parse.urlsplit(url)
            if parts.scheme not in ("http", "https") or not parts.hostname:
                raise Invalid("Paperless address must look like http://host:8000")
        token = (new.get("paperless") or {}).get("token")
        if token is not None:
            token = str(token).strip()
            if token and not re.fullmatch(r"[A-Za-z0-9._~+/=-]{8,200}", token):
                raise Invalid("The token contains characters an API token cannot have")

        profiles = new.get("profiles")
        if not isinstance(profiles, list) or not profiles:
            raise Invalid("At least one profile is needed")
        names = []
        for p in profiles:
            name = str(p.get("name") or "")
            if not NAME_RE.match(name):
                raise Invalid(f"Profile name {name!r}: use letters, digits, - and _ (at most 32)")
            if name in names:
                raise Invalid(f"Profile name {name!r} is used twice")
            names.append(name)
            label = str(p.get("label") or "")
            if len(label) > 60 or not all(32 <= ord(c) < 127 for c in label):
                raise Invalid(f"Profile {name}: the display text may only use plain ASCII characters (at most 60)")
            if p.get("mode") not in MODES:
                raise Invalid(f"Profile {name}: unknown colour mode")
            if not 1 <= int(p.get("jpeg_quality", 85)) <= 100:
                raise Invalid(f"Profile {name}: JPEG quality must be 1-100")
            if not 0 <= int(p.get("bw_threshold", 200)) <= 255:
                raise Invalid(f"Profile {name}: black/white threshold must be 0-255")
            title = str(p.get("title") or "")
            try:        # only {created} with a date format, e.g. {created:%Y-%m-%d %H:%M}
                if any(fld not in (None, "created") or conv for _, fld, _, conv in string.Formatter().parse(title)):
                    raise ValueError
                title.format(created=dt.datetime.now())
            except Exception:  # noqa: BLE001
                raise Invalid(f"Profile {name}: the title may only use {{created}} with a date format") from None
            if len(title) > 120 or any(len(str(t)) > 60 for t in p.get("tags") or []):
                raise Invalid(f"Profile {name}: title or tag too long")

        functions = {}
        for n, name in (new.get("functions") or {}).items():
            if not str(n).isdigit() or int(n) not in FUNCTIONS:
                raise Invalid(f"Function number {n} does not exist")
            if name:
                if name not in names:
                    raise Invalid(f"Function {n} points to the unknown profile {name!r}")
                functions[int(n)] = name
        if not functions:
            raise Invalid("At least one function number needs a profile")
        default = str(new.get("default_profile") or "")
        if default not in names:
            default = functions[min(functions)]
        trigger = new.get("trigger", "button")
        if trigger not in ("button", "paper"):
            raise Invalid("Unknown trigger")

        # ---- apply to the document, touching only what the page manages
        doc.setdefault("paperless", CommentedMap())["url"] = url or None
        doc["profile"] = default
        old = doc.setdefault("profiles", CommentedMap())
        for name in [n for n in old if n not in names]:
            del old[name]
        for p in profiles:
            entry = old.get(p["name"])
            if not isinstance(entry, dict):
                entry = old[p["name"]] = CommentedMap()
            entry["label"] = str(p.get("label") or "") or p["name"]
            opts = entry.get("sane")
            if not isinstance(opts, dict):
                opts = entry["sane"] = CommentedMap()
            opts["mode"] = p["mode"]
            opts.setdefault("resolution", 300)
            opts["duplex"] = "both" if p.get("duplex", True) else "front"
            opts["blankimagemode"] = "content" if p.get("drop_blank", True) else "none"
            entry["jpeg_quality"] = int(p.get("jpeg_quality", 85))
            entry["bw_threshold"] = int(p.get("bw_threshold", 200))
            entry["title"] = str(p.get("title") or "Scan {created:%Y-%m-%d %H:%M}")
            entry["tags"] = [int(t) if str(t).isdigit() else str(t) for t in (p.get("tags") or []) if str(t).strip()]
        native = doc.setdefault("native", CommentedMap())
        native["trigger"] = trigger
        fmap = CommentedMap()
        for n in sorted(functions):
            fmap[n] = functions[n]
        native["functions"] = fmap

        buf = io.StringIO()
        self.yaml.dump(doc, buf)
        backup = self.path.with_name(self.path.name + ".bak-web")
        if not backup.exists():
            backup.write_bytes(self.path.read_bytes())      # the file as it was before the page first changed it
        tmp = self.path.with_name("." + self.path.name + ".tmp")
        tmp.write_text(buf.getvalue())
        os.chmod(tmp, 0o644)
        tmp.rename(self.path)
        notes = ["Settings saved. The station restarts with them within about 15 seconds."]
        if token:
            tf = self.token_file(doc)
            fd = os.open(tf.with_name("." + tf.name + ".tmp"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(token + "\n")
            tf.with_name("." + tf.name + ".tmp").rename(tf)
            notes.append("New Paperless token stored.")
        return notes


def test_paperless(cfg, url, token):
    """Ask Paperless whether it would take an upload with the given (or the stored) token."""
    url = (url or "").strip().rstrip("/")
    if not url:
        return False, "No Paperless address set"
    if urllib.parse.urlsplit(url).scheme not in ("http", "https"):
        return False, "The address must start with http:// or https://"
    if not token:
        tf = cfg.token_file()
        token = tf.read_text().strip() if tf.is_file() else ""
    if not token:
        return False, "No token set"
    # The upload address itself: a GET is not allowed there (405), which still tells us that
    # Paperless is there and accepts the token; 401 means it does not.
    try:
        r = requests.get(url + "/api/documents/post_document/", timeout=8, allow_redirects=False,
                         headers={"Authorization": f"Token {token}", "Accept": "application/json"})
    except requests.RequestException as e:
        return False, f"Paperless not reachable: {type(e).__name__}"
    if r.status_code in (200, 405):
        return True, "Paperless answers and accepts the token"
    if r.status_code == 401:
        return False, "Paperless answers, but does not accept the token"
    if r.status_code == 403:
        return False, "The token is valid, but its user may not upload documents"
    return False, f"Unexpected answer from the upload address: HTTP {r.status_code}"


def station_state(cfg):
    """What the station is doing and its last jobs, from its status file and spool."""
    state = {"service": "unknown"}
    try:
        state.update(json.loads(STATUS_FILE.read_text()))
        state["service"] = "running"
    except (OSError, ValueError):
        state["service"] = "not running"
    jobs = []
    try:
        spool = pathlib.Path(cfg.load().get("spool_dir") or "/var/lib/kodak-scan")
        for kind in ("outbox", "sent", "failed"):
            for meta in (spool / kind).glob("*.json"):
                try:
                    m = json.loads(meta.read_text())
                    jobs.append({"job": meta.stem, "where": kind, "title": str(m.get("title", "")),
                                 "pages": int(m.get("pages", 0)), "created": str(m.get("created", ""))})
                except (OSError, ValueError):
                    continue
    except OSError:
        pass
    jobs.sort(key=lambda j: j["job"], reverse=True)
    state["jobs"] = jobs[:12]
    state["waiting"] = sum(j["where"] == "outbox" for j in jobs)
    state["failed"] = sum(j["where"] == "failed" for j in jobs)
    return state


# ------------------------------------------------------------------- server --
class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "kodak-web"
    protocol_version = "HTTP/1.1"
    cfg = None
    password = None

    def log_message(self, fmt, *args):
        log.info("%s %s", self.address_string(), fmt % args)

    def reply(self, code, body, ctype="application/json", extra=()):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def authorised(self):
        if self.password is None:
            return True
        head = self.headers.get("Authorization", "")
        if head.startswith("Basic "):
            try:
                user, _, pw = base64.b64decode(head[6:]).decode().partition(":")
            except Exception:  # noqa: BLE001
                user = pw = ""
            if user == "admin" and hmac.compare_digest(pw.encode(), self.password.encode()):
                return True
        self.reply(401, {"error": "password needed"}, extra=[("WWW-Authenticate", 'Basic realm="Kodak scan station", charset="UTF-8"')])
        return False

    def same_origin(self):
        """Changes are only accepted from this page itself, not from another site open in the browser."""
        if self.headers.get("X-Requested-With") != "kodak-web" or \
                not self.headers.get("Content-Type", "").startswith("application/json"):
            return False
        origin = self.headers.get("Origin")
        return origin is None or urllib.parse.urlsplit(origin).netloc == self.headers.get("Host")

    def do_GET(self):  # noqa: N802
        if not self.authorised():
            return
        path = urllib.parse.urlsplit(self.path).path
        if path in ("/", "/index.html"):
            self.reply(200, (HERE / "web" / "index.html").read_bytes(), "text/html; charset=utf-8")
        elif path == "/app.js":
            self.reply(200, (HERE / "web" / "app.js").read_bytes(), "text/javascript; charset=utf-8")
        elif path == "/api/settings":
            self.reply(200, self.cfg.view())
        elif path == "/api/state":
            self.reply(200, station_state(self.cfg))
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):  # noqa: N802
        if not self.authorised():
            return
        if not self.same_origin():
            return self.reply(403, {"error": "request not accepted"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= MAX_BODY:
                raise ValueError
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError
        except ValueError:
            return self.reply(400, {"error": "bad request"})
        path = urllib.parse.urlsplit(self.path).path
        try:
            if path == "/api/settings":
                notes = self.cfg.save(body)
                log.info("settings saved from %s", self.address_string())
                self.reply(200, {"ok": True, "notes": notes, "settings": self.cfg.view()})
            elif path == "/api/test-paperless":
                ok, text = test_paperless(self.cfg, body.get("url"), body.get("token"))
                self.reply(200, {"ok": ok, "message": text})
            else:
                self.reply(404, {"error": "not found"})
        except Invalid as e:
            self.reply(422, {"error": str(e)})
        except (TypeError, ValueError, AttributeError, KeyError) as e:
            self.reply(422, {"error": f"The settings could not be read ({type(e).__name__})"})
        except OSError as e:
            log.error("saving failed: %s", e)
            self.reply(500, {"error": f"Could not write the settings: {e.strerror}"})


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, handler):
        if ":" in addr[0]:
            self.address_family = socket.AF_INET6
        super().__init__(addr, handler)


def load_password(path):
    """The page's password; made once if the file does not exist."""
    path = pathlib.Path(path)
    if not path.is_file() or not path.read_text().strip():
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_urlsafe(9) + "\n")
        log.info("created a password for the settings page in %s", path)
    return path.read_text().strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default=str(ETC / "config.yaml"))
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    cfg = Config(args.config)
    web = {**DEFAULTS, **(cfg.load().get("web") or {})}
    Handler.cfg = cfg
    Handler.password = load_password(web["password_file"]) if web["auth"] else None
    if Handler.password is None:
        log.warning("the settings page runs WITHOUT a password (web.auth: false)")
    srv = Server((str(web["bind"]), int(web["port"])), Handler)
    log.info("settings page on http://%s:%d/ (user admin)", web["bind"], int(web["port"]))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
