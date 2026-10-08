# Tools

| Dir | What |
|---|---|
| `phase0/` | hardware info collection |
| `usbcap/` | `usbmon_capture.py` (full-payload capture from `/dev/usbmonN` into pcap, no dependencies) and `usbmon_decode.py` (pcap → transfer log / summary) |
| `re/` | `elftables.py`: read id→name tables out of the vendor libraries |
| `kdsprobe/` | native, read-only access to the scanner over libusb (no vendor driver): `kdsprobe.py info`, `kdsprobe.py watch`. Sends only requests classified as safe in `docs/protocol/commands.md` |

`kdsprobe/native_scan.py`: experimental native scan (replays a captured scan start, color 300 dpi duplex).
`kdsprobe/kds_image.py` + `kds_pages.py`: raw streams → cropped, deskewed, colour-corrected pages and a PDF (`docs/protocol/image-processing.md`).

Planned: `mockdev/` (replay device for backend tests).
