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
