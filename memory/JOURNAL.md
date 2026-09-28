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

## 2026-09-28: trace of the open hang
- `--trace-open` from the Pi: exit 124 after 180 s, usbmon empty, no dmesg change. The driver spawns lexexe+hippo.so over POSIX mqueues; hippo spins right after reading the ARM /proc/cpuinfo (F-026). Disassembly: `CTimingInfo::ComputeProcessorClockSpeed()` busy-waits on `clock_gettime` (ruled out: no clock_gettime syscalls in the trace); the cpuinfo readers are `COsCfgImpl::LoadSystemInfo()` and `boost::thread::physical_concurrency()`.
- Fix under test (ADR-008): fake x86 cpuinfo bind-mounted inside the chroot. Verified on x86: the chroot sees it, the host cpuinfo is untouched, and umount is clean.
- Owner should kill leftover spinning `lexexe` processes from earlier hung runs (timeout only kills scanimage).

## 2026-09-28: fake cpuinfo verified on the Pi (open hang fixed)
- Ran directly on the Pi this time (Claude session on `scannstation`). No leftover lexexe processes. Re-ran setup (it installed the fake-cpuinfo mount), then `--trace-open`, `--diagnose`, and `--diagnose --scan`.
- `--trace-open`: `scanimage -A` exit 0 after 34 s (was: hang until the 180 s timeout). Full option list, 17 options, no button/panel options (F-027).
- usbmon: first real protocol traffic. ~5.7 MB bulk OUT on EP 0x02 during open, then 53,856 B IN on each image pipe 0x82/0x86 (F-028, Q-021). qemu passes bulk URBs (Q-018).
- `--diagnose`: every step passes; `scanimage -A` takes 19 s. `--scan` with an empty feeder: "Document feeder out of documents" after 20 s, clean exit.
- Closed Q-020. New Q-021 (bulk-OUT block), Q-022 (buttons not exposed via SANE). Results in `pi/phase1/phase1-trace-20260928-205802`, `phase1-results-20260928-205858`, `-205941` (git-ignored).
- Next: real page scan with a sheet loaded.

## 2026-09-28: first successful scan on the Pi
- Owner loaded a sheet. The first attempt still said "out of documents" (USB trace: control-pipe status polling only, no feed). Owner then confirmed LED solid green, LCD `1`, paper loaded, and the retry **scanned**: 52 s, 20 MB raw per side over EP 0x82/0x86, 2476×3503 gray TIFF (F-029).
- Bug in `--diagnose --scan`: `--batch-count` puts scanimage in batch mode, so stdout stays empty and the image lands in `out1.tif` in the chroot's cwd, and only 1 of the 2 duplex images is kept. Fixed: explicit `--batch=/tmp/scan-gray300-%d.tiff --duplex both`.
- Saved image is almost white, so it is probably the blank side; to be re-checked with a printed page.
- Noticed bcdDevice 1.02 → 2.01 across the first open's 5.7 MB bulk-OUT: probably a runtime firmware load on each power-up (F-030, Q-021).

## 2026-09-28: duplex batch scan verified
- With the fixed `--diagnose --scan`: 2 printed sheets → 4 gray 300 dpi TIFFs (3 with content, 1 blank back) in 80 s including the open, ≈30 s/sheet (F-031). Phase 1's exit criterion (vendor driver scans on the Pi) is met with qemu; speed is modest but usable.
- Images stay local (git-ignored results dir).

## 2026-09-28: box64 port (owner chose "speed first")
- Debian trixie's box64 needs glibc 2.39 > bookworm's 2.36, so it's built from upstream v0.4.4 in a throw-away arm64 bookworm chroot (`setup-box64.sh`, ADR-009) and installed into the Kodak chroot with arm64 multiarch runtime libs. `EMU=box64 ./setup-x86-chroot.sh --diagnose` runs the same tests under box64; results dirs are now `phase1-results-<qemu|box64>-*`.
- `deviceprobe` worked at once; `scanimage -L`/`-A` segfaulted. Root cause: the vendor code treats dlopen handles as `struct link_map*` (F-032). Wrote a box64 patch (`box64-dlopen-null-linkmap.patch`) → open works, 14.7 s vs 19 s under qemu.
- Also: `gh` 2.46 installed on the Pi from Debian (owner request; needs `gh auth login`).
- Next: scan speed under box64 (needs paper), check whether lexexe/hippo children run under box64 or fall back to qemu.
- Follow-up: the box64 open failed at random. Cause: stale link_map entries after kds.ds dlclose/dlopen cycles (F-033). Extended the patch; now stable.
- **box64 scan: 3 duplex sheets in 27 s including the open, vs qemu 2 sheets in 80 s (F-034).** Image stats identical to qemu for the same sheets.

## 2026-09-28: Phase 5 scan station (first version)
- Owner's choices: REST API, color 300 dpi duplex, auto-start on paper. Design ADR-010.
- Feasibility: saned under box64 plus python3-sane via the net backend works; NO_DOCS polling is instant (F-035).
- Wrote `pi/scan-station/` (kodak_scand.py, units, install.sh, config example, README). Installed on the Pi; both services active, polling, ~0.5 % CPU idle. Upload not tested yet (no Paperless URL/token).
- Owner gave the Paperless address `http://192.168.10.242:8000` (the DNS name `paperless.niko.de` → 192.168.100.11 was unreachable from the Pi's WLAN) and the token (stored in /etc/kodak-scan/paperless-token, 600). The 3 spooled stacks uploaded at once (F-036). The token user can't read tags/tasks/documents (403).
