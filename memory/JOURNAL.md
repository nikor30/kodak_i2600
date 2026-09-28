# Journal (append-only, newest at the bottom)

## 2026-09-28: project kickoff
- Desk research: the i2600 (`040a:601d`) has no open-source driver; vendor Linux driver is x86/x86_64 only; `sane-kodak` covers only older SCSI/1394 models (a possible protocol reference).
- Created `PLAN.md` (6 phases, three-path strategy), the repo skeleton, and the git-based memory (`CLAUDE.md`, `memory/`).
- Next: Phase 0 hardware inventory.

## 2026-09-28: Phase 0 started
- Owner supplied `lsusb`: `040a:601d` on Bus 001 Device 003 (F-009).
- Downloaded Kodak Linux v4.14 (x86_64 + i586) into `vendor/`; unpacked and statically inventoried the x86_64 package → `re/linux-driver/inventory.md`.
- Key results: debug symbols present (F-011); libopenusb via dlopen (F-012, closes Q-003); one driver for 6 models incl. `29cc:100a/b/c` (F-013, closes Q-009); pipe map (F-014); OCP get/set API (F-015); SCSI hypothesis now unlikely (F-016).
- New ADRs: ADR-005 (libopenusb logging shim), ADR-006 (never run the vendor installer on targets).
- Added `tools/phase0/collect-hw-info.sh`, `docs/protocol/transport.md`, `docs/hardware/lsusb.txt`.
- Not done: Windows driver inventory (no 7z/innoextract in this environment).

## 2026-09-28: USB descriptors from the real unit
- Owner ran `tools/phase0/collect-hw-info.sh` on the Pi (`scannstation`, aarch64, kernel 6.18.50+rpt-rpi-v8). Output committed to `docs/hardware/hwinfo-20260928/`.
- 5 endpoints: 0x02 bulk OUT, 0x82/0x86 bulk IN, 0x81/0x88 interrupt IN (8 B, 64 ms). This matches the vendor pipe map one-to-one (F-017); closes Q-001.
- bcdDevice 2.01; USB serial all zeros (F-018). No kernel driver binds (vendor class), so userspace access via usbfs is clean.
- Idea: if box64 runs the vendor driver on the Pi, the Pi is also the capture host (Q-017).

## 2026-09-28: Phase 1 started
- Dry run on this x86_64 container: manual install of the vendor stack; the whole SANE→TWAIN→kds.ds chain loads, and discovery runs via the `deviceprobe_i2000` subprocess (F-020).
- Incident: `dpkg-deb -x libudev0.deb /` replaced the container's `/lib` symlink with a directory (F-021). The container is disposable; a safety check blocked the repair (`rmdir /lib`), so it was left as is. The Pi recipe avoids this with a staged tar merge.
- Wrote `pi/phase1/setup-x86-chroot.sh` + README (ADR-007): bookworm amd64 chroot, safe merge, SANE limited to kds_i2000, `kodak-x86` helper, `--diagnose [--scan]` results tarball. Verified end to end on x86_64: builds, idempotent, `/lib` intact, mounts released.
- Next: owner runs it on the Pi.

## 2026-09-28: first Pi run of Phase 1
- Owner ran setup + `--diagnose` + `--diagnose --scan` on the Pi 4 (trixie). Chroot, qemu binfmt, USB visibility and Kodak device enumeration all work; `scanimage -L` finds the scanner (F-022).
- Open/scan failed: `devicemanager.so` needs libpango/libpangocairo, which were missing in the minbase chroot (the x86 dry run had them on the host, so it did not catch this).
- Fix: chroot now installs pango/cairo/glib + xdg-user-dirs; new `check_deps` (ldd over every Kodak lib) runs at setup (fatal) and in `--diagnose` (`00-deps.txt`). Verified on x86: the check flags the gap, and after the fix the open gets past `Data->Init` to device discovery.
- dmesg showed two USB disconnect/re-enumerations of the scanner around the test (cause unknown: replug, or a reset by the driver). Watch for it in the next run.

## 2026-09-28: mount propagation bug (host /dev/pts lost)
- Owner: `sudo ./setup-x86-chroot.sh` → "sudo: unable to allocate pty: No such device". Cause: my `kodak-x86 umount` (`umount -R` on rbind mounts) propagated to the host's shared mounts (F-023).
- Fix: rslave on every bind mount, and rslave-before-umount (which also covers mounts from the old version). Verified: shared-tmpfs reproduction (old: submount lost; new: kept) plus a full setup→scanimage→umount cycle with the host /dev,/sys mount table unchanged.
- Owner recovery: reboot the Pi (restores /dev/pts and any /sys submounts), then git pull and re-run.
- Lesson: any script that bind-mounts host trees must use rslave; test mount code on a host with shared propagation.

## 2026-09-28: second Pi run: hang at scanimage -A
- Owner reports that `--diagnose` hangs at 05-scanimage-A (the open now gets further than the pango failure). Photos of the panel/back received (F-024, closes Q-010). Photos were not committed because they show a private document.
- Added `--snapshot` (second-terminal, read-only state dump) and `--trace-open` (QEMU_STRACE syscall trace + usbmon text capture of the scanner bus during one `scanimage -A`), and cut the 05 timeout to 180 s. Fixed a pipefail exit when the scanner is absent. `.gitignore` covers all phase1-* outputs.
- Hypothesis F-025 (reset + re-enumeration → stale handle); Q-020.
