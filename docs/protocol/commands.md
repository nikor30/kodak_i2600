# Command layer

Status: **request table and status block known; scan sequence and event payloads still to be captured.**
Last updated 2026-10-08. Every statement names its source; "hyp." marks a hypothesis.

Sources
- `C:` captures made on the Pi with `tools/usbcap/` (vendor driver under box64, firmware 1.2.15):
  `idle-poll-10s`, `open-A`, `scan-empty-gray200` (local, not committed: see `captures/README.md`).
- `T:` the truncated text trace of a power-up open, `pi/phase1/phase1-trace-20260928-205802/usbmon-bus1.txt`.
- `N:` name tables of the vendor `devicemanager.so` (`re/linux-driver/devicemanager.md`).
- `P:` our own read-only probe, `tools/kdsprobe/`.

## 1. Framing

There is **no command framing on the bulk pipes**. Every command is a **USB vendor control
request on endpoint 0** (C: an open and an empty-feeder scan attempt contain only control
transfers):

| | bmRequestType | bRequest | wValue / wIndex | data stage |
|---|---|---|---|---|
| host → device ("set") | `0x40` | command code | command arguments | wLength bytes OUT, often 0 |
| device → host ("get") | `0xC0` | command code | command arguments | wLength bytes IN |

The same code is used for both directions of one item (e.g. `0x35` NVRam: `C0 35` reads 128
bytes, `40 35` writes them). The bulk and interrupt endpoints carry only:

- `0x02` OUT: bulk downloads (FPGA configuration after `0x20`; T: 5.7 MB on a power-up open).
- `0x82` / `0x86` IN: front / rear image data.
- `0x81` / `0x88` IN: 8-byte interrupt events (section 4).

## 2. Request codes

Names are the vendor's own (N). "Seen" says where the request shows up in our captures.
**Class** is our safety class for the native code:
`R` = read without side effects, sent by the vendor driver on every open → allowed;
`W` = changes volatile device state, allowed once its arguments are documented here;
`X` = never send (persistent storage, firmware, calibration, diagnostics);
`?` = not classified yet → refused.

| Code | Name | Seen | Class | Notes |
|---|---|---|---|---|
| `00` | GetStatus | C, P: IN 32 | R | status block, section 3. The vendor's idle paper poll is this request alone |
| `02` | GetFwVersions | C, P: IN 56 | R | 4-byte records, section 5 |
| `03` | GetMachineConstants | C: IN 32 | R | |
| `09` | GetEventLog | – | ? | |
| `0a` | TraceLog | – | ? | |
| `10` | OperationStartStop | – | ? | expected in a real scan |
| `11` | SetLamp | C: OUT v=0 len 0 | ? | |
| `12` | SetButtonMode | – | ? | |
| `13` | AddEventToLog | – | X | writes the device log |
| `14` | ClearEventLog | – | X | |
| `15` | BatchPauseResume | – | ? | |
| `16` | SetSequenceNumber | C: OUT v=0001 i=0007 len 0 | ? | vendor name "button sequence number" (N); hyp.: the function number shown on the LCD (v) and its maximum (i) |
| `17` | StartCapture | – | ? | OUT len 0, one u16 argument in wValue, wIndex 0 (N) |
| `18` | SetAutoWhite | T: OUT len 4 | ? | |
| `19` | SetOCPMode | – | ? | |
| `1a` | HomeCamera | – | ? | flatbed |
| `1b` | Ring | – | ? | |
| `1c` | ElevatorStartStop | – | ? | not on the i2600 |
| `1d` | ClearImageBuffer | – | ? | |
| `1e` | BatchBeginEnd | – | ? | |
| `1f` | SetTime | C: OUT v=3077 i=7e46 len 0 | W | **seconds since 2001-01-01 00:00:00 UTC**, u32: wValue = high word, wIndex = low word (C: two captures 156 s apart both give that epoch to the second) |
| `20` | FPGADownload | T: OUT v=1 … bulk … v=0 | X* | power-up only |
| `21` | FirmwareDownload | T: OUT v=0 | X* | power-up only |
| `22` | Disconnect | – | ? | |
| `23` | SubsystemFwUpdate | – | X | |
| `24` | BulkDownload | – | X | |
| `30` | ScannerConfiguration | – | ? | expected in a real scan |
| `31` | EnergyStar | – | ? | |
| `32` | LampTimeout | C: IN 2 = `0e 3c` | R | |
| `33` | Meters | C, P: IN 20 | R | five u32 LE counters; meanings open |
| `34` | EOLConfiguration | C, P: IN 55 | R | VID, PID, vendor and product strings |
| `35` | NVRam | C: IN 128, IN 256 (v=1), **OUT 128** | R / X | the vendor driver rewrites it on every open; we do not write |
| `36` | SerialNumber | C, P: IN 16 ASCII | R | `0000000049374377` on the owner's unit |
| `37` | VRam | C: IN 64 | R | |
| `38` | ElevatorLoadPosition | – | ? | |
| `39` | SetPower | – | ? | |
| `3a` | InterruptEventControl | – | ? | OUT len 0, wValue = number of interrupt endpoints (2 on this family), wIndex = 1 enable / 0 disable (N: argument order of the vendor wrapper + its device database). Not sent on a warm open or an empty-feeder poll; the vendor enables events when a scan runs and during power-up init |
| `3b` | Background | – | ? | |
| `3c` | UDDS Calibration | – | X | ultrasonic multifeed sensor calibration |
| `3e` | SendError | – | ? | |
| `3f` | Transport | – | ? | |
| `40` | Subsystem | – | ? | |
| `41` | Camera | – | ? | |
| `42` | PDMD | – | ? | |
| `43` | DSP | – | ? | |
| `44` | PdmdLED | – | ? | |
| `45` | BatchData | C: IN 3 = `02 00 00` | R | |
| `50` | Printer | – | ? | imprinter, not fitted |
| `5f` | OCPLEDControl | – | ? | |
| `62` | LCDPopulate | C, P: OUT v=0104 i=0680 len 768 | W (type 4 id 1 only) | LCD message bitmap, section 7 |
| `64` | (unnamed) | – | ? | sent by the vendor's message loader after a language change (N); hyp.: text direction |
| `63` | LCDContrast | – | ? | |
| `a0` | AccessInternalMemory | T: OUT | X* | Cypress FX2 RAM load (`a0 e600` = CPUCS reset), power-up only |
| `a2` | AccessEeprom | – | X | |
| `a3` | AccessExternalMemory | C, T: IN/OUT, v = address | ? | registers at `d7xx…dexx` (FPGA, hyp.) |
| `a4` | AccessPortOutputEnable | – | X | |
| `a5` | AccessPortValue | – | X | |
| `e0` | AccessAFE | C: IN/OUT len 1, v = register, i = channel 0/1 | ? | analog front end setup on open |
| `e1` | AccessSRAM0 | – | ? | |
| `e2` | (unnamed) | T: IN 256 | ? | |
| `e3` | AccessSerialFlash | C: IN 256 v=0003 i=fc00 | ? | calibration data read |
| `e4` | AccessColorLUT | – | ? | |
| `e5` | AccessTetraSRAM | – | ? | |
| `e7` / `e8` | AccessDSP0 / AccessDSP1 | – | ? | |
| `e9` | AccessJobSpec | – | ? | |
| `ea` | AccessJPEGCore | – | ? | |
| `eb` | AccessUserPrefLUT | – | ? | |
| `f0` | DiagnosticAction | – | X | |
| `f1` | DiagnosticRequest | T: OUT v=3 | X* | power-up only |
| `f2` | DiagnosticResults | T: IN 512 | ? | |

`X*`: part of the vendor's normal power-up initialisation (section 6). A native driver cannot
avoid it after a power cycle; it is to be handled as a verbatim replay of a full capture and
needs an explicit decision before any code sends it.

## 3. GetStatus block (`C0 00`, 32 bytes)

Field order is the vendor's own dump order (N); offsets are fitted to the observed bytes (C, P).
Example (idle, feeder empty, LCD showing `1`):
`03 00 01 02 0f 00 05 00 | tick | 03 01 00 01 02 00 00 01 01 00 00 01 01 00 ff 00 | 00 00 00 00`

| Offset | Size | Field | Idle value | Notes |
|---|---|---|---|---|
| 0 | 1 | bFwId | `03` | running firmware component; `01` right after power-up (T), before the downloads |
| 1 | 4 | dwVersion | `00 01 02 0f` | same 4 bytes as that component's GetFwVersions record |
| 5 | 1 | bBoardType | `00` | |
| 6 | 2 | (not named) | `05 00` | `00 00` before the power-up downloads (T) |
| 8 | 4 | dwTick | – | u32 LE, 1 ms per count (C: +2010 per 2.01 s) |
| 12 | 1 | bPowerState | `03` | |
| 13 | 1 | bBufferState | `01` | |
| 14 | 1 | bTransportState | `00` | |
| 15 | 1 | bTrayState | `01` | hyp.: paper present / absent |
| 16 | 1 | bUddsState | `02` | |
| 17 | 2 | wLampState | `00 00` | |
| 19 | 1 | bInterlockState | `01` | hyp.: cover closed |
| 20 | 1 | bButtonState | `01` | hyp.: function number on the LCD |
| 21 | 1 | bPrintHeadState | `00` | |
| 22 | 1 | bPrinterState | `00` | |
| 23 | 1 | bFrontCameraState | `01` | |
| 24 | 1 | bRearCameraState | `01` | |
| 25 | 1 | bCarriageState | `00` | |
| 26 | 1 | bLampIntensityState | `ff` | |
| 27 | 1 | bErrorCode | `00` | |
| 28 | 4 | – | `00` | |

The field offsets from 12 on are a fit (medium confidence) until a state change is observed
for each of them.

## 4. Interrupt events (EP `0x81`, `0x88`)

8-byte messages; the vendor logs them as eight bytes and names them by an event id (N). Which
byte carries the id and what the other bytes mean is **open** (no event captured yet).

| Id | Name | Id | Name |
|---|---|---|---|
| `00` | Dummy | `30` | Paper Jam |
| `01` | End of Operation | `31` | Multifeed |
| `02` | Start of Operation | `32` | Buffer Overflow |
| `03` | Ding | `33` | Print Rollover |
| `10` | Power State | `34` | Other Error |
| `11` | Buffer State | `40` | Notification Offset |
| `12` | Transport State | `41` | Page Exit |
| `13` | Tray State | `42` | Imaging Complete |
| `14` | Udds State | `43` | Mini Cal Paused |
| `15` | Lamp State | `44` | Setup Pause |
| `16` | Interlock State | `50` | Patch Detected |
| `17` | Carriage State | `51` | Patch Pause |
| `18` | Scanner State | `52` | DSP Reply |
| `19` | Elevator State | `60` | Screen Missing |
| `1a` | S0 Sensor State | `f0` | Debug |
| `20` | **Button Press** | `f1` | Trace Log |

## 5. GetFwVersions (`C0 02`, 56 bytes)

4-byte records; the owner's unit returns `00 02 01 0d`, `00 04 01 0e`, `00 01 02 0f`,
`00 1f 00 00`, then zeros. The vendor's component order is Booter, Loader, Scanner, Fpga (N),
which gives booter 2.1.13, loader 4.1.14, scanner 1.2.15, FPGA 0x1f (hyp. for the byte order).

## 6. Sequences

### Idle poll (C: `idle-poll-10s`)
Every 2 s (one `sane_start` on an empty feeder): USB descriptor reads, then `C0 00` twice.
Nothing else, so "is there paper" comes from the status block.

### Open of an initialised scanner (C: `open-A`, 0.8 s of traffic)
`00`×3 → `02` → `34` → `36` → `37` → `1f` SetTime → `03` → `35` read → `a3` writes (`da27`,
`db27`, 4 bytes each) → `e3` read → `37`×2 → `a3 d901` read/write → `e0` AFE register writes
with read-backs for channels 0 and 1 → `a3 d901` → `45` → `11` SetLamp 0 → `32` → `00` →
`62` LCDPopulate (768 bytes) → `35` **write** → `16` SetSequenceNumber → `00` … → `33` → `35` reads.

### Power-up open (T, payloads truncated to 32 bytes)
The scanner enumerates with a boot firmware (bcdDevice 1.02) and the host loads everything:
1. `21` FirmwareDownload, then FX2 RAM load: `a0 e600`=1 (hold CPU), 8 × 2048 bytes at
   `0000…3800` via `a0`, `a0 e600`=0 (run).
2. `20` v=1, 5,748,852 bytes on bulk EP `0x02`, `20` v=0 (FPGA configuration).
3. `f1` v=3, `f2` read 512 (hyp.: FPGA memory test).
4. Four banks of 32 KiB written with `a3` at `4000…b800` (bank select through `a3 d80e`).
5. `21` + a second FX2 RAM load (the scanner firmware), after which bcdDevice reads 2.01.
6. The normal open sequence, plus `18` SetAutoWhite and more `a3`/`e2` traffic.

The images are host-side files/resources of the vendor driver (`loader`, `fpga`, `scanner0…3`;
N). They are vendor firmware: never committed, to be extracted locally by the user.

## 7. Operator panel (LCD)

### LCDPopulate (`40 62`, 768 bytes)
The host renders text itself and uploads **bitmaps**; the scanner stores them as numbered messages.

- `wValue` = `(message id << 8) | message type`; `wIndex` = `0x0680` = 6 pages × 128 columns (N: the
  vendor wrapper builds wValue from two byte arguments and uses the constants `0x680` and length `0x300`).
- Data: 6 pages of 128 bytes. Each byte is one 8-pixel column, **bit 0 = top row**; page 0 is the
  top. The message area is therefore **128 × 48 pixels**. (C: the payload of `open-A` renders as the
  text "Rescan documents" in an ~11 px font, rows 5–14, starting at column 1.)
- P: the scanner accepts a payload rendered by us for type 4, id 1 (2026-10-08, no stall, status unchanged).
  What the panel then shows is **not verified** (needs eyes on the LCD).

Message types (N: the vendor's message table and loader; meanings are hyp. until seen on the panel):

| Type | Ids | Use |
|---|---|---|
| 1 | button number | text label of a function number (set from the TWAIN `SetOcpButtons` task: button number + text). Not sent by the SANE path, so **never captured** |
| 2 | 0…3 | fixed, translated messages, loaded when the stored `lcd_messages_version`/language differs |
| 3 | 1…3 | fixed, translated messages |
| 4 | 1, 2, 3, 4, 5, 8, 9 | fixed, translated messages. **Id 1 is uploaded on every open** ("Rescan documents"; the vendor calls it the "disconnected while scanning" message) |

The vendor log text "Failed to populate an message to EEPROM" says the messages are kept in
non-volatile memory (hyp.: all types). Uploading is thus a persistent write, but one the vendor
driver performs on every open.

### Function number
- GetStatus `bButtonState` reads `01` while the LCD shows function 1 (hyp.: it is the function number).
- SetSequenceNumber (`40 16`, wValue 1, wIndex 7 on every open): the vendor wrapper is called
  "set button sequence number" and takes two u16 (N). Hyp.: wValue = number to show, wIndex = highest
  selectable number.
