# kodak_i2600

Open-source Raspberry Pi support for the **Kodak i2600** document scanner
(USB `040a:601d`): headless scanning into **Paperless-ngx**, and later driven
by the scanner's own buttons and panel.

## Status

**Working today:** put paper into the feeder → the Pi scans it → one PDF per
stack lands in Paperless-ngx.

| | |
|---|---|
| Driver | Kodak's x86_64 Linux driver v4.14, run on the Pi (aarch64) with **box64** |
| Speed | 3 duplex sheets in ~27 s (gray 300 dpi), ~30–35 s in color |
| Trigger | automatic when paper is loaded (the Start button and LCD are not usable yet) |
| Delivery | Paperless-ngx REST API, with a local spool and retry while Paperless is offline |
| Native driver | not started yet (Phases 2–4: USB protocol → our own SANE backend) |

Details and the next steps: [`memory/STATUS.md`](memory/STATUS.md) · the full plan: [`PLAN.md`](PLAN.md)

## How it works

```
Kodak i2600 ──USB── Raspberry Pi 4 (Debian trixie, aarch64)
                    ├─ /opt/kodak-x86   Debian bookworm amd64 chroot
                    │    Kodak driver (unpacked from the vendor .debs, not committed)
                    │    └─ saned 127.0.0.1:6566, run by box64       kodak-saned.service
                    └─ kodak_scand.py (native Python, SANE "net")    kodak-scand.service
                         paper detected → pages → PDF → spool → Paperless API
```

Everything above `saned` talks only SANE. When the native backend exists, it
replaces the vendor driver without changes to the scan service.

## Setup on a Raspberry Pi

Needs a Pi 4-class board with a 64-bit OS and a 4K-page kernel (a Pi 5 needs `kernel8.img`), plus internet access.

```bash
git clone https://github.com/nikor30/kodak_i2600.git && cd kodak_i2600

# 1. x86_64 chroot with the Kodak driver (downloaded from Kodak Alaris, not stored here)
sudo pi/phase1/setup-x86-chroot.sh
sudo pi/phase1/setup-x86-chroot.sh --diagnose        # scanner found? options listed?

# 2. box64, built from source with our patch (~20–40 min)
sudo pi/phase1/setup-box64.sh

# 3. scan station services
sudo pi/scan-station/install.sh
sudoedit /etc/kodak-scan/config.yaml                  # paperless.url, profile, tags
sudoedit /etc/kodak-scan/paperless-token              # the API token only
sudo systemctl restart kodak-scand
journalctl -u kodak-scand -f                          # load paper and watch
```

More detail: [`pi/phase1/README.md`](pi/phase1/README.md) (driver, qemu/box64, diagnostics) and
[`pi/scan-station/README.md`](pi/scan-station/README.md) (profiles, spool, Paperless permissions).

## Repository layout

| Path | Content |
|---|---|
| `pi/phase1/` | x86 chroot + Kodak driver setup, box64 build and patch, diagnostics |
| `pi/scan-station/` | scan service, systemd units, installer, config example |
| `memory/` | project memory: status, journal, findings (with sources), decisions (ADRs), open questions |
| `docs/` | hardware info and the protocol spec (the basis for the clean-room backend) |
| `re/`, `captures/`, `tools/` | reverse-engineering notes, USB captures, our tooling |
| `backend/` | native SANE backend (not started yet) |

## Known limitations
- The **Start button and LCD function number** are not exposed by Kodak's SANE backend. Profile selection is in the config for now; panel support is planned for the native backend.
- The vendor driver is closed source and x86-only; this setup depends on emulation (box64, or qemu as a slower fallback).

## Legal
Reverse engineering here is for interoperability only. No vendor binaries,
firmware or decompiled code are stored in this repository. The Kodak driver
is downloaded from Kodak Alaris at setup time.
