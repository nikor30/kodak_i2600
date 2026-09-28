# Kodak Linux driver v4.14: inventory

- **Package:** `LinuxSoftware_i2000_v4.14.x86_64.deb.tar.gz` (27,680,946 bytes; gzip timestamp 2016-12-07)
  from `https://resources.kodakalaris.com/docimaging/drivers/`. There is also an `.i586` variant (28,730,190 bytes).
- **Analysed:** 2026-09-28, x86_64 variant. Static inspection only (`file`, `readelf`, `nm -C`, `strings`).
- Notes only. No vendor code is reproduced here (see CLAUDE.md hard rules).

## Tarball contents

| File | Package / version | Notes |
|---|---|---|
| `kodak_i2000-04.14.00.00-1.amd64.deb` | `kodak-i2000` 04.14.00.00-1 | The driver itself |
| `libopenusb_1.1.0-6_amd64.deb` | libopenusb 1.1.0 | Old variant, depends on HAL |
| `libopenusb_1.1.13-0_amd64.deb` | libopenusb 1.1.13 | Depends on `libudev0` |
| `libudev0_175-0ubuntu19_amd64.deb` | libudev0 175 | For old openusb |
| `twaindsm.deb` | twaindsm 2.3.1 | TWAIN Data Source Manager |
| `setup` | shell script | Installer |

## Files installed by `kodak-i2000` (48 files)

| Path | Type | Role (from symbols/strings) |
|---|---|---|
| `/usr/lib/sane/libsane-kds_i2000.so.1.0.24` | ELF64 .so, **debug_info, not stripped** | SANE backend "sanetwain": a SANE→TWAIN bridge. It calls `DSM_Entry` (twaindsm) → `kds.ds`. Log at `/var/kodak/kds_i2000/sanetwain/sanetwain.log`; env vars `SANETWAIN_DEBUG/LOG/LOGCONSOLE/LOGMODE/PRESCAN/VERSION` |
| `/usr/local/lib/twain/kodak/kds_i2000/kds.ds` | ELF64 .so, debug, not stripped | TWAIN data source |
| `/opt/kodak/kds_i2000/lib/driver.so` | ELF64 .so, debug, not stripped (7,457 syms) | Scan logic, capability "database" (`CDatabase`, `CDbConfig`, report-inquiry parsing), profiles |
| `/opt/kodak/kds_i2000/lib/device.so` | ELF64 .so, debug, not stripped (2,353 syms) | **Device/transport layer**: `CDevIO`, `CDevProcessCommands`, `CDevDeviceEvent`, `COsUsb`/`COsUsbImpl`, `COpenUsb`, image managers |
| `/opt/kodak/kds_i2000/lib/devicemanager.so` | ELF64 .so (links pango/cairo/glib) | Device manager / UI helper |
| `/opt/kodak/kds_i2000/lib/hippo.so` | ELF64 .so, 33 MB (67k syms) | Image processing (Intel UIC/IPP, libtiff, JPEG, OpenMP) |
| `/opt/kodak/kds_i2000/lib/osjit.so`, `lexexe`, `driverlexexe` | ELF64 | "Lexicon" runtime (`GetLexiconId`/`Status`). Purpose to be determined (Phase 3) |
| `/opt/kodak/kds_i2000/lib/deviceprobe`, `/usr/local/bin/deviceprobe_i2000` | ELF64 | Device probing tool (useful in Phase 0/1 to test detection) |
| `/opt/kodak/kds_i2000/pnphelper` | ELF64 | Called by udev on add/remove |
| `/opt/kodak/kds_i2000/lib/twaingui.exe` | **PE32 .NET (Mono)** | TWAIN GUI; not needed for headless SANE use (to be confirmed) |
| `lib/libbotan-1.10.so` | crypto lib | Purpose unknown (licensing? `CDbLicense` exists) |
| `lib/libstdc++.so.6`, `libgcc_s.so.1`, `libimf/libirc/libintlc/libiomp5`, `libuic_*`, `libtiff.so.5`, `libfreetype.so.6` | bundled runtimes | Intel compiler runtime and UIC |
| `lib/profiles/*.profile`, `*.png`, `sounds/ding.wav` | data | Scan profiles |
| `/etc/sane.d/kds_i2000.conf` | text | Device list (6 models, below) |
| `/opt/kodak/kds_i2000/eklog.sh` | shell | Log collection |

**Headline:** all Kodak binaries ship with **DWARF debug info and full C++ symbol tables**. Static analysis in Ghidra will get real class and method names.

## Software stack (runtime call chain)

```
scanimage / python-sane
  └─ libsane-kds_i2000.so  (SANE ↔ TWAIN bridge)
       └─ libtwaindsm (DSM_Entry)
            └─ kds.ds (TWAIN DS)
                 └─ driver.so ── hippo.so (image processing)
                      └─ device.so  (CDevIO / COsUsbImpl)
                           └─ dlopen("/usr/local/lib/libopenusb.so" | "/usr/local/lib64/libopenusb.so")
                                └─ USB
```

- USB access goes through **libopenusb**, which is loaded with `dlopen` from a **hard-coded path** (not through libusb, and not linked as a dependency).
  → For Phase 2 we can install a **logging shim** at that path that forwards to the real libopenusb (ADR-005).
- openusb functions resolved by `device.so`/`driver.so` include `openusb_bulk_xfer`, `openusb_ctrl_xfer`, `openusb_intr_xfer`, `openusb_xfer_aio`, `openusb_xfer_wait`, `openusb_claim_interface`, `openusb_get_raw_desc`, and more.

## Supported models (from `kds_i2000.conf`, udev rules, and the embedded device list)

| Model | VID:PID | Duplex |
|---|---|---|
| i2400 | `040a:601c` | yes |
| **i2600** | **`040a:601d`** | yes |
| i2800 | `040a:601e` | yes |
| i2420 | `29cc:100a` | yes |
| i2620 | `29cc:100b` | yes |
| i2820 | `29cc:100c` | yes |
| i1000 A4 flatbed | `040a:6011` | accessory |
| i1000 A3 flatbed | `040a:6012` | accessory |
| i2000 Legal flatbed | `040a:602f` | accessory |
| i2000 A3 flatbed | `040a:602e` | accessory |

All six scanners share one `<common>` device configuration. This strongly suggests the same protocol across the i2x00 and i2x20 generations.

## USB pipe map (embedded device configuration in `device.so`, "osusbopen" section)

Configuration 1, interface 0, alternate setting 0:

| Driver pipe | Endpoint number | Flag |
|---|---|---|
| `OS_USBPIPE_BULKINTERRUPT` | 1 | 20 |
| `OS_USBPIPE_BULKINTERRUPT` | 8 | 24 |
| `OS_USBPIPE_BULKOUT` | 2 | – |
| `OS_USBPIPE_IMAGEFRONT` | 2 | – |
| `OS_USBPIPE_IMAGEREAR` | 6 | – |

Other device config values: `devretrytimeout = 302000`, `devsingleimageendpoint = false`, `devinterruptendpoint = true`.

Hypothesis (to verify with `lsusb -v`): EP 0x02 OUT = commands; EP 0x82 IN = front image; EP 0x86 IN = rear image; EP 0x81/0x88 IN = interrupt/status (buttons, events).

Callback names in `COsUsbImpl` show these logical channels: BulkIn, BulkOut, BulkInterrupt, ImageFront, ImageRear, LockIn, LockOut, MgrIn, MgrOut.

## Protocol clues

- **No SCSI/CDB strings** in `device.so`/`driver.so` apart from class names like `CDb*`. The SCSI-over-bulk hypothesis (Q-004) is now **unlikely**.
- The device capability model is an XML **"report inquiry"** (`CDbConfig::ProcessReportInquiryXml`, sort options for barcode/patch/multifeed/size). Whether the XML goes over the wire or is built on the host is the key question for Phase 2.
- Internal task vocabulary (`CDevProcessCommands`) includes: InterfaceOpen/Close, SessionBegin/End, ResourceBegin/End/Lock/Unlock, ScanBegin/End/RequestStop, ImageEnd, GetConfig/SetConfig, **GetOcpButton**, **SetOcpButtons** (fields: button number, text, graphic location, program, paper source, owner), GetConsumableMeters/ResetConsumableMeter/SetConsumableMeter, GetLog/ClearLog, GetVersionUser, Calibrate*, Diagnostic*, **Download*** (probably firmware), Ecdocustom*.
  - OCP = Operator Control Panel. Host-side read of the panel button **and** a host→panel configuration command both exist in the API (Q-005: partly answered).
  - **Safety:** Download*, Calibrate*, Diagnostic*, SetConsumableMeter/ResetConsumableMeter and ClearLog are on the **never-send** list until fully understood.
- `SessionBegin` has a `MonitorButtonEvents` flag, so button events are subscribed per session.
- There is simulation support (`Simulate/SimModel/SimFlatbed`, `COsUsbImpl::DeviceIoSimulation`). This may help us run the stack without hardware.

## Install side-effects to beware of (for Pi/box64)
- postinst writes `/etc/udev/rules.d/55-kodakdi.rules` (MODE 666, runs `pnphelper`), **appends to `50-udev-default.rules`**, and creates `/var/kodak` with `chmod -R 777`.
  On the Pi we install manually in a controlled way instead of running `setup`.

## Next analysis steps
1. Ghidra: `COsUsbImpl::Ioctl` / `BulkIOControl` → the byte layout that goes out on EP2.
2. The `EOSUSBIOCTL` enum values (demangled signatures use `EOSUSBIOCTL` and `EOSUSBPIPE`) → the list of wire-level operations.
3. Find out what `lexexe`/`osjit` ("lexicon") are.
