# USB captures

Captures of our own device, made on the Pi with `tools/usbcap/usbmon_capture.py`
(pcap, DLT_USB_LINUX; open with Wireshark or `tools/usbcap/usbmon_decode.py`).

**Raw captures stay in `captures/local/` (git-ignored)**: a power-up capture contains the
vendor firmware and a scan capture contains the scanned documents. Only the index below and
decoded facts (`docs/protocol/`) are committed.

| File (in `local/`) | Date | Driver | What |
|---|---|---|---|
| `idle-poll-10s.pcap` | 2026-10-08 | vendor v4.14, box64, via kodak-scand | 10 s of the 2 s paper poll, feeder empty |
| `open-A.pcap` | 2026-10-08 | vendor, `scanimage -A` | open + close of an initialised scanner |
| `scan-empty-gray200.pcap` | 2026-10-08 | vendor, `scanimage --mode Gray --resolution 200` | scan attempt, feeder empty → "out of documents" |
| `station-scan-1.pcap` | 2026-10-08 | vendor, via kodak-scand | recovery from a stale saned, then one A4 sheet color 300 dpi duplex (53 MB of image data; contains the scanned sheet) |
| `power-up-1.pcap` | 2026-10-08 | vendor, run by kodak-native's fallback | power-up open after a power cycle, full payloads (**contains the vendor firmware**); source of the local `powerup.json`/`.bin` (F-067) |
| `watch-2.txt` | 2026-10-08 | native `kdsprobe.py watch` | owner pressed ▲, ▼, Start, opened/closed the cover, inserted a sheet |
