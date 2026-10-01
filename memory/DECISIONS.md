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

## ADR-007: Phase 1 uses a Debian amd64 chroot + qemu-user first, box64 second (2026-09-28)
- **Context:** the vendor stack hard-codes absolute paths (`/opt/kodak`, `/usr/local/lib/twain`, `/usr/local/lib/libopenusb.so`, `/var/kodak`) and needs a full x86 SANE userland. Box64 running from the host would need those paths on the Pi's root filesystem plus a mixed-arch library setup.
- **Decision:** build an isolated `/opt/kodak-x86` bookworm amd64 chroot (debootstrap) run via qemu-user binfmt. Vendor debs are merged safely (F-021). The chroot SANE loads only `kds_i2000`. Box64 is tried only if qemu is too slow.
- **Consequences:** easy to undo (delete the directory); slower than box64. In Phase 5, native tools can reach it via an x86 `saned` on localhost plus the SANE `net` backend.

## ADR-008: Fake x86 /proc/cpuinfo inside the chroot (2026-09-28)
- **Context:** under qemu-user the vendor's image-processing helper (hippo.so via lexexe) hangs after parsing the host's ARM /proc/cpuinfo (F-026).
- **Decision:** the chroot's own procfs gets an x86-style cpuinfo (one entry per host CPU, `cpu MHz: 1800`, physical/core ids, SSE4.2-level flags), bind-mounted over `$ROOT/proc/cpuinfo` only. It is removed with the chroot's `/proc` on umount.
- **Consequences:** the host is unaffected. If hippo also uses CPUID to choose AVX code paths, qemu's CPU model decides that, not this file.

## ADR-009: box64 built from source, installed into the Kodak chroot (2026-09-28)
- **Context:** qemu-user works but is slow (≈30 s per duplex sheet, ~20 s open, F-031). The owner chose to try box64 for speed before building the Paperless pipeline. Debian trixie's box64 0.3.4 needs glibc ≥ 2.39; the Kodak chroot is bookworm (2.36), and moving the chroot to trixie risks breaking the vendor libs.
- **Decision:** `pi/phase1/setup-box64.sh` builds upstream box64 (pinned tag, `-DRPI4ARM64=1`) in a separate, disposable native arm64 bookworm chroot (`/opt/box64-build`), then installs the binary into `/opt/kodak-x86/usr/local/bin/box64` and adds `libc6:arm64` etc. to the Kodak chroot via multiarch. Use it with `kodak-x86 box64 <cmd>`; plain `kodak-x86 <cmd>` still uses qemu.
- **Consequences:** the host stays untouched (only the two /opt dirs). Both paths coexist, so we can A/B them. Child processes the driver spawns (deviceprobe, lexexe) may still fall back to qemu via binfmt unless box64 intercepts the execve; check this in the first run.

## ADR-010: Scan station = saned (box64, localhost) + native Python daemon, auto-start on paper (2026-09-28)
- **Context:** Phase 5. The vendor SANE backend exposes no button/LCD options (Q-022), and its open takes ~15 s. The owner chose REST API upload, color 300 dpi duplex, and auto-scan on paper.
- **Decision:** `kodak-saned.service` runs `saned -l -b 127.0.0.1` inside the chroot under box64. `kodak-scand.service` (native arm64 Python, python3-sane via the `net` backend, DynamicUser, token via LoadCredential) keeps the device open, polls `sane_start()` every 2 s (NO_DOCS returns immediately, F-035), scans the stack into one PDF (img2pdf) and uploads it via `/api/documents/post_document/` from a disk spool with retry. Profiles live in `/etc/kodak-scan/config.yaml`; blank-page removal is done by the driver.
- **Consequences:** only SANE is between the service and the driver (swappable for the native backend). The trigger moves to the Start button/LCD once those are reachable (native backend, interrupt EPs). The chroot's saned accepts 127.0.0.1 only.

## ADR-011: OLED status display as a separate service fed by a status file (2026-10-01)
- **Context:** the owner's Pi has a Waveshare PoE HAT (B) with an SSD1306 OLED (F-037) and wants the IP and the scan activity on it. kodak-scand runs sandboxed (DynamicUser, PrivateDevices) and should not get I2C access.
- **Decision:** kodak-scand writes its state (`starting`/`ready`/`scanning` + pages/`error`, upload queue) to `$RUNTIME_DIRECTORY/status.json` (`/run/kodak-scan/`). A separate `kodak-oled.service` (root, I2C devices only) renders IP + state with a small built-in SSD1306 driver (smbus2 + PIL, no luma dependency). The fan controller at `0x20` is not touched.
- **Consequences:** the display is optional and independent of the driver path; other consumers (status web page) can read the same file. The Kodak LCD stays unused until Q-022 is solved.
