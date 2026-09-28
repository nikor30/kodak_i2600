# Findings (verified facts)

Format: `F-### | fact | source | confidence (high/med/low) | date`

| ID | Fact | Source | Conf. | Date |
|---|---|---|---|---|
| F-001 | i2600 USB ID is `040a:601d`; i2400 `040a:601c`; i2800 `040a:601e` | sane-backends `doc/descriptions-external/kodak-twain.desc` | high | 2026-09-28 |
| F-002 | i1000 flatbed accessories are separate USB devices: A4 `040a:6011`, A3 `040a:6012` (12 V over the USB cable) | same file | high | 2026-09-28 |
| F-003 | No open-source SANE backend supports the i2x00 family; upstream lists it only under the proprietary external `kodak-twain` backend (status "untested") | same file | high | 2026-09-28 |
| F-004 | Upstream `sane-kodak` backend covers older SCSI/IEEE-1394 i-series (i1860 "basic", others untested), not the USB i2000 family | sane-backends `doc/descriptions/kodak.desc` | high | 2026-09-28 |
| F-005 | `sane-kodakaio` is for ESP/Hero inkjet AiOs and is irrelevant | sane-kodakaio(5) | high | 2026-09-28 |
| F-006 | Vendor Linux driver v4.14 is offered for x86 (i586) and x86_64 as `.deb.tar.gz`; there is no ARM build | kodakalaris.com i2600 product/driver page | high | 2026-09-28 |
| F-007 | Vendor Windows driver is v5.01 (installer `.exe` and ISO) | same page | high | 2026-09-28 |
| F-008 | The vendor Linux driver was reported working on x86 in 2013 (32-bit only then, hard to install) | sane-devel 2013-04 message 031233 | med | 2026-09-28 |
