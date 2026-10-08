# devicemanager.so: where the wire protocol lives

Analysed 2026-10-08 (v4.14 x86_64). Facts and names only, no vendor code.

- `device.so` is only the generic USB/XML plumbing. The i2x00-specific protocol is in
  **`/opt/kodak/kds_i2000/lib/devicemanager.so`** (5.1 MB, 5651 symbols, namespace `DeviceManager`).
- The binaries have full C++ symbol tables but **almost no DWARF** (`.debug_info` is 284 bytes),
  which corrects F-011: there are no struct layouts or enum values to read from debug info.
- Classes (method count): `FpgaBase` 173, `DeviceSettings` 124 (one method per USB request:
  `GetStatus`, `SetTime`, `SetOCPLED`, `SetButtonSequenceNumber`, `StartCapture`, …),
  `SetupScanner` 54, `Scanner` 49 (state machine, `HandleButtonPressEvent`, `HandleTrayStateEvent`,
  `StartInterruptEvents`), `ProcessCommands` 49 (the XML task vocabulary of F-015),
  `InitializeScanner` 25 (`LoadFirmware`, `ProgramFpga`, `LoadBankedFirmware`, `PuffBuffer`),
  `OCP` 14 (`LcdPopulate`, `PopulateMessage`, `SetSequenceNumber`, `DetectLCD`), `ImageManager`, `Image`.

## Name tables (read with `tools/re/elftables.py`)

| Table | How it is stored | Content |
|---|---|---|
| USB request code → name | a `std::map<unsigned char, std::string>` filled in the `DeviceSettings` constructor (byte constant + string per entry) | 69 entries, in `docs/protocol/commands.md` §2 |
| `s_eventtidtable` | 32 records {u8 id, char*} | interrupt event names, `commands.md` §4 |
| `s_stateidtable` | 14 records {u32 id, char*} | driver state machine: Error, Disconnected, NotReady, Initializing, Idle, SettingUpForScan, ScanningBaffles, ResetForBaffleCapture, Calibrating, Scanning, PausePending, Paused, WaitingOnQEmpty, WaitingOnGetOutput |
| `s_commandidtable`, `s_dmeventidtable` | same record shape | internal command/event ids (not wire level) |

- Status block field names come from the format strings of the vendor's status dump
  (`bFwId`, `dwVersion`, `bBoardType`, `dwTick`, `bPowerState` … `bErrorCode`).
- Firmware component names in the package loader: `booter`, `bootimage`, `loader`, `fpga`,
  `scanner`, `scanner0…3`, `scannerx`, `elevator`, `udds`, `ocp`, `dsp`, `scanmod`, `udds_config`
  (each `<name>.bin`); loaded from files or from resources embedded in the library.
- Board hint string: `Board Type … Fpga2C35:0 Fpga2C20:1` (Altera Cyclone II, hyp.).
