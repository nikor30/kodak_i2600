# Status

_Last updated: 2026-09-28_

**Current phase:** Phase 1 (vendor driver on the Pi via emulation). **Waiting on the owner's test run.**
Phase 0 is done except for the firmware version and photos.

## Done
- Phase 0: research, plan, memory, driver inventory (F-010…F-016), real USB descriptors (F-017…F-019).
- Phase 1: `pi/phase1/setup-x86-chroot.sh` + README. Dry-run verified on x86_64 (F-020); safe deb merge (F-021, ADR-007).

## Next actions
1. **Owner (on the Pi):** `sudo pi/phase1/setup-x86-chroot.sh`, then `--diagnose`, then `--diagnose --scan` with one sheet in the feeder. Send back the `phase1-results-*.tar.gz` files.
2. Evaluate: detection (Q-018), speed (Q-019), and the option list → `docs/protocol/sane-options-vendor.md`.
3. If qemu fails or is too slow: box64 variant.
4. Pending from Phase 0: firmware version, panel/port photos; Windows driver inventory (low priority).
5. `tools/openusb-logger/` (ADR-005): can be built inside the chroot and used on the Pi for Phase 2.

## Blockers
- Step 1 needs the owner to run the script on the Pi.
