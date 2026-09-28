# Decisions (ADR log)

Format: ID, date, context, decision, consequences. Never delete; supersede with a new ADR.

## ADR-001: Three-path strategy (2026-09-28)
- **Context:** no ARM driver exists; a full clean-room driver takes months.
- **Decision:** A) vendor x86 driver via box64 on the Pi as the interim solution; B) protocol capture/RE; C) native SANE backend.
- **Consequences:** the Paperless pipeline can be built early; the driver must stay swappable behind SANE.

## ADR-002: Linux vendor driver is the primary RE target, Windows is secondary (2026-09-28)
- **Context:** Linux `.so` files are easier to analyse (ELF, likely symbols, libusb), and usbmon/LD_PRELOAD make tracing cheap.
- **Decision:** start RE on the Linux driver; use the Windows driver only for features missing on Linux.

## ADR-003: Git-versioned project memory (2026-09-28)
- **Decision:** `CLAUDE.md` + `memory/*.md` are the long-term memory. They are updated and committed every session (`memory:` prefix).

## ADR-004: Clean-room separation & no vendor code in git (2026-09-28)
- **Decision:** `vendor/` is git-ignored; `re/` holds facts/notes only; `backend/` is written from `docs/protocol/` only.

## ADR-005: Capture tool is a libopenusb shim, not a libusb LD_PRELOAD (2026-09-28)
- **Context:** the vendor driver does not use libusb. It `dlopen`s libopenusb from `/usr/local/lib{,64}/libopenusb.so` (F-012).
- **Decision:** `tools/openusb-logger/` will be a drop-in `libopenusb.so` that logs every `openusb_*_xfer`/`xfer_aio` (pipe, endpoint, direction, length, hex payload, timestamp) and forwards to the real library. usbmon + Wireshark stays as the independent cross-check.
- **Consequences:** logs carry driver-level context (which pipe: BulkOut/ImageFront/…), not just raw URBs.

## ADR-006: Vendor installer is not run on target systems (2026-09-28)
- **Context:** postinst edits `50-udev-default.rules`, sets MODE 666 udev rules, and runs `chmod -R 777 /var/kodak`.
- **Decision:** on the Pi and on reference machines, unpack the `.deb` files manually and install our own minimal udev rule. Use a VM or a throw-away install if the vendor `setup` is ever needed.
