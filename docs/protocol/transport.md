# Transport layer (draft)

Status: **draft from vendor configuration data. Not yet verified on the wire.**
Sources: `re/linux-driver/inventory.md` (pipe map), user `lsusb` (2026-09-28).

## Device identity
- VID:PID `040a:601d`; `lsusb` string "Kodak Co. i2600 SCANNER".
- Configuration 1, interface 0, alternate setting 0.

## Pipes

| Logical pipe | EP number | Direction (hypothesis) | Use (hypothesis) |
|---|---|---|---|
| Command out | 2 | OUT (0x02) | Commands/requests |
| Image front | 2 | IN (0x82) | Front-side image data (and command replies?) |
| Image rear | 6 | IN (0x86) | Rear-side image data (duplex) |
| Interrupt/status #1 | 1 | IN (0x81) | Events: buttons, paper, errors |
| Interrupt/status #2 | 8 | IN (0x88) | Events (second channel; purpose unknown) |

To confirm: endpoint directions, types (bulk vs interrupt), and max packet sizes from `lsusb -v` (run `tools/phase0/collect-hw-info.sh`).

## Framing
Unknown. SCSI CDB framing is now unlikely (no SCSI strings in the driver). To be decided from Phase 2 captures.
