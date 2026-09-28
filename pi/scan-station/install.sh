#!/usr/bin/env bash
# Install the scan station services on the Pi (Phase 5, ADR-010).
# Needs pi/phase1/setup-x86-chroot.sh and setup-box64.sh to have run first.
#
#   sudo ./install.sh          # install/update and (re)start the services
#   sudo ./install.sh --remove # stop and remove them (config and spool are kept)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ROOT:-/opt/kodak-x86}"
LIB=/usr/local/lib/kodak-scan
ETC=/etc/kodak-scan

log() { printf '\033[1;34m[scan-station]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[scan-station] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"

if [ "${1:-}" = "--remove" ]; then
  systemctl disable --now kodak-scand.service kodak-saned.service 2>/dev/null || true
  rm -f /etc/systemd/system/kodak-scand.service /etc/systemd/system/kodak-saned.service
  rm -rf "$LIB"
  systemctl daemon-reload
  log "removed (kept $ETC and /var/lib/private/kodak-scan)"
  exit 0
fi

[ -x /usr/local/sbin/kodak-x86 ] || die "kodak-x86 missing: run pi/phase1/setup-x86-chroot.sh first"
[ -x "$ROOT/usr/local/bin/box64" ] || die "box64 missing: run pi/phase1/setup-box64.sh first"

log "Installing packages"
apt-get install -y --no-install-recommends python3-sane python3-pil python3-requests python3-yaml img2pdf libsane1 >/dev/null

log "saned in the chroot: allow 127.0.0.1"
grep -qx '127.0.0.1' "$ROOT/etc/sane.d/saned.conf" || echo 127.0.0.1 >>"$ROOT/etc/sane.d/saned.conf"

log "Installing $LIB and the systemd units"
install -d "$LIB/sane.d"
install -m 755 "$HERE/kodak_scand.py" "$LIB/"
install -m 644 "$HERE/sane.d/dll.conf" "$HERE/sane.d/net.conf" "$LIB/sane.d/"
install -m 644 "$HERE/README.md" "$LIB/"
install -m 644 "$HERE/kodak-saned.service" "$HERE/kodak-scand.service" /etc/systemd/system/

install -d -m 755 "$ETC"
[ -f "$ETC/config.yaml" ] || { install -m 644 "$HERE/config.example.yaml" "$ETC/config.yaml"; log "created $ETC/config.yaml: set paperless.url"; }
# LoadCredential= needs the file to exist; an empty token means "not configured yet".
[ -f "$ETC/paperless-token" ] || install -m 600 /dev/null "$ETC/paperless-token"
chmod 600 "$ETC/paperless-token"

systemctl daemon-reload
systemctl enable kodak-saned.service kodak-scand.service
systemctl restart kodak-saned.service kodak-scand.service
log "Done. Logs: journalctl -u kodak-scand -u kodak-saned -f"
[ -s "$ETC/paperless-token" ] || log "Paperless token not set yet: scans are kept in the spool until you add it (see README)"
