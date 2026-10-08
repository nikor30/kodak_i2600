# Native SANE backend `kodak_i2x00` (Path C)

A SANE backend in C for the Kodak i2600 (USB `040a:601d`), written from `docs/protocol/`
only. No vendor code, no emulation.

**State (2026-10-08): first version.** Built and tested offline against saved image streams;
on the scanner, detection, open, options, the empty-feeder case and **one sheet in color duplex**
are tested. Stacks, Gray/Lineart on the device, spooling and cancelling are not.

## What it does
- Finds the scanner, opens it, reads the panel: `--scan` (Start button pressed),
  `--function-number`, `--page-loaded`, `--cover-open`.
- Scans color 300 dpi duplex by replaying the captured start sequence and delivers
  `Color`, `Gray` or `Lineart` (`--threshold`), `ADF Front` or `ADF Duplex`.
- Each page is cropped to the sheet and colour-corrected (`--raw` turns both off).

## What it does not do yet
- No deskew, no blank-page removal, no resolution other than 300 dpi, no geometry options.
- No power-up initialisation: after the scanner was switched off and on, `sane_open` fails
  with an I/O error until something else has loaded the firmware (the `kodak-native` service
  does that).
- No LCD labels.
- The i2400 and i2800 are untested (add their USB ids in `kodak_i2x00.conf` to try).

## Build, test, install
Needs `libsane-dev`, `libusb-1.0-0-dev`, `pkg-config`, `python3` (to convert the sequence file).

    make                 # build/libsane-kodak_i2x00.so.1
    make check           # offline tests, no scanner needed
    make testenv         # try it without installing:
    LD_LIBRARY_PATH=build SANE_CONFIG_DIR=build/conf scanimage -L
    sudo make install    # library, /etc/sane.d/kodak_i2x00.conf, dll.d entry, sequence file

`make check TEST_STREAMS="front.raw rear.raw"` also runs the page splitter and sheet detection
over image streams saved from a real scan.

Only one program can have the scanner open: stop `kodak-native.service` before using the
backend, or `sane_open` returns "Device busy".

## Scanning a stack: use batch mode
The scanner feeds **the whole stack by itself** once a scan is started; the host cannot ask
for one sheet (as far as is known). The backend therefore works in batches:

    scanimage -d kodak_i2x00:usb:001:009 --format=png --batch=page%03d.png

- The first `sane_start` starts the batch; every `sane_start` returns the next side (front,
  then rear with `ADF Duplex`); `SANE_STATUS_NO_DOCS` ends it.
- Pages the frontend has not fetched yet wait in memory (4 pages by default) and then in files
  in `/var/tmp` (`memory-pages`, `spool-dir` in the config file). A raw page is 30 MB.
- `sane_cancel` after a completely read page keeps the rest of the batch for the next
  `sane_start`. `sane_cancel` inside a page, or `sane_close`, **discards the remaining pages**,
  although the sheets have gone through the scanner. A frontend that scans one page and closes
  loses the rest of the stack.

## Files
| File | Content |
|---|---|
| `kodak_i2x00.c` | SANE API: devices, options, frames |
| `kds_dev.c` | USB requests (with the allow-list), events, sequence replay, batch threads, page spool |
| `kds_split.c` | cuts an image stream into pages at the trailers |
| `kds_image.c` | sheet detection, colour correction, gray, black/white |
| `tools/json2seq.py` | converts `pi/scan-station/sequences/*.json` to the backend's text format |
| `tests/test_offline.c` | tests without hardware |

Safety: `kds_dev.c` refuses every request that is not in the scan list of
`docs/protocol/commands.md` section 8, both in its own calls and in a sequence file.

Debug output: `SANE_DEBUG_KODAK_I2X00=1…4`.

Not using `sanei_usb`/`sanei_config` (they are not installed with `libsane-dev`); a port to
them is needed before this can go upstream into sane-backends.
