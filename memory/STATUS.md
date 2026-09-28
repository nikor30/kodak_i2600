# Status

_Last updated: 2026-09-28_

**Current phase:** Phase 1 (vendor driver on the Pi via emulation). **First real scan works on the Pi (F-029). Next: verify image content with a printed duplex sheet, then measure multi-page speed.**
Phase 0 is done except for the firmware version and photos.

## Done
- Phase 0: research, plan, memory, driver inventory (F-010…F-016), real USB descriptors (F-017…F-019).
- Phase 1: `pi/phase1/setup-x86-chroot.sh` + README. Dry-run verified on x86_64 (F-020); safe deb merge (F-021, ADR-007).
- Phase 1 on the Pi: the fake cpuinfo (ADR-008) fixes the open hang. Detection, open (19–34 s), option dump, and bulk USB I/O all work under qemu (F-027, F-028). Empty-feeder scan fails cleanly with "out of documents".

## Next actions
1. Put a **printed** sheet in the feeder and wait for the green LED, then `--diagnose --scan` (now saves both sides; the first scan only kept the blank-looking side). Then a 5–10 page batch for pages/min (Q-019).
2. Write the option list → `docs/protocol/sane-options-vendor.md` (from `05-scanimage-A.txt`).
3. Buttons/LCD: the vendor SANE backend exposes none (Q-022). Look for another route (TWAIN/kds.ds events), or plan them for the native backend (the interrupt EPs 0x81/0x88).
4. If qemu is too slow: box64 variant.
5. Pending from Phase 0: firmware version; Windows driver inventory (low priority).
6. `tools/openusb-logger/` (ADR-005) for Phase 2; the bulk-OUT block on open is the first thing to classify (Q-021).

## Blockers
- Paper handling is physical: Claude runs the commands on the Pi, the owner loads sheets.
