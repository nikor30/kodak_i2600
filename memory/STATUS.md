# Status

_Last updated: 2026-09-28_

**Current phase:** Phase 1 (vendor driver on the Pi via emulation). **Phase 1 goal reached: the vendor driver scans duplex batches on the Pi under qemu (F-031), ≈2 sheets/min at gray 300 dpi.**
Phase 0 is done except for the firmware version and photos.

## Done
- Phase 0: research, plan, memory, driver inventory (F-010…F-016), real USB descriptors (F-017…F-019).
- Phase 1: `pi/phase1/setup-x86-chroot.sh` + README. Dry-run verified on x86_64 (F-020); safe deb merge (F-021, ADR-007).
- Phase 1 on the Pi: the fake cpuinfo (ADR-008) fixes the open hang. Detection, open (19–34 s), option dump, and bulk USB I/O all work under qemu (F-027, F-028). Empty-feeder scan fails cleanly with "out of documents".

## Next actions
1. Owner: visually check the 4 TIFFs in `pi/phase1/phase1-results-20260928-210749/` (orientation, sharpness, cropping).
2. Decide the interim path (ADR): qemu chroot + x86 `saned` on localhost → Phase 5 scan daemon + Paperless upload, or try box64 for speed first.
3. Write the option list → `docs/protocol/sane-options-vendor.md` (from `05-scanimage-A.txt`).
4. Buttons/LCD: the vendor SANE backend exposes none (Q-022). Look for another route (TWAIN/kds.ds events), or plan them for the native backend (the interrupt EPs 0x81/0x88).
5. If qemu is too slow: box64 variant.
6. Pending from Phase 0: firmware version; Windows driver inventory (low priority).
7. `tools/openusb-logger/` (ADR-005) for Phase 2; the bulk-OUT block on open is the first thing to classify (Q-021).

## Blockers
- Paper handling is physical: Claude runs the commands on the Pi, the owner loads sheets.
