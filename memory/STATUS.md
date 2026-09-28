# Status

_Last updated: 2026-09-28_

**Current phase:** Phase 5 (scan station). `pi/scan-station/` installed on the Pi: kodak-saned + kodak-scand run and poll for paper (ADR-010). **End-to-end works: paper → PDF → Paperless upload accepted (F-036).** Open: owner confirms the documents in the Paperless UI; token user lacks view permissions (tags/tasks).
Phase 0 is done except for the firmware version and photos.

## Done
- Phase 0: research, plan, memory, driver inventory (F-010…F-016), real USB descriptors (F-017…F-019).
- Phase 1: `pi/phase1/setup-x86-chroot.sh` + README. Dry-run verified on x86_64 (F-020); safe deb merge (F-021, ADR-007).
- Phase 1 on the Pi: the fake cpuinfo (ADR-008) fixes the open hang. Detection, open (19–34 s), option dump, and bulk USB I/O all work under qemu (F-027, F-028). Empty-feeder scan fails cleanly with "out of documents".

## Next actions
0. Owner: check the 3 uploaded "Scan 2026-09-28 22:3x" documents in Paperless. If tags per profile are wanted, give the token's user view permission on tags (or use tag ids). Test the outage case (stop Paperless → scan → start → auto-upload).
1. Owner: visually check the 4 TIFFs in `pi/phase1/phase1-results-20260928-210749/` (orientation, sharpness, cropping).
2. Use box64 as the emulator for the scan daemon (Phase 5: saned/scan script → Paperless consume). Consider sending the box64 patch upstream (ptitSeb/box64).
3. Write the option list → `docs/protocol/sane-options-vendor.md` (from `05-scanimage-A.txt`).
4. Buttons/LCD: the vendor SANE backend exposes none (Q-022). Look for another route (TWAIN/kds.ds events), or plan them for the native backend (the interrupt EPs 0x81/0x88).
5. If qemu is too slow: box64 variant.
6. Pending from Phase 0: firmware version; Windows driver inventory (low priority).
7. `tools/openusb-logger/` (ADR-005) for Phase 2; the bulk-OUT block on open is the first thing to classify (Q-021).

## Blockers
- Paper handling is physical: Claude runs the commands on the Pi, the owner loads sheets.
