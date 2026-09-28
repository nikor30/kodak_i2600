#!/usr/bin/env bash
# Phase 1 (Path A): run the Kodak i2000 x86_64 vendor driver on a Raspberry Pi.
#
# Builds an isolated Debian amd64 chroot under $ROOT, installs sane-utils plus
# the Kodak driver into it WITHOUT running Kodak's installer (ADR-006), and
# runs it through qemu-user binfmt emulation. The Pi's own system is only
# touched by: apt packages (qemu-user-static/binfmt-support/debootstrap) and
# the $ROOT directory. Remove with: kodak-x86 umount && rm -rf --one-file-system $ROOT
#
# Usage (as root, on the Pi):
#   sudo ./setup-x86-chroot.sh               # build/refresh the chroot
#   sudo ./setup-x86-chroot.sh --diagnose    # run tests, write a results tarball
#   sudo ./setup-x86-chroot.sh --diagnose --scan   # also scan ONE page from the ADF
#
# Env overrides: ROOT (default /opt/kodak-x86), SUITE (default bookworm),
#   MIRROR, DRIVER_TGZ (path to an already downloaded driver tarball),
#   ALLOW_NON_ARM=1 (dry-run on an x86_64 host; no emulation needed there).
set -euo pipefail

ROOT="${ROOT:-/opt/kodak-x86}"
SUITE="${SUITE:-bookworm}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
DRIVER_URL="https://resources.kodakalaris.com/docimaging/drivers/LinuxSoftware_i2000_v4.14.x86_64.deb.tar.gz"
DRIVER_TGZ="${DRIVER_TGZ:-}"
VIDPID="040a:601d"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log()  { printf '\033[1;34m[phase1]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[phase1] WARN:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[phase1] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
ARCH="$(uname -m)"
if [ "$ARCH" != "aarch64" ] && [ "${ALLOW_NON_ARM:-0}" != "1" ]; then
  die "expected aarch64 (Raspberry Pi 64-bit OS), got $ARCH. Set ALLOW_NON_ARM=1 for a dry run."
fi

# ---------------------------------------------------------------- mounts ----
# The bind mounts MUST be rslave. With systemd, / is mounted "shared", so a
# plain --rbind makes the chroot's dev/sys peers of the host's: `umount -R`
# on them then propagates and unmounts the HOST's /dev/pts, /sys/fs/cgroup, …
# (first Pi run: "sudo: unable to allocate pty"; F-023).
is_mounted() { mountpoint -q "$ROOT/$1"; }
do_mount() {
  mkdir -p "$ROOT"/{proc,sys,dev,run/udev}
  is_mounted proc     || mount -t proc proc "$ROOT/proc"
  is_mounted sys      || { mount --rbind /sys "$ROOT/sys" && mount --make-rslave "$ROOT/sys"; }
  is_mounted dev      || { mount --rbind /dev "$ROOT/dev" && mount --make-rslave "$ROOT/dev"; }
  if [ -d /run/udev ]; then
    is_mounted run/udev || { mount --bind /run/udev "$ROOT/run/udev" && mount --make-rslave "$ROOT/run/udev"; }
  fi
}
do_umount() {
  local m
  for m in run/udev dev sys proc; do
    if [ -d "$ROOT/$m" ] && is_mounted "$m"; then
      # Cut propagation first (this also protects mounts made by older versions).
      mount --make-rslave "$ROOT/$m" || { echo "refusing to unmount $ROOT/$m: cannot make it rslave" >&2; return 1; }
      umount -R "$ROOT/$m" || umount -lR "$ROOT/$m"
    fi
  done
}
in_root() { do_mount; chroot "$ROOT" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root "$@"; }

# ------------------------------------------------------------- host prep ----
host_packages() {
  log "Installing host packages (debootstrap, qemu user emulation, binfmt)"
  apt-get update -qq
  apt-get install -y --no-install-recommends debootstrap debian-archive-keyring ca-certificates curl time usbutils
  if [ "$ARCH" = "aarch64" ]; then
    apt-get install -y --no-install-recommends qemu-user-static binfmt-support \
      || apt-get install -y --no-install-recommends qemu-user qemu-user-binfmt
    [ -e /proc/sys/fs/binfmt_misc/qemu-x86_64 ] || update-binfmts --enable qemu-x86_64 || true
    [ -e /proc/sys/fs/binfmt_misc/qemu-x86_64 ] || die "binfmt for x86_64 not registered (see /proc/sys/fs/binfmt_misc)"
    grep -q '^flags:.*F' /proc/sys/fs/binfmt_misc/qemu-x86_64 \
      || warn "qemu-x86_64 binfmt lacks the F flag; the chroot may not find the interpreter"
  fi
}

# ---------------------------------------------------------------- chroot ----
build_chroot() {
  if [ -x "$ROOT/usr/bin/apt-get" ]; then
    log "Chroot $ROOT already exists, reusing it"
  else
    log "Creating Debian $SUITE amd64 chroot in $ROOT (this takes a while under emulation)"
    debootstrap --arch=amd64 --variant=minbase "$SUITE" "$ROOT" "$MIRROR"
  fi
  cp /etc/resolv.conf "$ROOT/etc/resolv.conf" 2>/dev/null || true
  log "Installing SANE tools inside the chroot"
  in_root apt-get update -qq
  in_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    sane-utils libsane1 usbutils strace file procps xdg-user-dirs \
    libpango-1.0-0 libpangocairo-1.0-0 libcairo2 libglib2.0-0
}

# Every Kodak library must resolve inside the chroot. device.so dlopen()s
# devicemanager.so (pango/cairo); a missing dep only shows up at open time as
# "open of device kds_i2000:i2000 failed: Invalid argument" (first Pi run, F-022).
check_deps() {
  log "Checking shared-library dependencies of the Kodak files"
  local missing
  # libuic_*.so report libpthread.so.0 "not found" under ldd although it loads
  # fine via hippo.so; that ldd quirk is filtered out.
  missing="$(in_root sh -c 'for f in /opt/kodak/kds_i2000/lib/*.so /usr/local/lib/twain/kodak/kds_i2000/kds.ds \
      /usr/lib/sane/libsane-kds_i2000.so.1.0.24 /usr/local/lib/libopenusb.so /usr/local/lib/openusb_backend/linux.so; do
      LD_LIBRARY_PATH=/opt/kodak/kds_i2000/lib ldd "$f" 2>/dev/null | grep "not found" | grep -v libpthread.so.0 | sed "s|^|$f: |"; done' || true)"
  if [ -n "$missing" ]; then
    warn "missing libraries:"; printf '%s\n' "$missing" >&2
    return 1
  fi
  log "  all Kodak libraries resolve"
}

# ---------------------------------------------------------- kodak driver ----
# Unpacks each .deb into a staging dir and merges it into the chroot with
# tar --keep-directory-symlink --no-overwrite-dir. A plain `dpkg-deb -x` onto
# a usrmerged root replaces the /lib -> usr/lib symlink with a directory and
# resets directory metadata. We saw this happen in a test (see JOURNAL 2026-09-28).
install_kodak() {
  local tgz="$DRIVER_TGZ" work
  work="$(mktemp -d)"
  if [ -z "$tgz" ]; then
    tgz="$work/driver.tgz"
    log "Downloading Kodak driver v4.14 (x86_64)"
    curl -fL --retry 3 -o "$tgz" "$DRIVER_URL"
  fi
  log "Unpacking Kodak driver"
  mkdir -p "$work/tar"
  tar -xzf "$tgz" -C "$work/tar"
  local d stage
  for d in kodak_i2000-04.14.00.00-1.amd64.deb libopenusb_1.1.13-0_amd64.deb \
           libudev0_175-0ubuntu19_amd64.deb twaindsm.deb; do
    [ -f "$work/tar/$d" ] || die "missing $d in driver tarball"
    stage="$work/stage-${d%%_*}"; mkdir -p "$stage"
    dpkg-deb -x "$work/tar/$d" "$stage"
    tar -C "$stage" -cf - . | tar -C "$ROOT" -xf - --keep-directory-symlink --no-overwrite-dir
    log "  merged $d"
  done
  rm -rf "$work"

  log "Configuring the chroot (replaces Kodak's postinst)"
  mkdir -p "$ROOT/var/kodak/common" "$ROOT/var/kodak/kds_i2000/sanetwain"
  ln -sf /usr/lib/sane/libsane-kds_i2000.so.1.0.24 "$ROOT/usr/lib/x86_64-linux-gnu/sane/libsane-kds_i2000.so.1"
  # Only the Kodak backend: probing ~80 other backends is slow under emulation.
  [ -f "$ROOT/etc/sane.d/dll.conf.orig" ] || cp "$ROOT/etc/sane.d/dll.conf" "$ROOT/etc/sane.d/dll.conf.orig"
  echo kds_i2000 > "$ROOT/etc/sane.d/dll.conf"
  rm -f "$ROOT"/etc/sane.d/dll.d/*
  in_root ldconfig
}

install_wrapper() {
  log "Installing helper /usr/local/sbin/kodak-x86"
  cat > /usr/local/sbin/kodak-x86 <<EOF
#!/usr/bin/env bash
# Run a command inside the Kodak x86 chroot ($ROOT). "kodak-x86 umount" releases the mounts.
set -euo pipefail
ROOT="$ROOT"
$(declare -f is_mounted do_mount do_umount)
if [ "\${1:-}" = "umount" ]; then do_umount; exit 0; fi
do_mount
exec chroot "\$ROOT" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root \\
  SANETWAIN_LOG="\${SANETWAIN_LOG:-}" SANE_DEBUG_DLL="\${SANE_DEBUG_DLL:-}" "\$@"
EOF
  chmod 755 /usr/local/sbin/kodak-x86
}

# ------------------------------------------------------------- diagnose -----
diagnose() {
  local do_scan="$1" out ts
  ts="$(date +%Y%m%d-%H%M%S)"
  out="$HERE/phase1-results-$ts"
  mkdir -p "$out"
  log "Diagnostics → $out"
  step() { local name="$1"; shift; log "  $name"; { echo "\$ $*"; /usr/bin/time -v timeout "${T:-300}" "$@"; echo "exit=$?"; } >"$out/$name.txt" 2>&1 || true; }

  { uname -a; echo; cat /etc/os-release; echo; nproc; free -m; } >"$out/host.txt" 2>&1
  ls -l /proc/sys/fs/binfmt_misc/ >"$out/binfmt.txt" 2>&1 || true
  cat /proc/sys/fs/binfmt_misc/qemu-x86_64 >>"$out/binfmt.txt" 2>&1 || true
  lsusb -d "$VIDPID" >"$out/host-lsusb.txt" 2>&1 || true

  check_deps >"$out/00-deps.txt" 2>&1 || true
  step 01-chroot-uname      "$0" --run uname -m
  step 02-chroot-lsusb      "$0" --run lsusb -d "$VIDPID"
  step 03-deviceprobe       "$0" --run /usr/local/bin/deviceprobe_i2000
  T=600 step 04-scanimage-L "$0" --run env SANE_DEBUG_DLL=3 scanimage -L
  T=600 step 05-scanimage-A "$0" --run scanimage -A
  if [ "$do_scan" = "1" ]; then
    log "  06: scanning ONE page from the ADF (put one sheet in the feeder)"
    T=900 step 06-scan-gray300 "$0" --run sh -c \
      'scanimage --mode Gray --resolution 300 --batch-count=1 --format=tiff > /tmp/scan-gray300.tiff; ls -l /tmp/scan-gray300.tiff; file /tmp/scan-gray300.tiff'
    cp "$ROOT/tmp/scan-gray300.tiff" "$out/" 2>/dev/null || true
  fi
  cp -r "$ROOT/var/kodak" "$out/var-kodak" 2>/dev/null || true
  dmesg 2>/dev/null | tail -n 60 >"$out/dmesg-tail.txt" || true
  tar -C "$HERE" -czf "$out.tar.gz" "$(basename "$out")"
  log "Done. Please send: $out.tar.gz"
}

# ------------------------------------------------------------------ main ----
case "${1:-}" in
  --run)      shift; in_root "$@"; exit $? ;;
  --umount)   do_umount; exit 0 ;;
  --diagnose) diagnose "$([ "${2:-}" = "--scan" ] && echo 1 || echo 0)"; exit 0 ;;
  ""|--install)
    host_packages
    build_chroot
    install_kodak
    check_deps || die "fix the missing libraries above, then re-run"
    install_wrapper
    log "Setup complete. Next: sudo $0 --diagnose   (add --scan to scan one page)"
    ;;
  *) die "unknown argument: $1" ;;
esac
