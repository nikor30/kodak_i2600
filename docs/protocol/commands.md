# Command layer

Status: **request table, status block, events, panel read-out, scan start/stop, raw image format, page trailers and the power-up replay known; per-mode setup registers still open.**
Last updated 2026-10-08. Every statement names its source; "hyp." marks a hypothesis.

Sources
- `C:` captures made on the Pi with `tools/usbcap/` (vendor driver under box64, firmware 1.2.15):
  `idle-poll-10s`, `open-A`, `scan-empty-gray200`, `station-scan-1` (one sheet, color 300 dpi duplex)
  (local, not committed: see `captures/README.md`).
- `W:` `kdsprobe.py watch` with the owner pressing buttons (`captures/local/watch-2.txt`, 2026-10-08).
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
| `10` | OperationStartStop | C: OUT len 0, v=1 start / v=0 stop | ? | section 8 |
| `11` | SetLamp | C: OUT len 0, v=1 before a scan, v=0 on open and ~28 s after a scan | ? | |
| `12` | SetButtonMode | – | ? | |
| `13` | AddEventToLog | – | X | writes the device log |
| `14` | ClearEventLog | – | X | |
| `15` | BatchPauseResume | – | ? | |
| `16` | SetSequenceNumber | C: OUT v=0001 i=0007 len 0, on every open and after the power-up | W (v=1; i = 2^n − 1, n = 1…9) | **wValue = function number to show, wIndex = bit mask of the numbers ▲/▼ offer**: i=7 → 1–3, i=0x1f → 1–5 with wrap-around (P + owner at the panel, 2026-10-08). Volatile; sets the selection to wValue. Masks with gaps are untried |
| `17` | StartCapture | C: OUT len 0, v=3 | ? | used for the short pre-scan capture only (section 8) |
| `18` | SetAutoWhite | T: OUT len 4 | ? | |
| `19` | SetOCPMode | – | ? | |
| `1a` | HomeCamera | – | ? | flatbed |
| `1b` | Ring | C: OUT v=0 len 0, right after events are enabled | ? | answered by two Ding events (`03`, values 1 and 2) |
| `1c` | ElevatorStartStop | – | ? | not on the i2600 |
| `1d` | ClearImageBuffer | – | ? | |
| `1e` | BatchBeginEnd | – | ? | |
| `1f` | SetTime | C: OUT v=3077 i=7e46 len 0 | W | **seconds since 2001-01-01 00:00:00 UTC**, u32: wValue = high word, wIndex = low word (C: two captures 156 s apart both give that epoch to the second) |
| `20` | FPGADownload | T: OUT v=1 … bulk … v=0 | X* | power-up only |
| `21` | FirmwareDownload | T: OUT v=0 | X* | power-up only |
| `22` | Disconnect | – | ? | |
| `23` | SubsystemFwUpdate | – | X | |
| `24` | BulkDownload | – | X | |
| `30` | ScannerConfiguration | C: OUT 120 bytes | ? | section 8 |
| `31` | EnergyStar | C: OUT 1 byte `01` before a scan | ? | |
| `32` | LampTimeout | C: IN 2 = `0e 3c`; OUT the same 2 bytes before a scan | R | |
| `33` | Meters | C, P: IN 20 | R | five u32 LE counters; meanings open |
| `34` | EOLConfiguration | C, P: IN 55 | R | VID, PID, vendor and product strings |
| `35` | NVRam | C: IN 128, IN 256 (v=1), **OUT 128** | R / X | the vendor driver rewrites it on every open; we do not write |
| `36` | SerialNumber | C, P: IN 16 ASCII | R | `0000000049374377` on the owner's unit |
| `37` | VRam | C: IN 64; OUT 64 (same content) before a scan | R | |
| `38` | ElevatorLoadPosition | – | ? | |
| `39` | SetPower | – | ? | |
| `3a` | InterruptEventControl | C, P: OUT len 0, **wValue 1, wIndex 1 = on / 0 = off** | W | off by default. (The vendor code suggested wValue = number of interrupt endpoints = 2; the wire shows 1.) |
| `3b` | Background | – | ? | |
| `3c` | UDDS Calibration | – | X | ultrasonic multifeed sensor calibration |
| `3e` | SendError | – | ? | |
| `3f` | Transport | – | ? | |
| `40` | Subsystem | – | ? | |
| `41` | Camera | – | ? | |
| `42` | PDMD | – | ? | |
| `43` | DSP | – | ? | |
| `44` | PdmdLED | – | ? | |
| `45` | BatchData | C: IN 3 = `02 00 00`; OUT `01 00 02` before a scan, `02 00 00` after | R | |
| `50` | Printer | – | ? | imprinter, not fitted |
| `5f` | OCPLEDControl | – | ? | |
| `62` | LCDPopulate | C, P: OUT v=0104 i=0680 len 768 | W (type 4 id 1; type 1 ids 1–9) | LCD message bitmap, section 7 |
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
needs an explicit decision before any code sends it. Decision: ADR-016 (replay of a locally
extracted capture, request allow-list, see "Native replay of the power-up open" in section 6).

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
| 12 | 1 | bPowerState | `03` | **`01` = standby** (S), `03` = idle, `04` = operating (C) |
| 13 | 1 | bBufferState | `01` | |
| 14 | 1 | bTransportState | `00` | |
| 15 | 1 | bTrayState | `01` | **`01` = feeder empty, `02` = paper in the feeder** (W, P) |
| 16 | 1 | bUddsState | `02` | `01` while the cover is open (W) and in standby (S) |
| 17 | 2 | wLampState | `00 00` | |
| 19 | 1 | bInterlockState | `01` | **`01` = cover closed, `02` = open** (W). **In standby it reads `02` with the cover closed** (S): only valid while bPowerState is not `01` |
| 20 | 1 | bButtonState | `01` | **the function number shown on the LCD** (W: ▲ → 2, ▼ → 1) |
| 21 | 1 | bPrintHeadState | `00` | |
| 22 | 1 | bPrinterState | `00` | |
| 23 | 1 | bFrontCameraState | `01` | |
| 24 | 1 | bRearCameraState | `01` | |
| 25 | 1 | bCarriageState | `00` | |
| 26 | 1 | bLampIntensityState | `ff` | |
| 27 | 1 | bErrorCode | `00` | |
| 28 | 4 | – | `00` | byte 28 read `02` in one poll right after ▲ (W); meaning open |

Tray, interlock and button offsets are confirmed by observed changes; the others are still a fit.

`S`: standby, seen on the owner's unit 2026-10-08/09 (F-095). The scanner went to standby by itself 29 min after
the last scan with events `15 00 00`, `10 00 01`, `16 00 02` in the same second. 15 hours later GetStatus still read
`03 00 01 02 0f 00 05 00 … 01 01 00 01 01 00 00 02 01 00 00 01 01 00 ff 00` (power 1, udds 1, interlock 2).
GetStatus every 2 s, LCDPopulate label uploads and InterruptEventControl do not wake it. What wakes it and which
events it sends then is not seen yet (Q-033).

## 4. Interrupt events (EP `0x81`, `0x88`)

8-byte messages, **all on EP `0x88`** (C, W; nothing seen on `0x81`), only while events are enabled
with request `3a`. Layout: **byte 0 = event id, byte 2 = new state / value**, bytes 1 and 3–4 carry
extra data for some events, the rest is zero. Names are the vendor's (N).

Observed (C = during the scan, W = owner at the panel):

| Bytes | Meaning |
|---|---|
| `60 00 nn 01 …` | function number changed to `nn` by ▲/▼ (W). Also sent with the current number when the scanner returns to idle (C). The vendor table calls id `60` "Screen Missing" |
| `20 01 nn 00 …` | **Start button pressed** while function `nn` is selected (W) |
| `13 00 02` / `13 00 01` | Tray State: paper inserted (W) / feeder ran empty (C) |
| `16 00 02` / `16 00 01` | Interlock State: cover opened / closed (W) |
| `03 00 01`, `03 00 02` | Ding, after request `1b` Ring (C) |
| `10 00 04` / `10 00 03` | Power State: 4 while an operation runs, 3 when idle (C) |
| `10 00 01` | Power State 1: the scanner enters standby; followed at once by `16 00 02` although the cover is closed (S, section 3) |
| `02 00 00` | Start of Operation (C) |
| `12 00 01`, `12 00 02`, `12 00 00` | Transport State: starting, running, stopped (C; names hyp.) |
| `42 00 00 00 nn` | Imaging Complete, `nn` = image count so far (C: 1 after the pre-scan capture, 2 after the sheet) |
| `01 rr ss 00 nn` | End of Operation (C: `01 00 00 00 00` after the pre-scan; `01 05 01 00 01` when the feeder ran empty after 1 sheet; `rr`/`ss` meanings open) |
| `15 00 00` | Lamp State: off, ~28 s after the scan (C) |

All known ids:

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

### Native replay of the power-up open (C: `power-up-1`, full payloads; ADR-016)
`tools/usbcap/extract_powerup.py` cuts the power-up open out of a capture into `powerup.json`
(request list) and `powerup.bin` (payloads ≥ 256 bytes and all bulk data). On the owner's unit:
743 steps, of which 351 bulk-OUT transfers on EP `0x02` with 5,748,852 bytes in total (the same
size as in T).

- Window: from the first GetStatus reporting firmware id 1 before the first `21` to the
  `11` SetLamp 0 that ends the vendor's open.
- Left out: `35` NVRam write and `62` LCDPopulate (permanent storage / not needed).
- Requests in the replay. OUT: `21`, `a0`, `20`, `f1` (v=3 only), `a3`, `37`, `1f`, `18`, `11`,
  `e0`, `30`, `17`. IN: `00`, `f2`, `a3`, `02`, `34`, `36`, `37`, `03`, `35`, `e3`, `e2`, `e0`,
  `32`, `33`. A file containing anything else is refused before the first request is sent.
- `1f` SetTime is sent with the current time, not the captured one.
- `00` GetStatus steps are waits: poll until the firmware id equals the captured one (the
  device does not answer while a freshly loaded firmware starts).
- After `17` (calibration capture) both image pipes are read until a short block, data discarded.
- Start condition: firmware id 1 (boot firmware). End condition: firmware id 3.
- Timing used by the working replay: the captured pause before a step is kept, capped at 2 s;
  control and bulk-OUT timeouts 5 s; a GetStatus wait polls every 0.1 s for at most 15 s (errors
  while the new firmware starts are ignored); the reads after `17` use 16,384-byte requests with a
  3 s timeout until a short block. A short bulk-OUT write is an error. The device keeps its USB
  address; the same handle is used throughout.
- **Text format for the C backend** (`powerup.seq`, beside the unchanged `powerup.bin`; made by
  `backend/tools/powerup2seq.py`), one step per line: `out`/`in` as in the scan sequence file
  (section 8), `outblob RR VVVV IIII OFFSET LENGTH PAUSE` (payload from the `.bin`),
  `bulk OFFSET LENGTH PAUSE` (EP `0x02`), `wait ID PAUSE` (GetStatus until the firmware id is ID).
- **After the replay the panel has no function number**: the LCD is blank, GetStatus `bButtonState`
  is 0 and a Start press arrives as `20 01 00` (scanning works all the same). The vendor driver sends
  `16` SetSequenceNumber v=1 i=7 half a second after the replayed window; sending exactly that sets
  `bButtonState` to 1, and the LCD shows the number and its label again, ▲/▼ work (owner, 2026-10-08).
  A driver sends it when `bButtonState` reads 0.

**Status: run once on the owner's unit (2026-10-08 15:06, F-075):** boot firmware to firmware
id 3 in 10.1 s, LCD labels accepted right after, station `ready`. A scan after a native
power-up and a second power-up with the same file are still to be shown. Arguments of `18`, `a3`, `e0`, `f1`/`f2` in
this sequence are not decoded; they are sent exactly as captured.

## 7. Operator panel (LCD)

### LCDPopulate (`40 62`, 768 bytes)
The host renders text itself and uploads **bitmaps**; the scanner stores them as numbered messages.

- `wValue` = `(message id << 8) | message type`; `wIndex` = `0x0680` = 6 pages × 128 columns (N: the
  vendor wrapper builds wValue from two byte arguments and uses the constants `0x680` and length `0x300`).
- Data: 6 pages of 128 bytes. Each byte is one 8-pixel column, **bit 0 = top row**; page 0 is the
  top. The message area is therefore **128 × 48 pixels**. (C: the payload of `open-A` renders as the
  text "Rescan documents" in an ~11 px font, rows 5–14, starting at column 1.)
- The vendor renders with cairo/pango: a 128 × 48 surface, font "Sans 9", wrapped at 128 px, no antialiasing (N).
- P: the scanner accepts a payload rendered by us for type 4, id 1 (2026-10-08, no stall, status unchanged).
  What the panel then shows is **not verified** (needs eyes on the LCD).

Message types (N: the vendor's message table and loader; meanings are hyp. until seen on the panel):

| Type | Ids | Use |
|---|---|---|
| 1 | button number | text label of a function number (set from the TWAIN `SetOcpButtons` task: button number + text). Not sent by the SANE path, so never captured. P (2026-10-08): uploads for ids 1–3 with wValue `(n << 8) \| 1` are accepted and **shown on the LCD next to the function number** (confirmed by the owner) |
| 2 | 0…3 | fixed, translated messages, loaded when the stored `lcd_messages_version`/language differs |
| 3 | 1…3 | fixed, translated messages |
| 4 | 1, 2, 3, 4, 5, 8, 9 | fixed, translated messages. **Id 1 is uploaded on every open** ("Rescan documents"; the vendor calls it the "disconnected while scanning" message) |

The vendor log text "Failed to populate an message to EEPROM" says the messages are kept in
non-volatile memory (hyp.: all types). Uploading is thus a persistent write, but one the vendor
driver performs on every open.

### Function number
- GetStatus `bButtonState` reads `01` while the LCD shows function 1 (hyp.: it is the function number).
- SetSequenceNumber (`40 16`, wValue 1, wIndex 7 on every open): the vendor wrapper is called
  "set button sequence number" and takes two u16 (N). wValue = number to show; **wIndex = bit mask of the
  selectable numbers**, not the highest number: with 7 the arrows cycle 1–3, with `0x1f` 1–5 (wrapping
  5 → 1 and 1 → 5), each number showing its type-1 label (P + owner, 2026-10-08). A number without an
  uploaded label shows only the number (hyp., from the blank state after power-up).

## 8. Scanning (C: `station-scan-1`, color 300 dpi duplex, one A4 sheet)

Order of requests after the usual open, with paper in the feeder:

1. `3a` events on, `1b` Ring, `32`/`31` power settings, `11` SetLamp 1, `45` BatchData `01 00 02`, `37` VRam write-back.
2. **Mode setup through registers**: ~50 `a3` writes (addresses `d9xx…ddxx`) and ~75 `e0` AFE writes, with
   read-backs, in 0.7 s. These depend on mode/resolution and are **not decoded**; other modes need their own capture.
3. `30` ScannerConfiguration (120 bytes, pre-scan variant) → `10` v=1 → events Power State 4, Start of
   Operation → `17` StartCapture v=3 → **154,948 bytes on each image pipe** (20 lines + 148 bytes; no paper
   moves: a capture of the background, the vendor's "baffle" scan) → `10` v=0 → Imaging Complete, End of Operation.
4. Six more `a3` writes, `30` ScannerConfiguration (scan variant) → `10` v=1. **No StartCapture**: the
   scanner feeds by itself. Events: Transport 1, 2, Start of Operation; image data starts ~0.1–0.3 s later.
5. Host reads both image pipes with 16,384-byte bulk requests until the feeder is empty: Tray State 1,
   Imaging Complete, Transport 0, End of Operation. The vendor then cancels its pending bulk reads. No control
   requests are sent while the sheet is scanned.
6. ~28 s later: `11` SetLamp 0, `45` BatchData `02 00 00`, `3a` events off.

### ScannerConfiguration (`40 30`, 120 bytes)
Mostly zero. The two variants differ only in bytes 1–2 (`00 01` pre-scan / `01 00` scan), byte 32
(`00` / `04`), byte 48 (`02` / `01`) and byte 54 (`33` / `03`). Common non-zero fields (u16 LE where two
bytes): offset 6 `0e5d` (3677), 10 `02fa` (762), 13–16 and 18–21 `0377`, `0170` (887, 368), 40 `0c36`
(3126), 42 `0c54` (3156), 50 and 52 `00c5` (197), 55 `0f`, 56 `01`. Field meanings are **open**.

### Image data (EP `0x82` front, `0x86` rear)
- **Uncompressed 8-bit RGB, pixel-interleaved, 2,580 pixels = 7,740 bytes per line**, top line first, no
  line or block headers (the two streams render as correct pictures of the sheet when cut every 7,740 bytes).
  2,580 px at 300 dpi = 8.6 inch: the full sensor width, with black background left and right of the sheet.
- The scanner sends raw sensor lines: black background above/below the sheet, slight colour cast, no crop,
  no deskew. Cropping, deskew, colour correction and blank-page detection are host work (the vendor's hippo).
- The rear stream starts ~0.2 s before the front one.
- The pre-scan block ends with tagged trailer bytes (`… 00 38 01 ff`); layout open.
- **Pages and trailers** (P: native scans of 1 and 3 sheets). Each side's stream is a plain sequence of
  pages. A page is a whole number of 7,740-byte lines (about 3,900–3,920 for A4: the sheet plus ~200
  background lines above and below), followed directly by a trailer:
  - 32 bytes of tags, each `value(2, big endian) 01 tag`, except the first which has a 6-byte value:
    `00 01 00 00 nn nn 02 f5` (nnnn = **image number**: 1 for the pre-scan block, then 2, 3, 4 … per
    sheet, the same on both sides), `01 f6` = 0, `01 f9` = 2, `01 fa` = 2, `01 fc` = `050a` (1290 = half
    the line width in pixels), `01 fd` = 0 (1 in the pre-scan block), `01 fe` = 0. Meanings other than the
    image number are open.
  - `2 × k` filler bytes (leftover pixel-like data), then `00 k 01 ff`.
  The next page's first line follows immediately. After the last trailer the pipes are silent.
- Events per sheet: `42 00 00 00 nn` Imaging Complete (image number) and `41 ss xx` Page Exit (ss = sheet
  count); at the end `01 05 ss 00 ss` End of Operation with ss = number of sheets fed.
- Speed (P): 3 A4 sheets, 181 MB, from OperationStart to End of Operation in 7.2 s.
- The front stream in the `station-scan-1` capture is short (3,113 lines) because the capture tool lost
  bulk events, not because the scanner sent less.

### Native replay (P, 2026-10-08)
`tools/kdsprobe/native_scan.py` replayed the 319 captured control requests of steps 1–4 verbatim (same
order and pauses) on an idle, vendor-initialised scanner with one sheet loaded, then read both pipes in
parallel with 256 KiB bulk requests until End of Operation. Result: the same event sequence as the vendor
run plus `41 01 01` Page Exit, and two complete images. So the sequence does not depend on host-side state.
51 of the IN replies differed from the capture: all are `a3` reads at `da1a/1c/1e` and `db1a/1c/1e`
(values that change from read to read; hyp.: live sensor/white-patch levels the vendor samples). Sending
the captured writes regardless still gave a good image.

Requests this makes usable for scanning in color 300 dpi duplex (class `W`, exactly as captured):
`3a`, `1b`, `32`, `31`, `11`, `45`, `37`, `30`, `10`, `17`, and the captured `a3`/`e0` register writes.

### Driver procedure for a scan (P: the native station, jobs of 1–3 sheets in color, 2026-10-08)
What a driver has to do around the replay; each point is what the working native station does.

**Before the start** (GetStatus): `bInterlockState` = 1 (cover closed), `bTrayState` = 2 (paper),
`bFwId` = 3 and `bErrorCode` = 0. Otherwise nothing is sent. The interface is claimed and events are
enabled (`3a` 1/1) for as long as the device is open; events are read from EP `0x88` in 8-byte
interrupt transfers.

**Replay rules** for the captured start sequence (steps 1–4 above):
- only the requests listed under "Native replay" may appear: OUT `3a`, `1b`, `32`, `31`, `11`, `45`,
  `37`, `a3`, `e0`, `30`, `10`, `17`; IN `00`, `32`, `35`, `37`, `a3`, `e0`. A sequence with any other
  request is refused as a whole;
- the captured pause before a request is kept, capped at 1 s;
- IN replies are not compared with the capture. Exception: the `37` VRam reply is kept and the
  following `37` write sends **these** bytes back, not the captured ones;
- directly after `17` StartCapture both image pipes are read with 16,384-byte requests until a
  short block arrives (the pre-scan block, discarded).

**While scanning**: one reader per image pipe, bulk reads of 256 KiB with a 500 ms timeout; no
control requests. Pages are cut at the trailers (above): test for the 32 tag bytes at every line
boundary (multiples of 7,740 bytes from the start of the page), then look for `00 k 01 ff` at
`2k` bytes after the tags for k = 0…255. If no k fits, the tag bytes were pixel data and the page
goes on. The next page starts directly after the trailer.

**End**: the **second** End of Operation event (`01`; the first one ends the pre-scan) marks the
end of the batch, its byte 4 is the number of sheets fed. The readers stop after two consecutive
read timeouts following that event. Then `11` SetLamp 0 and `45` BatchData `02 00 00` are sent.
More than 50 leftover lines without a trailer on a pipe mean the data ended inside a page.

**Errors and abort**: events `30` Paper Jam, `31` Multifeed, `32` Buffer Overflow, `34` Other Error
are recorded and the batch still ends with End of Operation. Interlock State = 2 (cover opened) or
no event at all for 30 s end the batch from the host side; in these cases, and when the host gives
up, `10` v=0 OperationStop is sent before the lamp-off. (Not observed: whether `10` v=0 stops the
feeder in the middle of a stack. The scanner feeds the whole stack on its own once started; a way
to ask for a single sheet is not known.)

**A start that does nothing** (P: seen once, 2026-10-08, first scan after 73 min without one; the
vendor stack shows I/O errors after ~30 min idle, too): the start sequence is accepted, but the scanner
sends no event and no data afterwards. A normal start reports Transport State `12 00 01` within 0.1 s
of the second `10` v=1, `12 00 02` and Start of Operation about 1 s later, and image data from ~2.8 s.
The backend therefore waits 6 s for any event; if none comes it sends the usual stop/lamp-off and
replays the start once more. Cause unknown (hyp.: the scanner rests after a long idle time and the
replayed pauses are too short for its wake-up).

**Sequence file for the C backend**: the same steps as text, one per line:
`out RR VVVV IIII HEXDATA|- PAUSE` or `in RR VVVV IIII LENGTH PAUSE` (request, wValue, wIndex in
hex; length decimal; pause in seconds; `#` starts a comment).
