# Open questions

| ID | Question | Resolve in | Status |
|---|---|---|---|
| Q-001 | USB endpoints: is there an interrupt-IN endpoint for button/panel events? | Phase 0 (`lsusb -v`) | open |
| Q-002 | Which Windows kernel driver binds the device (`usbscan.sys`?) | Phase 0 (INF) | open |
| Q-003 | Does the vendor Linux driver use libusb or raw `usbdevfs` ioctls? | Phase 0 (`nm -D`/`ldd`) | open |
| Q-004 | Is the transport SCSI-CDB-over-USB-bulk, like `sane-kodak`/`canon_dr`/`fujitsu`? | Phase 2 | open |
| Q-005 | Can the host read the panel function number (1–9)? Can it *write* the LCD? | Phase 2/3 | open |
| Q-006 | Does the vendor x86_64 SANE stack run under box64 on aarch64? | Phase 1 | open |
| Q-007 | Image transfer format: raw, JPEG, or proprietary; how is duplex interleaved? | Phase 2 | open |
| Q-008 | Firmware version of our unit; do protocol differences exist across firmware versions? | Phase 0 | open |
| Q-009 | USB IDs of the successors i2420/i2620/i2820, and do they share the protocol? | later | open |
| Q-010 | Does the i2600 have an accessory port, or do accessories only connect over their own USB? | Phase 0 | open |
