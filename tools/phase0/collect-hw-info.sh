#!/usr/bin/env bash
# Phase 0: collect read-only USB/hardware information about the Kodak i2600.
# Sends NO commands to the scanner; only reads what the kernel already knows.
# Usage: sudo ./collect-hw-info.sh [outdir]   (sudo needed for full lsusb -v)
set -u
VID=040a PID=601d
OUT="${1:-i2600-hwinfo-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

run() { local f="$1"; shift; echo "\$ $*" > "$OUT/$f"; "$@" >> "$OUT/$f" 2>&1 || true; }

run uname.txt         uname -a
run lsusb.txt         lsusb
run lsusb-v.txt       lsusb -v -d "$VID:$PID"
run lsusb-t.txt       lsusb -t
command -v usb-devices >/dev/null && run usb-devices.txt usb-devices

# Raw sysfs view of the device (descriptors, speed, power, bound driver)
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  if [ "$(cat "$d/idVendor")" = "$VID" ] && [ "$(cat "$d/idProduct")" = "$PID" ]; then
    {
      echo "sysfs: $d"
      for a in manufacturer product serial version speed bMaxPower bcdDevice \
               bDeviceClass bNumConfigurations bNumInterfaces busnum devnum; do
        [ -f "$d/$a" ] && printf '%-20s %s\n' "$a" "$(cat "$d/$a")"
      done
      for i in "$d"/*:*; do
        [ -d "$i" ] || continue
        echo "--- interface $i"
        for a in bInterfaceClass bInterfaceSubClass bInterfaceProtocol bNumEndpoints; do
          [ -f "$i/$a" ] && printf '  %-20s %s\n' "$a" "$(cat "$i/$a")"
        done
        [ -e "$i/driver" ] && echo "  driver: $(basename "$(readlink "$i/driver")")"
        for e in "$i"/ep_*; do
          [ -d "$e" ] || continue
          printf '  %s type=%s dir=%s maxpkt=%s interval=%s\n' "$(basename "$e")" \
            "$(cat "$e/type")" "$(cat "$e/direction")" "$(cat "$e/wMaxPacketSize")" "$(cat "$e/interval")"
        done
      done
    } > "$OUT/sysfs.txt"
    xxd "$d/descriptors" > "$OUT/descriptors.hex" 2>/dev/null || od -An -tx1 "$d/descriptors" > "$OUT/descriptors.hex"
  fi
done

run dmesg-usb.txt sh -c "dmesg | grep -iE 'usb|kodak|040a' | tail -n 80"
command -v scanimage >/dev/null && run scanimage-L.txt scanimage -L

echo "Done. Output in: $OUT"
echo "Review for anything private (e.g. hostnames), then commit to docs/hardware/."
