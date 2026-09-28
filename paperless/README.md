# Paperless-ngx integration

The upload client is part of the scan service: [`pi/scan-station/kodak_scand.py`](../pi/scan-station/kodak_scand.py)
(REST `/api/documents/post_document/`, spool + retry). Profiles (scan settings, title, tags)
live in `/etc/kodak-scan/config.yaml`; see [`pi/scan-station/config.example.yaml`](../pi/scan-station/config.example.yaml).

Planned: mapping the panel's function number (1–9) to profiles once the panel is reachable.
