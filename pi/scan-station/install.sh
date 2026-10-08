#!/usr/bin/env bash
# Install the scan station services on the Pi (Phase 5, ADR-010).
# Needs pi/phase1/setup-x86-chroot.sh and setup-box64.sh to have run first.
#
#   sudo ./install.sh                 # install/update, run the vendor-driver station (kodak-scand + kodak-saned)
#   sudo DRIVER=native ./install.sh   # install/update, run the native station (kodak-native, Python driver)
#   sudo DRIVER=sane ./install.sh     # install/update, build and install our SANE backend, run kodak-sane
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
  systemctl disable --now kodak-web.service kodak-oled.service kodak-sane.service kodak-native.service kodak-scand.service kodak-saned.service 2>/dev/null || true
  rm -f /etc/systemd/system/kodak-web.service /etc/systemd/system/kodak-sane.service /etc/systemd/system/kodak-native.service /etc/systemd/system/kodak-oled.service /etc/systemd/system/kodak-scand.service /etc/systemd/system/kodak-saned.service
  rm -rf "$LIB"
  systemctl daemon-reload
  log "removed (kept $ETC and /var/lib/private/kodak-scan)"
  exit 0
fi

[ -x /usr/local/sbin/kodak-x86 ] || die "kodak-x86 missing: run pi/phase1/setup-x86-chroot.sh first"
[ -x "$ROOT/usr/local/bin/box64" ] || die "box64 missing: run pi/phase1/setup-box64.sh first"

log "Installing packages"
apt-get install -y --no-install-recommends python3-sane python3-pil python3-requests python3-yaml img2pdf libsane1 \
  python3-smbus2 python3-numpy libusb-1.0-0 fonts-dejavu-core python3-ruamel.yaml smbclient >/dev/null

# OLED on the PoE HAT (B): needs I2C (dtparam=i2c_arm=on + i2c-dev), which raspi-config sets up.
if [ ! -e /dev/i2c-1 ] && command -v raspi-config >/dev/null; then
  log "Enabling I2C for the OLED"
  raspi-config nonint do_i2c 0
fi

log "saned in the chroot: allow 127.0.0.1"
grep -qx '127.0.0.1' "$ROOT/etc/sane.d/saned.conf" || echo 127.0.0.1 >>"$ROOT/etc/sane.d/saned.conf"

log "Installing $LIB and the systemd units"
install -d "$LIB/sane.d"
install -m 755 "$HERE/kodak_scand.py" "$HERE/kodak_oled.py" "$HERE/kodak_native.py" "$HERE/kodak_sane.py" "$HERE/kodak_web.py" "$LIB/"
install -d "$LIB/web"
install -m 644 "$HERE"/web/* "$LIB/web/"
install -m 644 "$HERE/kds_usb.py" "$HERE/kds_scan.py" "$HERE/kds_image.py" "$HERE/kodak_deliver.py" "$LIB/"
install -d "$LIB/sequences"
install -m 644 "$HERE"/sequences/*.json "$LIB/sequences/"
install -m 644 "$HERE/sane.d/dll.conf" "$HERE/sane.d/net.conf" "$LIB/sane.d/"
install -m 644 "$HERE/README.md" "$LIB/"
install -m 644 "$HERE/kodak-saned.service" "$HERE/kodak-scand.service" "$HERE/kodak-oled.service" "$HERE/kodak-native.service" "$HERE/kodak-sane.service" "$HERE/kodak-web.service" /etc/systemd/system/

install -d -m 755 "$ETC"
[ -f "$ETC/config.yaml" ] || { install -m 644 "$HERE/config.example.yaml" "$ETC/config.yaml"; log "created $ETC/config.yaml: set paperless.url"; }
# LoadCredential= needs the file to exist; an empty token means "not configured yet".
[ -f "$ETC/paperless-token" ] || install -m 600 /dev/null "$ETC/paperless-token"
chmod 600 "$ETC/paperless-token"

systemctl daemon-reload
systemctl enable kodak-oled.service
systemctl restart kodak-oled.service
# Settings page (kodak-web): password protected; the password is made on the first start.
systemctl enable kodak-web.service
systemctl restart kodak-web.service
if [ "${DRIVER:-vendor}" = sane ]; then
  # Our SANE backend (backend/ in the repository) + a station that is a plain SANE client.
  log "Building and installing the kodak_i2x00 SANE backend"
  apt-get install -y --no-install-recommends gcc make pkg-config libsane-dev libusb-1.0-0-dev >/dev/null
  make -C "$HERE/../../backend" >/dev/null
  make -C "$HERE/../../backend" check >/dev/null || die "backend self-test failed"
  make -C "$HERE/../../backend" install >/dev/null
  if [ -f "$ETC/firmware/powerup.json" ] && [ ! "$ETC/firmware/powerup.seq" -nt "$ETC/firmware/powerup.json" ]; then
    log "Converting the local power-up file for the backend"
    python3 "$HERE/../../backend/tools/powerup2seq.py" "$ETC/firmware/powerup.json" "$ETC/firmware/powerup.seq"
  fi
  [ -f "$ETC/firmware/powerup.seq" ] || log "No power-up file in $ETC/firmware: after a scanner power cycle the vendor driver will be used (see README)"
  systemctl disable --now kodak-native.service kodak-scand.service kodak-saned.service 2>/dev/null || true
  systemctl enable kodak-sane.service
  systemctl restart kodak-sane.service
  log "Done (SANE backend). Logs: journalctl -u kodak-sane -u kodak-oled -f"
elif [ "${DRIVER:-vendor}" = native ]; then
  # Native driver: Start button → scan. The vendor station stays installed as the fallback.
  systemctl disable --now kodak-sane.service kodak-scand.service kodak-saned.service 2>/dev/null || true
  systemctl enable kodak-native.service
  systemctl restart kodak-native.service
  log "Done (native driver). Logs: journalctl -u kodak-native -u kodak-oled -f"
else
  systemctl disable --now kodak-sane.service kodak-native.service 2>/dev/null || true
  systemctl enable kodak-saned.service kodak-scand.service
  systemctl restart kodak-saned.service kodak-scand.service
  log "Done (vendor driver). Logs: journalctl -u kodak-scand -u kodak-saned -u kodak-oled -f"
fi
log "Settings page: http://$(hostname -I | cut -d" " -f1):2600/  user admin, password: sudo cat $ETC/web-password"
[ -s "$ETC/paperless-token" ] || log "Paperless token not set yet: scans are kept in the spool until you add it (see README)"
