# Status

_Last updated: 2026-09-28_

**Current phase:** Phase 0 (inventory & baseline). **Nearly complete.**

## Done
- Desk research on existing support (FINDINGS F-001 … F-008).
- Project plan, repo skeleton, memory system.
- Kodak Linux driver v4.14 downloaded (git-ignored `vendor/`) and inventoried: `re/linux-driver/inventory.md` (F-010 … F-016).
- **USB descriptors collected from the owner's unit** on the Pi (`docs/hardware/hwinfo-20260928/`). The endpoint layout matches the vendor pipe map exactly (F-017, F-018). `docs/protocol/transport.md` updated.
- The scan station is a Raspberry Pi 4 class board, aarch64, kernel 6.18 (F-019).

## Next actions
1. **Owner:** firmware version (Kodak shows it in the driver or on the panel's diagnostic screen, if available) plus photos of the panel and back ports → `docs/hardware/`.
2. Windows driver v5.1 inventory (needs 7z/innoextract; check the `.inf` for `usbscan.sys`, Q-002). Low priority.
3. **Start Phase 1 on the Pi:** box64 + the vendor x86_64 driver, installed manually (ADR-006). If it works, the Pi also becomes the Phase 2 capture host via usbmon (Q-017), so no x86 machine is needed.
4. Otherwise: an x86_64 reference machine or VM for the reference scan and SANE option dump.
5. Write `tools/openusb-logger/` (ADR-005).

## Blockers
- None hard. Item 1 needs the owner; item 3 needs shell access on the Pi (the owner runs the prepared steps).
