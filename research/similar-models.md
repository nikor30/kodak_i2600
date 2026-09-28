# Similar models & reference backends

| Device / backend | Relation to i2600 | Use for us |
|---|---|---|
| Kodak i2400 (`040a:601c`), i2800 (`040a:601e`) | Same family and driver package | Our backend should cover them too |
| Kodak i2420 / i2620 / i2820 | Successor generation | Check USB IDs and protocol later (Q-009) |
| i1000 A4/A3 flatbed (`040a:6011`/`6012`) | Accessory for the i2x00 family | Optional later |
| SANE `kodak` backend (i1860 etc., SCSI/1394) | Older Kodak document scanners | Compare the opcode set (`kodak-cmd.h`) |
| SANE `canon_dr`, `fujitsu` backends | SCSI-over-USB document scanners | Structural template for the backend |
| Kodak `kodak-twain` external backend | The vendor driver itself (x86 only) | Reference behaviour & option list |
| SANE `kodakaio` | Kodak inkjet AiOs | Not relevant |
