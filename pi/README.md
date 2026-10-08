# Raspberry Pi integration

- `phase1/`: vendor x86_64 driver on the Pi (chroot + qemu-user / box64).
- `scan-station/`: the scan station. Two alternatives that share config, spool, uploader and OLED:
  `kodak-native` (native driver, Start button, LCD labels) and `kodak-scand` + `kodak-saned`
  (vendor driver under box64, scan on paper). See `scan-station/README.md`.
