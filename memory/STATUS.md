# Status

_Last updated: 2026-10-08_

**Current phase:** Phase 2 (protocol capture) + first native code, running on the Pi itself. The owner asked for the native ARM driver because the box64 station crashes often and is costly; the Phase 5 station (vendor driver under box64) stays in service meanwhile.
**Known now:** commands are vendor control requests on EP0 (F-041), 69 request names (F-043), status block incl. paper/cover/function number (F-045, F-051), **button and panel events read natively** (F-051), scan start/stop sequence (F-052), **raw RGB image format** (F-053), LCD bitmap upload (F-046), SetTime (F-050), power-up firmware sequence (F-048).
**First native scan done (F-056):** `tools/kdsprobe/native_scan.py` scans one sheet color 300 dpi duplex by replaying the captured start sequence; raw images only (no crop/deskew), and only on a scanner the vendor driver has initialised since power-up.
**Image processing done (F-059):** `kds_pages.py` turns the raw streams into cropped, deskewed, colour-corrected pages and a PDF, and can queue it in the station's upload spool.
**Not known yet:** the mode-dependent setup registers and config fields (Q-026), page ends / stream length (Q-028), LCD function labels (Q-025), power-up init for a native driver (Q-027).

## Done
- Phase 0: research, plan, memory, driver inventory (F-010…F-016), real USB descriptors (F-017…F-019).
- Phase 1 (qemu): `pi/phase1/setup-x86-chroot.sh` + README; dry run (F-020), safe deb merge (F-021, ADR-007); fake cpuinfo fixes the open hang (ADR-008, F-027); first USB traffic (F-028, F-030); first scans, ≈30 s per duplex sheet (F-029, F-031).
- Phase 1b (box64): `pi/phase1/setup-box64.sh` builds box64 v0.4.4 with our patch `box64-dlopen-null-linkmap.patch` (F-032, F-033, ADR-009). ≈10× faster than qemu: 3 duplex sheets in 27 s gray (F-034).
- Phase 2 (started 2026-10-08): `tools/usbcap/`, `tools/re/elftables.py`, `tools/kdsprobe/`, `docs/protocol/commands.md`, `re/linux-driver/devicemanager.md` (F-041…F-050, ADR-013, ADR-014).
- Phase 5 (first version): `pi/scan-station/` = kodak-saned (saned under box64, localhost) + kodak-scand (auto-scan on paper → PDF → Paperless REST, spool + retry), ADR-010, F-035. Installed and enabled on the Pi; end to end verified with 3 color stacks uploaded to Paperless at `http://192.168.10.242:8000` (F-036).
- OLED status display on the PoE HAT (B): `kodak-oled.service` + status file from kodak-scand (ADR-011, F-037). Idle screensaver (starfield + bouncing IP). Fan thermostat in the same service (F-040; HAT fan switch must be in the programmable position). Installed 2026-10-01; not yet confirmed visually.

## Next actions (native driver)
N1. (multi-sheet done, F-057) Other modes: capture gray/bw and 200/600 dpi, simplex with `scanimage` under box64 and diff the register writes + ScannerConfiguration against color 300.
N2. (image processing done, F-059) Next: verify the rear side with a double-sided sheet (Q-030); make processing faster (rotation); then one command/daemon that does native scan → process → spool, triggered by the Start button.
N3. Native panel daemon (quick win for the owner's wish): events → Start button + function number → trigger the existing station profile; replaces the 2 s box64 poll. Needs a design decision with the owner (changes ADR-010; the vendor driver claims the interface while it is open).
N4. LCD function labels (Q-025): owner's go-ahead needed (persistent write, format only known from vendor code).
N5. Power-up capture (Q-027): start a capture, owner power-cycles the scanner, restart kodak-saned.
N6. C SANE backend in `backend/` from `docs/protocol/` once N2 works (needs `libsane-dev`, `libusb-1.0-0-dev`).

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
