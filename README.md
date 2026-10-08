# kodak_i2600

Open-source Raspberry Pi support for the **Kodak i2600** document scanner
(USB `040a:601d`): press Start on the scanner and the stack lands as one PDF in
**Paperless-ngx**. The scanner is driven by this project's own SANE backend; no
vendor software runs while it scans.

## Status

Tested on one unit (i2600, firmware 1.2.15) on a Raspberry Pi 4. Experimental.

| | |
|---|---|
| Driver | our own C SANE backend `kodak_i2x00` (`backend/`), written from the protocol documentation in `docs/protocol/` |
| Trigger | the scanner's **Start button**; ▲/▼ choose a profile, the scanner's display shows its name |
| Modes | colour, gray, black/white at 300 dpi; both sides or front only; blank sides left out |
| Speed | 6 s from Start to the uploaded PDF for one sheet, 9 s for three double-sided sheets |
| Image | deskewed, cropped to the sheet, colour-corrected |
| Delivery | Paperless-ngx REST API, with a local spool and retry while Paperless is offline |
| Power cycle | the station brings the scanner back by itself in about 23 s (needs a local power-up file, see below) |
| Settings | a web page on the Pi (port 2600): Paperless address and token, buttons, profiles |

Details and next steps: [`memory/STATUS.md`](memory/STATUS.md) · the plan: [`PLAN.md`](PLAN.md) ·
releases: <https://github.com/nikor30/kodak_i2600/releases>

## How it works

```
Kodak i2600 ──USB── Raspberry Pi 4 (Debian trixie, aarch64)
                    ├─ libsane-kodak_i2x00.so     our SANE backend (C, libusb)
                    │    power-up · Start button, function number, paper, cover as sensors
                    │    display labels · scan · deskew, crop, colour
                    ├─ kodak_sane.py              kodak-sane.service  (a plain SANE client)
                    │    Start → profile → pages → blank removal → PDF → spool → Paperless API
                    ├─ kodak_web.py               kodak-web.service   settings page, port 2600
                    └─ kodak_oled.py              kodak-oled.service  status on the PoE HAT display (optional)
```

Everything above the backend talks only SANE. Two earlier stations stay installed as
fallbacks: `kodak-native` (a Python driver, same features) and `kodak-scand` + `kodak-saned`
(Kodak's x86_64 driver under box64, triggered by inserting paper).

## Setup on a Raspberry Pi

Needs a Pi 4-class board with a 64-bit OS and a 4K-page kernel (a Pi 5 needs `kernel8.img`), plus internet access.

```bash
git clone https://github.com/nikor30/kodak_i2600.git && cd kodak_i2600

# 1. Kodak's own driver in an x86_64 chroot + box64 (downloaded from Kodak Alaris, not stored here).
#    Still needed once: to make the power-up file, and as the fallback after a power cycle.
sudo pi/phase1/setup-x86-chroot.sh
sudo pi/phase1/setup-x86-chroot.sh --diagnose        # scanner found? options listed?
sudo pi/phase1/setup-box64.sh                        # builds box64 with our patch (~20–40 min)

# 2. The station on our SANE backend (builds, self-tests and installs the backend)
sudo DRIVER=sane pi/scan-station/install.sh
journalctl -u kodak-sane -f                          # load paper, press Start and watch

# 3. Settings: open http://<address of the Pi>:2600/   user admin
sudo cat /etc/kodak-scan/web-password                # the password, made at the first start
```

On the settings page: enter the Paperless address and API token (**Test connection** checks
both), choose which profile is on which function number, and edit the profiles (text on the
scanner's display, colour mode, sides, blank sides, title and tags in Paperless). Saving
restarts the station with the new settings. The same settings are in `/etc/kodak-scan/config.yaml`
if you prefer an editor.

### Power-up file (so the station survives a scanner power cycle on its own)
After power-on the scanner has only a boot firmware; the host must load the rest. The backend
does that from a file you make once from **your own** USB capture of Kodak's driver starting the
scanner (steps in [`pi/scan-station/README.md`](pi/scan-station/README.md), "Native power-up").
The file contains Kodak's firmware: it stays on your machine and is not part of this repository.
Without it the station runs Kodak's driver once after each power cycle instead.

More detail: [`pi/scan-station/README.md`](pi/scan-station/README.md) (stations, profiles, spool,
settings page), [`backend/README.md`](backend/README.md) (the SANE backend, `scanimage` use, tests),
[`pi/phase1/README.md`](pi/phase1/README.md) (vendor driver, box64, diagnostics).

## Repository layout

| Path | Content |
|---|---|
| `backend/` | the SANE backend `kodak_i2x00` in C, with offline tests (`make check`) |
| `pi/scan-station/` | station services, settings page, systemd units, installer, config example |
| `pi/phase1/` | x86 chroot + Kodak driver setup, box64 build and patch, diagnostics |
| `docs/protocol/` | the USB protocol as far as it is known: the only source the backend is written from |
| `tools/` | USB capture and decoding (`usbcap`), a protocol probe (`kdsprobe`) |
| `memory/` | project memory: status, journal, findings (with sources), decisions (ADRs), open questions |
| `re/`, `captures/` | reverse-engineering notes; index of USB captures (the captures themselves stay local) |

## Known limitations
- **One unit, one mode on the wire.** A scan is started by replaying a request sequence captured from one i2600 in colour 300 dpi duplex; gray, black/white and front-only are derived in software. Other resolutions are not supported, and whether the sequences work on other units or on the i2400/i2800 is not known.
- **Pages are stored as fed.** A sheet loaded bottom edge first comes out upside down.
- **The scanner feeds a whole stack by itself.** A SANE frontend that scans one page and closes loses the rest; use batch mode (`scanimage --batch`). The station does.
- **Display text is plain ASCII** (no umlauts).
- Deskew has only seen nearly straight sheets on the device; cancelling a scan inside a page is untested.
- The settings page is protected by one password over plain HTTP: use it on a network you trust.
- Kodak's closed, x86-only driver is still needed once for setup (see above).

## Legal
Reverse engineering here is for interoperability only. No vendor binaries,
firmware or decompiled code are stored in this repository. The Kodak driver
is downloaded from Kodak Alaris at setup time; the power-up file is made
locally by the user and must not be redistributed.
