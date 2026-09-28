# Open questions

| ID | Question | Resolve in | Status |
|---|---|---|---|
| Q-001 | USB endpoints: is there an interrupt-IN endpoint for button/panel events? | Phase 0 (`lsusb -v`) | partly answered: vendor config says interrupt pipes on EP1 and EP8 (F-014); confirm with `lsusb -v` |
| Q-002 | Which Windows kernel driver binds the device (`usbscan.sys`?) | Phase 0 (INF) | open |
| Q-003 | Does the vendor Linux driver use libusb or raw `usbdevfs` ioctls? | Phase 0 (`nm -D`/`ldd`) | **closed**: neither; it uses libopenusb via dlopen (F-012) |
| Q-004 | Is the transport SCSI-CDB-over-USB-bulk, like `sane-kodak`/`canon_dr`/`fujitsu`? | Phase 2 | open, now **unlikely** (F-016) |
| Q-005 | Can the host read the panel function number (1–9)? Can it *write* the LCD? | Phase 2/3 | partly answered: API has GetOcpButton/SetOcpButtons (F-015); i2600 behaviour unknown |
| Q-006 | Does the vendor x86_64 SANE stack run under box64 on aarch64? | Phase 1 | open |
| Q-007 | Image transfer format: raw, JPEG, or proprietary; how is duplex interleaved? | Phase 2 | open (separate front/rear endpoints suggest no interleave, F-014) |
| Q-008 | Firmware version of our unit; do protocol differences exist across firmware versions? | Phase 0 | open |
| Q-009 | USB IDs of the successors i2420/i2620/i2820, and do they share the protocol? | later | **closed**: `29cc:100a/b/c`, same driver and shared config (F-013) |
| Q-010 | Does the i2600 have an accessory port, or do accessories only connect over their own USB? | Phase 0 | open (flatbeds have their own USB IDs, F-013) |
| Q-011 | What are `lexexe`/`driverlexexe`/`osjit.so` ("lexicon")? Is any logic executed as a script/JIT? | Phase 3 | open |
| Q-012 | Wire meaning of the `EOSUSBIOCTL` values used by `COsUsbImpl::Ioctl`/`BulkIOControl` | Phase 3 | open |
| Q-013 | Is the "report inquiry" XML sent by the device, or built host-side? | Phase 2 | open |
| Q-014 | Is the Mono `twaingui.exe` needed for headless SANE scanning? | Phase 1 | open |
