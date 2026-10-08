# Status

_Last updated: 2026-10-08_

**Current phase:** Phase 2/4 on the Pi itself. **The Pi now runs `kodak-native.service` (ADR-015, F-060)**: native driver, Start button, LCD labels; kodak-scand/kodak-saned (vendor driver under box64) are disabled but installed as the fallback (`sudo ./install.sh` in `pi/scan-station/` switches back, `DRIVER=native` switches forward).
**Confirmed 2026-10-08:** Start → scan → Paperless works end to end (F-061). LCD labels confirmed by the owner (F-062). Gray, b/w, 3-sheet stack and double-sided sheet confirmed (F-063, F-064). **Still to test:** b/w with the new threshold 200 on faint print; behaviour after long scanner idle/sleep: one Start press was lost after ~35 min idle (F-066, Q-029), cause open; the service now logs events and state changes. Power cycle recovery works through the vendor fallback (F-065). **Native power-up works (F-075, ADR-016):** on the power cycle at 15:06 the service loaded firmware and FPGA itself from `/etc/kodak-scan/firmware/powerup.json` in 10.1 s, no vendor fallback; a scan after a native power-up works (F-077). The LCD was blank after the native power-up (F-076); cause found: `16` SetSequenceNumber 1/7 was missing (F-078, owner confirmed the display). kodak-native now sends it when the function number reads 0 (installed 15:17). The automatic path ran on the 15:17 power cycle (F-079): 23 s from power-off to ready; the owner confirmed the display. **The Python station now survives a power cycle on its own.** Released as **v0.2.0** (2026-10-08; v0.1.0 and v0.1.1 earlier the same day): native power-up proven, C SANE backend `kodak_i2x00`.
**Known now:** commands are vendor control requests on EP0 (F-041), 69 request names (F-043), status block incl. paper/cover/function number (F-045, F-051), **button and panel events read natively** (F-051), scan start/stop sequence (F-052), **raw RGB image format** (F-053), LCD bitmap upload (F-046), SetTime (F-050), power-up firmware sequence (F-048).
**First native scan done (F-056):** `tools/kdsprobe/native_scan.py` scans one sheet color 300 dpi duplex by replaying the captured start sequence; raw images only (no crop/deskew), and only on a scanner the vendor driver has initialised since power-up.
**Image processing done (F-059):** `kds_pages.py` turns the raw streams into cropped, deskewed, colour-corrected pages and a PDF, and can queue it in the station's upload spool.
**Not known yet:** the mode-dependent setup registers and config fields (Q-026), page ends / stream length (Q-028), whether the native power-up is repeatable over many power cycles and whether scanning after it behaves the same (Q-027).

## Done
- Phase 0: research, plan, memory, driver inventory (F-010…F-016), real USB descriptors (F-017…F-019).
- Phase 1 (qemu): `pi/phase1/setup-x86-chroot.sh` + README; dry run (F-020), safe deb merge (F-021, ADR-007); fake cpuinfo fixes the open hang (ADR-008, F-027); first USB traffic (F-028, F-030); first scans, ≈30 s per duplex sheet (F-029, F-031).
- Phase 1b (box64): `pi/phase1/setup-box64.sh` builds box64 v0.4.4 with our patch `box64-dlopen-null-linkmap.patch` (F-032, F-033, ADR-009). ≈10× faster than qemu: 3 duplex sheets in 27 s gray (F-034).
- Phase 2 (started 2026-10-08): `tools/usbcap/`, `tools/re/elftables.py`, `tools/kdsprobe/`, `docs/protocol/commands.md`, `re/linux-driver/devicemanager.md` (F-041…F-050, ADR-013, ADR-014).
- Phase 5 (first version): `pi/scan-station/` = kodak-saned (saned under box64, localhost) + kodak-scand (auto-scan on paper → PDF → Paperless REST, spool + retry), ADR-010, F-035. Installed and enabled on the Pi; end to end verified with 3 color stacks uploaded to Paperless at `http://192.168.10.242:8000` (F-036).
- OLED status display on the PoE HAT (B): `kodak-oled.service` + status file from kodak-scand (ADR-011, F-037). Idle screensaver (starfield + bouncing IP). Fan thermostat in the same service (F-040; HAT fan switch must be in the programmable position). Installed 2026-10-01; not yet confirmed visually.

## C SANE backend (N6, started 2026-10-08)
`backend/` builds `libsane-kodak_i2x00.so.1` (ADR-017, F-068). Works on the device: detect, open, options, sensors, empty-feeder `NO_DOCS`, busy detection. Offline tests pass on the saved 3-sheet streams (`make check`). **Real scans done: one sheet (F-069), a 3-sheet stack in color (F-070) and in Gray with all pages spooled to disk (F-071), 6 frames in 9 s.** Not installed system-wide (`make testenv` + `LD_LIBRARY_PATH`/`SANE_CONFIG_DIR`).
B1. (1 sheet and 3 sheets color duplex done, F-069, F-070.) Gray, spooling, Lineart and `ADF Front` done (F-071, F-072). All scan modes of the backend have now run on the device. Procedure: `systemctl stop kodak-native`, owner loads paper without pressing Start, `cd backend && make testenv && LD_LIBRARY_PATH=$PWD/build SANE_CONFIG_DIR=$PWD/build/conf SANE_DEBUG_KODAK_I2X00=3 scanimage -d $(scanimage -f %d) --format=png --batch=/tmp/p%d.png`, expect 2 files and `batch finished: 1 sheets`; then 3 sheets, then Gray/Lineart/`ADF Front`, then `systemctl start kodak-native`.
B2. Start-button sensor with the owner at the panel (`scanimage -A` after a press shows `--scan … [yes]`), cover/paper sensors.
B3. Spool test: `memory-pages 0` and a stack; cancel inside a page (Q-031: does OperationStop stop the feeder?).
B3b. **Deskew is in (F-073), offline-verified; needs one stack through scanimage on the device** (expect `skew x.xx deg` in the debug log and pages of about 2442 × 3458).
B3c. **LCD labels are in (F-074), upload accepted; needs the owner's eyes:** with kodak-native stopped, `scanimage -A` using a config with `label` lines, then look at the display and press ▲/▼.
B3d. **Power-up replay in the C backend works on the device (F-081, 9.9 s), labels readable.** A scan after it works (F-082). Original test notes: Test: `systemctl stop kodak-native`, owner power-cycles, then `scanimage -A` with a config containing `powerup /etc/kodak-scan/firmware/powerup.seq` and `SANE_DEBUG_KODAK_I2X00=2` (expect `power-up done in ~10 s, firmware id 3`), scan a sheet, `systemctl start kodak-native`.
B4. Then: `make install`, and a SANE-client station to replace kodak-native.

## Next actions (native driver)
N1. (multi-sheet done, F-057) Other modes: capture gray/bw and 200/600 dpi, simplex with `scanimage` under box64 and diff the register writes + ScannerConfiguration against color 300.
N2. (image processing done, F-059) Next: verify the rear side with a double-sided sheet (Q-030); make processing faster (rotation); then one command/daemon that does native scan → process → spool, triggered by the Start button.
N3. Native panel daemon (quick win for the owner's wish): events → Start button + function number → trigger the existing station profile; replaces the 2 s box64 poll. Needs a design decision with the owner (changes ADR-010; the vendor driver claims the interface while it is open).
N4. LCD function labels (Q-025): owner's go-ahead needed (persistent write, format only known from vendor code).
N5. (native power-up passed its first power cycle, F-075.) Left: owner scans one sheet after a native power-up; watch the next power cycles for `native power-up failed`. If it misbehaves: move `/etc/kodak-scan/firmware/powerup.json` away and power-cycle again.
N6. C SANE backend: **started**, see the section above (`libsane-dev`, `libusb-1.0-0-dev`, `sane-utils` installed on the Pi 2026-10-08).

## Next actions (station, from before)
0. Owner: check the PoE HAT OLED (IP on top, `Ready` below; `rotate: 180` in the `oled:` config if upside down) and feed a stack to see `Scanning page N` (ADR-011, F-037).
0b. Watch whether the F-038 I/O error returns after idle time and whether the automatic saned restart (ADR-012, F-039) cures it: `journalctl -u kodak-scand | grep 'saned restart'` (Q-023).
1. Owner: confirm the 3 "Scan 2026-09-28 22:3x" documents in Paperless; rotate the API token (it was pasted in the chat) via `sudoedit /etc/kodak-scan/paperless-token` + `systemctl restart kodak-scand`.
2. Tags per profile: the token's user needs view permission on tags (currently 403 on /api/tags, /api/tasks, /api/documents), or use tag ids.
3. Outage test (Phase 5 exit criterion): stop Paperless → scan → start it → automatic upload; the time from Start to document should be < 60 s.
4. Buttons/LCD (Q-022): not exposed by the vendor SANE backend. Look for a TWAIN/kds.ds route, or do it in the native backend (interrupt EPs 0x81/0x88); then panel function number → profile.
5. Upstream the box64 patch (ptitSeb/box64) so we don't need a local build.
6. Write the option list → `docs/protocol/sane-options-vendor.md` (from `05-scanimage-A.txt`).
7. Phase 2 prep: `tools/openusb-logger/` (ADR-005); classify the 5.7 MB bulk-OUT block on the first open (Q-021).
8. Pending from Phase 0: firmware version; Windows driver inventory (low priority).

## Blockers
- None. Paper handling is physical: Claude runs the commands on the Pi, the owner loads sheets.
