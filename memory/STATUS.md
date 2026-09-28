# Status

_Last updated: 2026-09-28_

**Current phase:** Phase 0 (inventory & baseline). Not started on hardware yet.

## Done
- Desk research on existing support (see FINDINGS F-001 … F-008).
- Project plan (`PLAN.md`), repo skeleton, and memory system created.

## Next actions
1. `lsusb -v -d 040a:601d` → `docs/hardware/usb-descriptors.txt`.
2. Download the Kodak Linux driver v4.14 (x86_64) + Windows v5.01 → `vendor/`; write inventories in `re/*/inventory.md`.
3. Get a reference scan on x86 Linux and dump the vendor SANE options.
4. Time-boxed Path A test: vendor driver under box64 on the Pi.

## Blockers
- Need the physical scanner plus an x86 Linux machine (or a VM with USB passthrough).
