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

## Use
- Put the stack into the feeder. Within about 2 s the scanner starts pulling it in.
- One stack becomes one PDF. Blank sides are dropped by the driver (`blankimagemode: content`).
- The title is `Scan <date time>`; the tags come from the active profile.
- The active profile is `profile:` in the config (`color300`, `gray300`, `bw300`). Restart the service after a change.

## Spool (`/var/lib/kodak-scan`, i.e. `/var/lib/private/kodak-scan`)
| dir | content |
|---|---|
| `work/<job>/` | pages of the stack being scanned; recovered as "(incomplete)" after a crash |
| `outbox/` | PDFs waiting for upload (retried every `retry_interval` s) |
| `sent/` | uploaded PDFs, deleted after `keep_sent_days` |
| `failed/` | PDFs Paperless rejected (HTTP 400/413/415); check the log |

## Limits (for now)
- The Start button and the LCD function number are not visible through the vendor SANE
  backend (Q-022). That's why scanning starts on paper detection, and the profile is set in the config.
- The vendor driver's open takes ~15 s, so the service keeps the device open.

## Remove
`sudo ./install.sh --remove` (keeps the config and the spool).
