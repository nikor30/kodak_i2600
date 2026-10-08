# Scan station: Kodak i2600 → Paperless-ngx (Phase 5)

Put paper into the feeder and it gets scanned, turned into one PDF per
stack, and uploaded to Paperless-ngx. Scans are spooled on the Pi, so nothing
is lost while Paperless is down.

```
feeder ─USB─ Kodak vendor driver (x86_64) under box64, in /opt/kodak-x86
                └─ saned 127.0.0.1:6566              kodak-saned.service
                     │ SANE net protocol
             kodak_scand.py (native, arm64)          kodak-scand.service
               poll sane_start every 2 s → pages → img2pdf → spool/outbox
               upload thread → POST /api/documents/post_document/
```

Everything above saned talks only SANE, so the vendor driver can later be
replaced by the native backend without touching this service.

## Two stations, one at a time
| | `kodak-native.service` (native driver) | `kodak-scand` + `kodak-saned` (vendor driver under box64) |
|---|---|---|
| Trigger | **Start button**; ▲/▼ pick the profile, the scanner's LCD shows its label | paper in the feeder (2 s poll) |
| Scan modes | always color 300 dpi duplex; gray, black/white, simplex and blank removal in software | whatever the vendor backend offers |
| Idle cost | one small Python process, no emulation | saned under box64, polled every 2 s |
| After a scanner power cycle | loads the firmware natively if a local power-up file exists (see below; 10 s, worked on the first real power cycle), otherwise or on failure runs the vendor driver once (~25 s), then native | – |
| Status | new (2026-10-08), color/gray/bw at 300 dpi only | proven, but goes stale after idle time (F-038, F-054) |

Switch: `sudo DRIVER=native ./install.sh` or `sudo ./install.sh` (vendor). Both use the same config,
spool, uploader and OLED status. They never run together (`Conflicts=`).

### Station on our SANE backend (`kodak-sane.service`)
`sudo DRIVER=sane ./install.sh` builds and installs the C backend `kodak_i2x00` (`backend/`) and runs
`kodak_sane.py`, which is a plain SANE client: Start button, function number, paper and cover come
from the backend's sensor options, labels and the power-up file are handed to the backend through a
generated SANE config (`/run/kodak-scan/sane.d`), and the station adds blank-side removal,
black/white, PDF, spool and upload. It uses the same config file and the `native:` section
(`trigger`, `functions`) as kodak-native. Logs: `journalctl -u kodak-sane -f`.
It needs `/etc/kodak-scan/firmware/powerup.seq` to survive a scanner power cycle on its own (the
installer converts an existing `powerup.json`); without it the vendor driver is run once instead.
Back to the Python driver: `sudo DRIVER=native ./install.sh`.

### Settings page (`kodak-web.service`, port 2600)
`http://<address of the Pi>:2600/`, user `admin`, password in `/etc/kodak-scan/web-password` (made at the
first start; change it by editing that file and `systemctl restart kodak-web`).
- **Station**: what the station is doing, what is selected on the scanner, the last scans and whether they are uploaded.
- **Paperless-ngx**: address and API token, with a connection test. The token is written to
  `/etc/kodak-scan/paperless-token` (mode 600) and never sent back to the browser.
- **Network share (SMB)** and **E-mail**: a share (`//server/share`, optional folder, user, password) and a mail
  server (host, port, STARTTLS/SSL, user, password, sender, recipient), each with a test button. The passwords go
  to `/etc/kodak-scan/smb-password` and `smtp-password` (mode 600) and are never sent back.
- **Buttons**: which profile is on function numbers 1–7, whether a scan starts on Start or when paper is inserted,
  whether the scanner's display shows page counts, date and time, and after how many idle minutes the station
  leaves the scanner alone.
- **Profiles**: name, text on the scanner's display, colour mode, both sides, blank sides, JPEG quality or
  black/white threshold, where the PDF goes (Paperless, share or e-mail, with an own recipient if wanted),
  document title / file name and tags.
- **Statistics**: pages and scans today, in the last 7 and 30 days and in total, pages per day, per profile and
  per destination (kept in `/var/lib/kodak-scan/stats.json`).

Saving rewrites `/etc/kodak-scan/config.yaml` (comments are kept; the file from before the first change is
`config.yaml.bak-web`). `kodak-sane` notices the change and restarts itself when idle (about 6 s); the other
two stations need `systemctl restart`. Settings in the `web:` section of the config: `port` (2600), `bind`
(`0.0.0.0`), `auth` (`true`). The page is plain HTTP with one password: for a home network, not the internet.

### Destinations
Every profile has a `destination`: `paperless` (default), `smb` or `email`. A finished job waits in the spool until
its destination took it; a destination that is down does not hold up the others. Files on the share are named after
the document title and never overwritten (a second file with the same title gets the job id added). E-mail sends
the PDF as an attachment; a PDF over `email.max_mb` (20) is moved to `failed/` instead of being retried.
The share is written with `smbclient`, so no mount is needed.

### Display and standby
With `native.display_info` the scanner's display shows, under the profile's text, `Today N  Total N` (pages) and
the date and time; the selected function is refreshed every minute and all of them after a scan.
After `native.standby_after` idle minutes (15; 0 = never) the station asks the backend to be quiet: nothing is sent
to the scanner any more, the clock line is removed (it would stand still), and the scanner is free to enter its own
power saving. Start, ▲/▼, new paper or the cover end the rest. The station cannot put the scanner to sleep itself:
the scanner's power requests are not decoded.

### Native power-up (optional)
After power-on the scanner has only a boot firmware; the host must load the rest. The native
service can replay the vendor driver's own initialisation from a file you make locally once:

1. Stop the service, start `sudo tools/usbcap/usbmon_capture.py -o CAPTURE.pcap` (`--bus N` for the scanner's bus) and wait
   until it reports that it is running.
2. Switch the scanner off and on, then let the vendor driver open it once
   (`scanimage -A` under box64, or start the service again and let its fallback do it).
3. Stop the capture (it should report 0 dropped events) and run
   `tools/usbcap/extract_powerup.py CAPTURE.pcap /etc/kodak-scan/firmware/` as root; keep the
   directory `0700`.

`powerup.bin` **contains Kodak's firmware: keep it on your machine, never commit or share it.**
Without the file, or if the replay fails, the service falls back to the vendor driver, so the
setup in `pi/phase1/` stays a prerequisite. Path: `native.powerup` in the config.

### Native station
```
Start button ─ interrupt event ─ kodak_native.py              kodak-native.service
   kds_scan.py   replay sequences/color300-duplex.json, read both image pipes, cut into pages
   kds_image.py  find sheet, deskew, crop, colour-correct, drop blank sides (worker processes)
   → spool/work → img2pdf → spool/outbox → the same upload thread as kodak-scand
```
- `native.functions` in the config maps LCD function numbers to profiles; a profile's `label:` is the
  text on the scanner's LCD (uploaded every time the service connects; the scanner forgets it at power-off).
- `native.trigger: paper` scans as soon as paper is inserted instead of waiting for Start.
- Start with an empty feeder or an unassigned number shows a short message on the OLED and does nothing.
- Protocol and processing are described in `docs/protocol/commands.md` and `image-processing.md`.

## Install
Prerequisites: `pi/phase1/setup-x86-chroot.sh` and `pi/phase1/setup-box64.sh`.

```bash
sudo ./install.sh
sudoedit /etc/kodak-scan/config.yaml          # paperless.url, profile, tags
sudoedit /etc/kodak-scan/paperless-token      # the API token only (Paperless: profile → "API Auth Token")
sudo systemctl restart kodak-scand
journalctl -u kodak-scand -f
```

## Paperless permissions
Uploading only needs the "add document" permission. **Tag names** in a profile are
resolved via `/api/tags/`, so the token's user also needs **view** permission on tags
(otherwise use tag ids). The uploaded documents belong to the token's user.

## Use
- Put the stack into the feeder. Within about 2 s the scanner starts pulling it in.
- One stack becomes one PDF. Blank sides are dropped by the driver (`blankimagemode: content`).
- The title is `Scan <date time>`; the tags come from the active profile.
- The active profile is `profile:` in the config (`color300`, `gray300`, `bw300`). Restart the service after a change.

## Status display (PoE HAT (B) OLED)
`kodak-oled.service` drives the HAT's 128×32 SSD1306 display (I2C bus 1, `0x3c`). `install.sh`
enables I2C if needed.

| line | content |
|---|---|
| top | the Pi's IP address, or `no network` |
| bottom | `Scanner starting…`, `Ready`, `Ready · N to upload`, `Scanning page N` (with a moving bar), the scanner's error text, or `Scan service off` |

After 30 s of `Ready` with nothing to upload, a screensaver takes over (a starfield with the IP,
CPU temperature and fan state bouncing over it); it ends as soon as a scan starts, an upload is queued, or an error occurs.
Switch it off with `screensaver: false`.

kodak-scand publishes its state in `/run/kodak-scan/status.json`; the display service only reads
that file. Settings: the optional `oled:` section in the config (`rotate: 180` if the text is
upside down, `contrast`, `screensaver`, `screensaver_after`), then `systemctl restart kodak-oled`.

## Fan (PoE HAT (B))
The HAT's fan can only be switched on or off (one pin of the PCF8574 at `0x20`; no PWM).
`kodak-oled.service` runs it as a thermostat: on at `fan.on_temp` (60 °C CPU), off at
`fan.off_temp` (50 °C). The fan is switched on when the service stops, and it stays on if the
temperature cannot be read. `fan.enabled: false` leaves the fan alone.

## Spool (`/var/lib/kodak-scan`, i.e. `/var/lib/private/kodak-scan`)
| dir | content |
|---|---|
| `work/<job>/` | pages of the stack being scanned; recovered as "(incomplete)" after a crash |
| `outbox/` | PDFs waiting for upload (retried every `retry_interval` s) |
| `sent/` | uploaded PDFs, deleted after `keep_sent_days` |
| `failed/` | PDFs Paperless rejected (HTTP 400/413/415); check the log |

## Self-recovery
After 3 scanner errors in a row kodak-scand exits with status 75 and systemd restarts
`kodak-saned` (and with it kodak-scand), at most once per 10 minutes. This covers the case where
the vendor driver inside a long-running saned no longer opens the device (F-038).

## Limits (for now)
- The Start button and the LCD function number are not visible through the vendor SANE
  backend (Q-022). That's why scanning starts on paper detection, and the profile is set in the config.
- The vendor driver's open takes ~15 s, so the service keeps the device open.

## Remove
`sudo ./install.sh --remove` (keeps the config and the spool).
