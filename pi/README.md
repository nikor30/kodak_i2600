# Raspberry Pi scan station

- [`phase1/`](phase1/README.md): Kodak's x86_64 driver on the Pi: the chroot, box64 (with our patch), qemu fallback, diagnostics.
- [`scan-station/`](scan-station/README.md): `kodak-saned` + `kodak-scand`: auto-scan on paper → PDF → Paperless-ngx, with a spool and retry.

Button/panel handling is still open (the vendor SANE backend doesn't expose it, Q-022).
