# Transport layer

Status: **endpoint layout confirmed from real descriptors (2026-09-28). Framing not yet known.**
Sources: `docs/hardware/hwinfo-20260928/lsusb-v.txt` (owner's unit), `re/linux-driver/inventory.md` (vendor pipe map).

## Device identity (owner's unit)
| Field | Value |
|---|---|
| VID:PID | `040a:601d` |
| Strings | Manufacturer `"KODAK     "` (padded to 10 chars), Product `"i2600 SCANNER"`, Serial `"0000000000000000"` (the serial number is **not** exposed over USB) |
| bcdUSB / speed | 2.00, High Speed (480 Mbit/s) |
| bcdDevice | 2.01 |
| Class | 0xff/0xff/0xff (vendor specific) at device and interface level. No standard class driver binds it (`Driver=[none]`) |
| Power | Self powered, MaxPower 2 mA |
| Configuration | 1 configuration, 1 interface (#0), alternate setting 0, 5 endpoints |

## Endpoints (confirmed) ↔ vendor pipe map

| EP address | Type | Max packet | Interval | Vendor pipe (F-014) | Role |
|---|---|---|---|---|---|
| `0x02` OUT | Bulk | 512 | – | `OS_USBPIPE_BULKOUT` | Host → device commands/data |
| `0x82` IN | Bulk | 512 | – | `OS_USBPIPE_IMAGEFRONT` | Front image data (probably also command replies; to confirm) |
| `0x86` IN | Bulk | 512 | – | `OS_USBPIPE_IMAGEREAR` | Rear image data (duplex) |
| `0x81` IN | Interrupt | **8** | bInterval 10 → 64 ms | `OS_USBPIPE_BULKINTERRUPT` (flag 20) | Events/status, 8-byte messages |
| `0x88` IN | Interrupt | **8** | bInterval 10 → 64 ms | `OS_USBPIPE_BULKINTERRUPT` (flag 24) | Events/status, 8-byte messages |

- Every endpoint in the vendor configuration exists with the expected direction and type. The earlier hypothesis is **confirmed**.
- Duplex images come on **two separate bulk-IN endpoints**, so there is no need to de-interleave front and back on one stream.
- Button/panel/paper events are very likely the **8-byte interrupt messages** on EP 0x81/0x88. To be confirmed in Phase 2 by pressing buttons during capture.
- The control endpoint (EP0) may carry vendor requests too. The driver resolves `openusb_ctrl_xfer`; Phase 2 will show whether it uses it.

## Framing
Unknown. SCSI CDB framing is unlikely (no SCSI strings in the driver, F-016). To be decided from Phase 2 captures.
