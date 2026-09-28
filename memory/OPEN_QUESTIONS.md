# Open questions

| ID | Question | Resolve in | Status |
|---|---|---|---|
| Q-001 | USB endpoints: is there an interrupt-IN endpoint for button/panel events? | Phase 0 (`lsusb -v`) | **closed**: two interrupt-IN endpoints `0x81` and `0x88`, 8-byte packets (F-017). Which carries buttons is still for Phase 2 |
| Q-002 | Which Windows kernel driver binds the device (`usbscan.sys`?) | Phase 0 (INF) | open |
| Q-003 | Does the vendor Linux driver use libusb or raw `usbdevfs` ioctls? | Phase 0 (`nm -D`/`ldd`) | **closed**: neither; it uses libopenusb via dlopen (F-012) |
| Q-004 | Is the transport SCSI-CDB-over-USB-bulk, like `sane-kodak`/`canon_dr`/`fujitsu`? | Phase 2 | open, now **unlikely** (F-016) |
| Q-005 | Can the host read the panel function number (1–9)? Can it *write* the LCD? | Phase 2/3 | partly answered: API has GetOcpButton/SetOcpButtons (F-015); i2600 behaviour unknown |
| Q-006 | Does the vendor x86_64 SANE stack run under box64 on aarch64? | Phase 1 | open |
| Q-007 | Image transfer format: raw, JPEG, or proprietary; how is duplex interleaved? | Phase 2 | open (separate front/rear endpoints suggest no interleave, F-014) |
| Q-008 | Firmware version of our unit; do protocol differences exist across firmware versions? | Phase 0 | open (bcdDevice 2.01 is known, F-018, but may not be the firmware version) |
| Q-009 | USB IDs of the successors i2420/i2620/i2820, and do they share the protocol? | later | **closed**: `29cc:100a/b/c`, same driver and shared config (F-013) |
| Q-010 | Does the i2600 have an accessory port, or do accessories only connect over their own USB? | Phase 0 | **closed**: back has only USB-B, power, switch (F-024); accessories use their own USB IDs (F-013) |
| Q-011 | What are `lexexe`/`driverlexexe`/`osjit.so` ("lexicon")? Is any logic executed as a script/JIT? | Phase 3 | open |
| Q-012 | Wire meaning of the `EOSUSBIOCTL` values used by `COsUsbImpl::Ioctl`/`BulkIOControl` | Phase 3 | open |
| Q-013 | Is the "report inquiry" XML sent by the device, or built host-side? | Phase 2 | open |
| Q-014 | Is the Mono `twaingui.exe` needed for headless SANE scanning? | Phase 1 | open |
| Q-015 | Is EP0 (control) used for vendor requests, or only bulk/interrupt? | Phase 2 | open |
| Q-016 | Which of the interrupt endpoints `0x81`/`0x88` carries button/panel events, and what is the 8-byte format? | Phase 2 | open |
| Q-018 | Does qemu-user pass the usbfs ioctls (`USBDEVFS_SUBMITURB`/`REAPURB`, claim interface) that openusb's linux backend uses? | Phase 1 (`--diagnose --scan`) | answered for control + bulk: ~5.7 MB bulk OUT and bulk IN on both image pipes go through qemu (F-028); a real page scan is still untested |
| Q-019 | Is scanning under qemu fast enough (hippo.so image processing is heavy)? Measure pages/min | Phase 1 | **answered**: box64 is ≈10× faster than qemu. 3 duplex sheets in 27 s including a ~15 s open (F-034). Color not measured yet |
| Q-017 | Can the Pi itself serve as the capture host (box64 vendor driver + usbmon), removing the need for an x86 machine? | Phase 1 | open |
| Q-020 | Why does opening hang under qemu (F-025)? A USB reset plus re-enumeration, an unsupported ioctl, or a netlink/udev wait? What do the LCD and the red Start LED show during the hang? | Phase 1 (`--trace-open`, `--snapshot`) | **closed**: hippo spun after parsing the ARM /proc/cpuinfo (F-026); the fake x86 cpuinfo fixes it (F-027). LCD `0` + red LED meaning still unknown |
| Q-021 | What is the ~5.7 MB bulk-OUT block sent on the first open after power-up (F-028)? It is likely firmware/FPGA load (F-030), so the native backend would need it too. Is it identical every time, and where does the driver keep it? | Phase 2 | open (only first open after power-on, F-030) |
| Q-022 | The vendor SANE backend exposes no button/panel options (F-027). Can the OCP API (F-015) be reached some other way (TWAIN caps, `kds.ds` events), or does panel support need the native backend? | Phase 1/4 | open |
