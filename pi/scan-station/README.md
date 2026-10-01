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
enables I2C if needed. The HAT's fan controller (`0x20`) is not touched.

| line | content |
|---|---|
| top | the Pi's IP address, or `no network` |
| bottom | `Scanner starting…`, `Ready`, `Ready · N to upload`, `Scanning page N` (with a moving bar), the scanner's error text, or `Scan service off` |

After 30 s of `Ready` with nothing to upload, a screensaver takes over (a starfield with the IP
bouncing over it); it ends as soon as a scan starts, an upload is queued, or an error occurs.
Switch it off with `screensaver: false`.

kodak-scand publishes its state in `/run/kodak-scan/status.json`; the display service only reads
that file. Settings: the optional `oled:` section in the config (`rotate: 180` if the text is
upside down, `contrast`, `screensaver`, `screensaver_after`), then `systemctl restart kodak-oled`.

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
