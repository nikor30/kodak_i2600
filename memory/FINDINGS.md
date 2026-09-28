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
| F-009 | Owner's unit enumerates as `040a:601d` "Kodak Co. i2600 SCANNER" (Bus 001 Device 003) | owner's `lsusb`, `docs/hardware/lsusb.txt` | high | 2026-09-28 |
| F-010 | Linux driver v4.14 download URLs: `https://resources.kodakalaris.com/docimaging/drivers/LinuxSoftware_i2000_v4.14.{x86_64,i586}.deb.tar.gz`; Windows v5.1: `.../i2000March2018/InstallSoftware_i2000_v5.1.exe` and `CD_WINDOWS_i2000_v5.1.iso` | kodakalaris.com i2600 page; Linux files downloaded successfully | high | 2026-09-28 |
| F-011 | All Kodak Linux binaries are **not stripped and carry DWARF debug info** (full C++ class/method names) | `file`, `nm -C` on the v4.14 x86_64 package | high | 2026-09-28 |
| F-012 | Stack: `libsane-kds_i2000` (SANE→TWAIN bridge) → twaindsm → `kds.ds` → `driver.so` → `device.so` → **libopenusb**, dlopen'ed from the hard-coded `/usr/local/lib{,64}/libopenusb.so` | strings/symbols, `re/linux-driver/inventory.md` | high | 2026-09-28 |
| F-013 | The same driver covers i2400/i2600/i2800 (`040a:601c/d/e`) and i2420/i2620/i2820 (`29cc:100a/b/c`) with one shared device config; flatbeds `040a:6011/6012/602e/602f` | `kds_i2000.conf`, postinst udev rules, device.so embedded XML | high | 2026-09-28 |
| F-014 | Vendor pipe map (config 1, iface 0, alt 0): BULKINTERRUPT EP1 (flag 20) and EP8 (flag 24), BULKOUT EP2, IMAGEFRONT EP2, IMAGEREAR EP6; `devinterruptendpoint=true`, `devsingleimageendpoint=false` | device.so embedded "osusbopen" XML | high (as config), directions unverified | 2026-09-28 |
| F-015 | The driver API has OCP (operator control panel) tasks `GetOcpButton` and `SetOcpButtons` (button number, text, graphic location, program, paper source, owner); SessionBegin has a `MonitorButtonEvents` flag | device.so symbols | high (API exists), i2600 device support unknown | 2026-09-28 |
| F-016 | No SCSI/CDB strings in the transport libs; the capability model is an XML "report inquiry" | strings/symbols of device.so/driver.so | med | 2026-09-28 |
