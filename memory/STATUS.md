# Status

_Last updated: 2026-09-28_

**Current phase:** Phase 0 (inventory & baseline). **In progress.**

## Done
- Desk research on existing support (FINDINGS F-001 … F-008).
- Project plan, repo skeleton, memory system.
- Owner's scanner confirmed as `040a:601d` (F-009).
- Kodak Linux driver v4.14 downloaded (git-ignored `vendor/`) and **inventoried**: `re/linux-driver/inventory.md` (F-010 … F-016).
  Highlights: binaries unstripped with debug info; USB through dlopen'ed libopenusb; pipe map EP1/EP8 interrupt, EP2 out/front image, EP6 rear image; OCP button get/set API exists.
- `tools/phase0/collect-hw-info.sh` (read-only descriptor collection) written.
- Draft transport doc: `docs/protocol/transport.md`.

## Next actions
1. **Owner:** run `sudo tools/phase0/collect-hw-info.sh` on the machine the scanner is attached to; commit the output to `docs/hardware/` (confirms endpoint directions and types).
2. **Owner:** note the firmware version (if the panel/driver shows it) and take photos of the panel and ports → `docs/hardware/`.
3. Windows driver v5.1 inventory (needs 7z/innoextract; check the `.inf` for `usbscan.sys`, Q-002).
4. Set up the x86_64 reference machine/VM, install the driver manually (ADR-006), and get a reference scan plus the SANE option dump.
5. Write `tools/openusb-logger/` (ADR-005) in preparation for Phase 2.

## Blockers
- Items 1, 2 and 4 need the physical scanner (and an x86 machine for 4).
