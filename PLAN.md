# Kodak i2600 → Raspberry Pi → Paperless-ngx: Project Plan

> Goal: turn a **Kodak i2600** document scanner and a **Raspberry Pi** into a
> headless scan station. Pressing the scanner's own buttons scans straight
> into **Paperless-ngx**, and the scanner's **operator panel (LCD + buttons)**
> is used to choose the scan profile.
>
> This is the master plan. The project's running memory (status, findings,
> decisions, journal) lives in [`CLAUDE.md`](CLAUDE.md) and [`memory/`](memory/).

---

## 1. Research summary: is the i2600 already supported?

| Question | Answer | Confidence / source |
|---|---|---|
| USB ID of the i2600 | `040a:601d` (vendor `040a` = Kodak) | High. SANE `doc/descriptions-external/kodak-twain.desc` |
| Sibling models, same family | i2400 `040a:601c`, i2800 `040a:601e`. Successors i2420/i2620/i2820 (IDs to be checked) | High for the IDs, medium for protocol sameness |
| Flatbed accessories | i1000 A4 flatbed `040a:6011`, A3 flatbed `040a:6012` (these are separate USB devices) | High (same `.desc`) |
| Open-source SANE backend? | **None.** Upstream SANE lists the i2x00 family only under the external, proprietary `kodak-twain` backend, with status "untested" | High |
| `sane-kodak` backend (upstream) | Covers older i-series machines (i1860, i8xx, i6xx …) over **SCSI/IEEE-1394**, not the USB i2000 family. Its SCSI command set is still a strong **reference point** for our protocol work | High that it doesn't cover the i2600; the command-set overlap is a hypothesis |
| `sane-kodakaio` backend | For Kodak ESP/Hero inkjet all-in-ones. **Not relevant** | High |
| Vendor Linux driver | Kodak Alaris "scanonlinux" SANE + TWAIN driver **v4.14**, `.deb` packages for **x86 (i586) and x86_64 only**. No ARM/aarch64 build | High. kodakalaris.com i2600 support page |
| Vendor Windows driver | v5.01 (TWAIN/ISIS/WIA), installer `.exe` or ISO | High |
| Community reverse engineering | None found | Medium (absence of evidence) |
| Does the vendor Linux driver work? | Yes on x86. A 2013 sane-devel report calls it "works very well" but hard to install, 32-bit only at the time | Medium |

**Conclusion:** no ready-made driver runs natively on a Raspberry Pi (ARM). We
do, however, have a **Linux x86 vendor driver**. It is far easier to analyse
than the Windows driver, and it gives us a working reference to capture USB
traffic from. The Windows driver is a secondary source, used where the Linux
driver is incomplete (for example firmware/panel features).

---

## 2. Strategy: cheapest working path first

We use three paths in order. Each one is a usable deliverable, so the project
produces value early even if the full clean-room driver takes months.

```
Path A (days)      Path B (weeks)               Path C (months)
Vendor x86 driver  Protocol capture + RE        Native open-source
under emulation →  (usbmon, LD_PRELOAD shim, →  SANE backend "kodak_i2x00"
on the Pi (box64)  Ghidra on .so / Windows DLL) for ARM, upstreamable
        \______________________________________________/
                          |
               Scan daemon + panel/buttons + Paperless upload
               (the same on every path, because it only talks to SANE)
```

* **Path A: emulation.** Run the x86_64 Kodak SANE driver on an aarch64 Pi
  with **box64** (or `qemu-user` + binfmt as a fallback). If this works, we
  have a working scan station early and can build the Paperless pipeline in
  parallel.
* **Path B: protocol recovery.** Capture and decode the USB protocol.
* **Path C: native backend.** Write a clean-room C SANE backend from the
  protocol spec. Target: upstream `sane-backends`, benefiting i2400/i2600/i2800.

Design rule: **everything above the driver talks only to SANE**. A driver
swap (A → C) must not touch the scan daemon or the Paperless integration.

---

## 3. Legal & ethical ground rules

* Reverse engineering is for **interoperability** (EU Software Directive
  2009/24/EC Art. 6, § 69e UrhG in Germany; US DMCA §1201(f)).
* **Never commit vendor binaries, firmware, or decompiled vendor code.** They
  go in `vendor/` (git-ignored). Only our own notes, protocol descriptions,
  and captures of *our own* device traffic are committed.
* **Clean-room split:** `re/` documents *what* the device does (protocol
  facts). `backend/` is written from `docs/protocol/` only, not from
  decompiled listings.
* Firmware is never flashed or modified. Commands with unknown effects are not
  sent to the device until classified as safe (see §6.4).

---

## 4. Repository structure

```
kodak_i2600/
├── PLAN.md                 ← this file (master plan, phases, exit criteria)
├── CLAUDE.md               ← project memory entry point (auto-loaded by Claude Code)
├── README.md               ← short public overview
├── memory/                 ← long-term project memory, versioned in git
│   ├── STATUS.md           ← current phase, next actions, blockers
│   ├── FINDINGS.md         ← verified facts (each with source + confidence)
│   ├── DECISIONS.md        ← architecture decision log (ADR-style)
│   ├── OPEN_QUESTIONS.md   ← unknowns to resolve, with owner/phase
│   └── JOURNAL.md          ← dated session log (append-only)
├── docs/
│   ├── hardware/           ← i2600 specs, panel, ports, photos, USB descriptors
│   ├── protocol/           ← the clean-room protocol specification (source of truth for backend/)
│   └── setup/              ← Pi install guides, Paperless config
├── research/
│   └── similar-models.md   ← SANE backends/protocols of related scanners
├── re/                     ← reverse-engineering notes (no vendor code!)
│   ├── linux-driver/       ← notes on the Kodak Linux .so files (symbols, call graph)
│   ├── windows-driver/     ← notes on the INF/DLLs of the Windows driver
│   └── ghidra/             ← exported Ghidra *project notes* / scripts (not databases of vendor code)
├── captures/               ← USB captures (.pcapng) of our own device + index.md
├── tools/                  ← our tooling: LD_PRELOAD libusb logger, pcap → protocol decoder, Wireshark dissector
├── backend/                ← native SANE backend (Path C), C, sane-backends style
├── pi/                     ← Raspberry Pi integration: scan daemon, scanbd config, systemd units
├── paperless/              ← upload client, consume-folder setup, profiles
└── vendor/                 ← (git-ignored) downloaded Kodak drivers/ISOs
```

---

## 5. Hardware facts to establish (Phase 0)

What we know or assume about the i2600. Verify each item and record it in
`memory/FINDINGS.md`.

* Duplex ADF, ~50 ppm / 100 ipm, 75-sheet feeder (spec sheet, to confirm).
* Interface: **USB 2.0** (Type-B). Get `lsusb -v` descriptors: interfaces,
  endpoints (expect bulk-in/bulk-out, maybe interrupt-in for buttons/events).
* **Operator control panel:** small LCD showing a *function number* (1–9,
  "Smart Touch" jobs) and error codes; **Start/Resume** button; **scroll**
  buttons to select the function. Open questions:
  * Does the host *read* the selected function number? (Almost certainly, for
    Smart Touch.)
  * Can the host *write* to the LCD (custom digits/text)? Unknown.
  * Is button state polled (a command) or pushed (interrupt endpoint)?
* **Ports:** USB-B + power in. Does an accessory port exist for the flatbed or
  imprinter, or do accessories use their own USB cable (IDs `6011`/`6012`)?
* Sensors to expose: paper present, multifeed (ultrasonic) detection, cover
  open, jam, lamp/LED state, counters (page/roller counts for maintenance).

---

## 6. Phases, tasks, and exit criteria

### Phase 0: Inventory & baseline (≈1 week)
- [ ] Record hardware: model label, firmware version, serial, `lsusb -v` dump → `docs/hardware/`.
- [ ] Download the Kodak Linux driver v4.14 (x86_64 `.deb.tar.gz`) and the Windows driver v5.01 into `vendor/` (ignored).
- [ ] Inventory the driver packages: list every file, ELF/PE type, exported symbols, strings → `re/linux-driver/inventory.md`, `re/windows-driver/inventory.md`.
- [ ] Read the Windows `.inf`: which kernel driver binds the device? (Hypothesis: Microsoft's generic **`usbscan.sys`**. If so, the whole protocol lives in *user-mode* DLLs and **no kernel-driver disassembly is needed**.)
- [ ] Set up an x86_64 reference machine (a PC or a VM with USB passthrough) and scan successfully with the vendor driver (`scanimage -L`, `scanimage --help -d kds_i2000:…`).
- [ ] Dump the full SANE option list of the vendor backend → `docs/protocol/sane-options-vendor.md`. This is the **feature list our backend must reach**.

**Exit:** a working reference scan on x86, a file inventory of both drivers, and the USB descriptors recorded.

### Phase 1 (Path A): vendor driver on the Pi via emulation (≈1 week, time-boxed)
- [ ] Pi 4/5, Raspberry Pi OS Lite **64-bit**, install `box64` (plus `box86` if only the 32-bit build works).
- [ ] Install the x86_64 SANE stack and the Kodak `.so` files into an x86_64 sysroot. Run `scanimage` under box64.
- [ ] Fallback: `qemu-user-static` + binfmt with a Debian amd64 chroot.
- [ ] Measure: pages/min, CPU, stability across 200-page batches, button readout via `scanimage -A`.

**Exit:** either (a) reliable scanning on the Pi, which becomes the **interim production driver**, or (b) a documented failure reason. Either way, continue to Phase 2.

### Phase 2 (Path B): USB protocol capture (≈2–3 weeks)
Least-effort, highest-yield techniques first:
1. **`usbmon` + Wireshark** on the x86 Linux reference machine while running scripted scans (`captures/`).
   One capture per variable: resolution, colour mode, simplex/duplex, paper size, compression, button presses, panel function change, error states (jam, open cover, multifeed).
2. **libopenusb logging shim** (`tools/openusb-logger/`, ADR-005). The vendor driver `dlopen`s `/usr/local/lib{,64}/libopenusb.so` (not libusb, F-012), so a drop-in library at that path logs every `openusb_*_xfer` with pipe and endpoint context and forwards to the real library.
3. **Windows side** (only for features missing on Linux): USBPcap + Wireshark, API Monitor on `DeviceIoControl`/`ReadFile`/`WriteFile` to the `usbscan` handle.
4. Write `tools/decode/`: a pcap → annotated command log decoder, later a **Wireshark Lua dissector**.

Known from Phase 0 (F-014): separate pipes for commands (EP2 OUT), front image (EP2 IN), rear image (EP6 IN) and two interrupt/event pipes (EP1, EP8). The driver binaries contain no SCSI strings (F-016), so the next hypothesis is now **unlikely**; it is kept only as a reference. Original hypothesis: the transport is **SCSI-style command blocks over USB bulk** (a CDB out, data in/out, status in). This is common to Kodak/Fujitsu/Canon document scanners and would line up with the upstream `sane-kodak` command set (compare `backend/kodak-cmd.h` in sane-backends). Related `canon_dr`/`fujitsu` backends are good structural templates.

**Exit:** `docs/protocol/` describes the transport framing, INQUIRY/identify, set-window/parameters, start scan, read image data (format, compression, duplex interleave), end/cancel, sense/error codes, button/panel status.

### Phase 3: static analysis to fill gaps (in parallel with Phase 2)
- **Linux `.so` first** (ELF, often with symbols): load into **Ghidra**, find the transport layer by cross-referencing libusb imports, name the command builders, and recover the enum of opcodes/option IDs. Notes go in `re/linux-driver/`.
- **Windows DLLs second**: Ghidra/IDA Free + x64dbg. Look for the TWAIN data source (`.ds`), the KDS core DLLs, and the WIA mini-driver. Entry points: imports of `DeviceIoControl`, `CreateFileW("\\\\.\\Usbscan…")`, SetupAPI. Look for panel/Smart Touch features, firmware counters, and imprinter control.
- Record **facts only** (opcode X = function Y, payload layout) in `docs/protocol/`. No decompiled code is copied into the repo.

**Exit:** every opcode seen in captures is explained, and the panel read (and write, if it exists) mechanism is identified.

### Phase 4 (Path C): native SANE backend `kodak_i2x00` (≈1–2 months)
- [ ] Scaffold in `backend/` following sane-backends conventions (`sanei_usb`, `sanei_config`, `.desc`, man page).
- [ ] Milestones: (1) detect + INQUIRY, (2) simplex grey 300 dpi uncompressed, (3) colour + duplex, (4) JPEG transfer, (5) ADF paper sensing + multifeed, (6) hardware buttons + function number as SANE sensor options (`button-start`, `function-number`, …), (7) panel write (if supported).
- [ ] Safety: a whitelist of known-safe opcodes. Unknown opcodes are refused in normal builds.
- [ ] Test harness: replay captured sessions against a **mock device** (`tools/mockdev/`) so CI runs without hardware.
- [ ] Cross-compile/build natively for aarch64. Validate against the Phase 0 option list.

**Exit:** `scanimage` on the Pi gives output identical in function to the vendor driver for the core modes, with buttons readable. Then submit upstream to sane-backends.

### Phase 5: Pi scan station + Paperless integration (can start after Phase 1)
- **Button handling:** `scanbd` (polls SANE sensor options) *or* a small custom daemon `pi/kodak-scand` (Python, `python-sane`), which gives direct control over the panel.
- **Panel mapping:** the function number (1–9) on the LCD selects a profile in `paperless/profiles.yaml`. Examples:
  - 1 = colour duplex 300 dpi PDF, 2 = B/W 300 dpi, 3 = photo 600 dpi, 9 = "split batch by patch/blank-page separator".
  - Profiles can set Paperless **tags / correspondent / document type / storage path**.
- **Pipeline:** scan → TIFF/JPEG pages → blank-page removal → `img2pdf` → (optionally `ocrmypdf`, otherwise let Paperless do OCR) → **upload**.
- **Upload:** Paperless-ngx REST API `POST /api/documents/post_document/` with a token (preferred, supports tags/metadata), or write to the **consume folder** over SMB/NFS (simplest).
- **Robustness:** a local spool queue (retry when Paperless is offline), an idempotent upload ID, and journald logging. The panel shows an error code or a status LED pattern when an upload fails, if the panel can be written.
- **Ops:** systemd units, read-only root / overlayfs option, config in `/etc/kodak-scan/`, optional small status web page.

**Exit:** press Start on the scanner → the document appears in Paperless with the profile's tags in under 60 s, and it survives a Paperless outage.

### Phase 6: Hardening & release
- Documentation in `docs/setup/`, a Pi image or Ansible role, and upstream PRs (SANE backend, `.desc` entries for i2400/i2600/i2800 as "basic/good").

---

## 7. Toolchain

| Purpose | Tools |
|---|---|
| USB capture | `usbmon`, Wireshark, USBPcap (Windows), `lsusb -v`, `usbutils` |
| Dynamic tracing | LD_PRELOAD shim, `ltrace`, `strace -f`, `gdb`; Windows: API Monitor, x64dbg, Procmon |
| Static RE | Ghidra (primary), IDA Free, `readelf`/`objdump`/`nm`, `strings`, `binwalk` (installer extraction), `innoextract`/`7z`/`msiextract` |
| Emulation on Pi | box64 / box86, `qemu-user-static` + binfmt |
| Backend | C, sane-backends tree, `sanei_usb`, meson/autotools as upstream, `scanimage`, `sane-troubleshoot` |
| Pi service | Python 3, `python-sane` or `scanimage` subprocess, `scanbd`, `img2pdf`, `ocrmypdf`, `requests`, systemd |
| Paperless | Paperless-ngx REST API (token auth), consume folder |

---

## 8. Risks & mitigations

| Risk | Mitigation |
|---|---|
| box64 can't run the vendor stack (threads, dlopen chains) | qemu-user fallback. Otherwise skip to Path B/C, or use an x86 mini-PC as an interim `saned` server |
| Encrypted/obfuscated protocol or firmware handshake | Replay captured handshakes verbatim first, then analyse the Linux `.so` for the algorithm |
| Image data in proprietary compression | Capture uncompressed modes first; JPEG is likely standard |
| Panel can't be written by the host | Use the function number (read-only) plus Pi-side feedback (LED/buzzer/web page) |
| Bricking via unknown commands | Opcode whitelist, no firmware update commands, captured-only command replay |
| Loss of project context over months | Git-versioned memory (`CLAUDE.md` + `memory/`), updated every session |

---

## 9. Immediate next actions
1. Plug the i2600 into a Linux box and commit `lsusb -v -d 040a:601d` to `docs/hardware/usb-descriptors.txt`.
2. Download the Kodak Linux v4.14 x86_64 driver + Windows v5.01 into `vendor/`. Write the inventories.
3. Get a reference scan working on x86 and dump its SANE options.
4. Try Path A (box64) on the Pi.

## Sources
- SANE external backend list, `kodak-twain.desc` (USB IDs): <https://gitlab.com/sane-project/backends/-/blob/master/doc/descriptions-external/kodak-twain.desc>
- SANE `kodak` backend description: <https://gitlab.com/sane-project/backends/-/blob/master/doc/descriptions/kodak.desc>
- SANE `kodakaio` backend: <http://www.sane-project.org/man/sane-kodakaio.5.html>
- Kodak Alaris i2600 page & drivers: <https://www.kodakalaris.com/en/scanners/i2600-scanner>
- i2000 Series User Guide: <https://www.kodakalaris.com/sites/default/files/2025-03/A61677_i2xx0_UserGuide_180820_en_0.pdf>
- sane-devel 2013 report on the i2600: <https://alioth-lists.debian.net/pipermail/sane-devel/2013-April/031233.html>
