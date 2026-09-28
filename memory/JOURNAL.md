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
