# Status

_Last updated: 2026-10-01_

**Current phase:** Phase 5 (scan station). `pi/scan-station/` installed on the Pi: kodak-saned + kodak-scand run and poll for paper (ADR-010). **End-to-end works: paper → PDF → Paperless upload accepted (F-036).** Open: owner confirms the documents in the Paperless UI; token user lacks view permissions (tags/tasks).
Phase 0 is done except for the firmware version and photos.

## Done
- Phase 0: research, plan, memory, driver inventory (F-010…F-016), real USB descriptors (F-017…F-019).
- Phase 1 (qemu): `pi/phase1/setup-x86-chroot.sh` + README; dry run (F-020), safe deb merge (F-021, ADR-007); fake cpuinfo fixes the open hang (ADR-008, F-027); first USB traffic (F-028, F-030); first scans, ≈30 s per duplex sheet (F-029, F-031).
- Phase 1b (box64): `pi/phase1/setup-box64.sh` builds box64 v0.4.4 with our patch `box64-dlopen-null-linkmap.patch` (F-032, F-033, ADR-009). ≈10× faster than qemu: 3 duplex sheets in 27 s gray (F-034).
- Phase 5 (first version): `pi/scan-station/` = kodak-saned (saned under box64, localhost) + kodak-scand (auto-scan on paper → PDF → Paperless REST, spool + retry), ADR-010, F-035. Installed and enabled on the Pi; end to end verified with 3 color stacks uploaded to Paperless at `http://192.168.10.242:8000` (F-036).
- OLED status display on the PoE HAT (B): `kodak-oled.service` + status file from kodak-scand (ADR-011, F-037). Idle screensaver (starfield + bouncing IP). Installed 2026-10-01; not yet confirmed visually.

## Next actions
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
