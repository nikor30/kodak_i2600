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
#   sudo ./setup-x86-chroot.sh --snapshot     # 2nd terminal, while something hangs
#   sudo ./setup-x86-chroot.sh --trace-open   # qemu syscall trace + usbmon of one open
#   sudo EMU=box64 ./setup-x86-chroot.sh --diagnose [--scan]   # same tests under box64 (setup-box64.sh)
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
# Under qemu-user, x86 code reads the host's ARM /proc/cpuinfo. Kodak's
# hippo.so (image processing helper, run by lexexe) spins forever after
# parsing it: it looks for x86 fields like "cpu MHz" / "physical id" (F-026).
# Give the chroot an x86-style cpuinfo, bind-mounted over the chroot's own
# /proc/cpuinfo only; the host's /proc is untouched.
fake_cpuinfo() {
  local f="$ROOT/etc/kodak-x86-cpuinfo" n i
  n="$(nproc 2>/dev/null || echo 4)"
  : >"$f"
  for ((i = 0; i < n; i++)); do
    cat >>"$f" <<EOF
processor	: $i
vendor_id	: GenuineIntel
cpu family	: 6
model		: 85
model name	: Intel(R) Xeon(R) CPU (kodak-x86 chroot, emulated)
stepping	: 7
cpu MHz		: 1800.000
cache size	: 1024 KB
physical id	: 0
siblings	: $n
core id		: $i
cpu cores	: $n
apicid		: $i
fpu		: yes
fpu_exception	: yes
cpuid level	: 13
wp		: yes
flags		: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ht syscall nx lm constant_tsc nopl pni ssse3 cx16 sse4_1 sse4_2 popcnt lahf_lm
bogomips	: 3600.00
clflush size	: 64
cache_alignment	: 64
address sizes	: 40 bits physical, 48 bits virtual

EOF
  done
  echo "$f"
}
cpuinfo_bound() { grep -q " $ROOT/proc/cpuinfo " /proc/self/mountinfo; }
do_mount() {
  mkdir -p "$ROOT"/{proc,sys,dev,run/udev}
  is_mounted proc     || mount -t proc proc "$ROOT/proc"
  cpuinfo_bound       || mount --bind "$(fake_cpuinfo)" "$ROOT/proc/cpuinfo"
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
$(declare -f is_mounted fake_cpuinfo cpuinfo_bound do_mount do_umount)
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
  out="$HERE/phase1-results-${EMU:-qemu}-$ts"
  mkdir -p "$out"
  # EMU=box64 runs the x86 programs under box64 instead of qemu (ADR-009).
  local X=()
  if [ "${EMU:-qemu}" = "box64" ]; then
    [ -x "$ROOT/usr/local/bin/box64" ] || die "box64 not installed: run setup-box64.sh"
    X=(/usr/local/bin/box64)
  fi
  log "Diagnostics (${EMU:-qemu}) → $out"
  step() { local name="$1"; shift; log "  $name"; { echo "\$ $*"; /usr/bin/time -v timeout "${T:-300}" "$@"; echo "exit=$?"; } >"$out/$name.txt" 2>&1 || true; }

  { uname -a; echo; cat /etc/os-release; echo; nproc; free -m; } >"$out/host.txt" 2>&1
  ls -l /proc/sys/fs/binfmt_misc/ >"$out/binfmt.txt" 2>&1 || true
  cat /proc/sys/fs/binfmt_misc/qemu-x86_64 >>"$out/binfmt.txt" 2>&1 || true
  lsusb -d "$VIDPID" >"$out/host-lsusb.txt" 2>&1 || true

  check_deps >"$out/00-deps.txt" 2>&1 || true
  step 01-chroot-uname      "$0" --run uname -m
  step 02-chroot-lsusb      "$0" --run lsusb -d "$VIDPID"
  step 03-deviceprobe       "$0" --run "${X[@]}" /usr/local/bin/deviceprobe_i2000
  T=600 step 04-scanimage-L "$0" --run env SANE_DEBUG_DLL=3 "${X[@]}" /usr/bin/scanimage -L
  T=180 step 05-scanimage-A "$0" --run "${X[@]}" /usr/bin/scanimage -A
  if [ "$do_scan" = "1" ]; then
    log "  06: scanning ONE sheet, both sides, from the ADF (put one sheet in the feeder)"
    rm -f "$ROOT"/tmp/scan-gray300-*.tiff
    # --batch writes one file per image (a duplex sheet gives two); scanimage
    # ignores stdout in batch mode, so the file pattern must be explicit.
    T=900 step 06-scan-gray300 "$0" --run sh -c \
      "${X[*]} /usr/bin/scanimage --mode Gray --resolution 300 --duplex both --format=tiff --batch=/tmp/scan-gray300-%d.tiff; ls -l /tmp/scan-gray300-*.tiff; file /tmp/scan-gray300-*.tiff"
    cp "$ROOT"/tmp/scan-gray300-*.tiff "$out/" 2>/dev/null || true
  fi
  cp -r "$ROOT/var/kodak" "$out/var-kodak" 2>/dev/null || true
  dmesg 2>/dev/null | tail -n 60 >"$out/dmesg-tail.txt" || true
  tar -C "$HERE" -czf "$out.tar.gz" "$(basename "$out")"
  log "Done. Please send: $out.tar.gz"
}

# Kodak logs + USB/kernel state, shared by --snapshot and --trace-open.
collect_state() {
  local out="$1"
  lsusb >"$out/lsusb.txt" 2>&1 || true
  dmesg 2>/dev/null | tail -n 80 >"$out/dmesg-tail.txt" || true
  cp -r "$ROOT/var/kodak" "$out/var-kodak" 2>/dev/null || true
}

# --snapshot: run in a SECOND terminal while scanimage hangs. Read-only.
snapshot() {
  local out p t
  out="$HERE/phase1-snapshot-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
  log "Snapshot → $out"
  ps -eLo pid,tid,stat,wchan:32,etime,args | grep -E 'PID|scanimage|deviceprobe|pnphelper' | grep -v grep >"$out/ps-threads.txt" || true
  for p in $(pgrep -f 'scanimage|deviceprobe' || true); do
    {
      echo "=== pid $p: $(tr '\0' ' ' </proc/"$p"/cmdline)"
      grep -E 'State|Threads|VmRSS' /proc/"$p"/status
      echo "--- kernel stack"; cat /proc/"$p"/stack 2>/dev/null
      echo "--- open fds"; ls -l /proc/"$p"/fd 2>/dev/null
      for t in /proc/"$p"/task/*; do
        echo "--- thread ${t##*/} wchan=$(cat "$t"/wchan 2>/dev/null) syscall=$(cat "$t"/syscall 2>/dev/null)"
      done
    } >>"$out/processes.txt" 2>&1
  done
  collect_state "$out"
  tar -C "$HERE" -czf "$out.tar.gz" "$(basename "$out")"
  log "Done. Please send: $out.tar.gz"
}

# --trace-open: open the scanner once (scanimage -A, max 3 min) while
# recording (a) qemu's syscall trace of the emulated driver and (b) the raw
# USB traffic of the scanner's bus via usbmon. Sends nothing extra itself.
trace_open() {
  local out bus mpid=""
  out="$HERE/phase1-trace-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
  log "Trace → $out (takes up to 3 minutes)"
  bus="$(lsusb -d "$VIDPID" 2>/dev/null | awk '{print $2+0; exit}' || true)"
  if [ -n "$bus" ]; then
    modprobe usbmon 2>/dev/null || true
    mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
    if [ -r "/sys/kernel/debug/usb/usbmon/${bus}u" ]; then
      cat "/sys/kernel/debug/usb/usbmon/${bus}u" >"$out/usbmon-bus$bus.txt" & mpid=$!
      log "  usbmon capturing bus $bus"
    else
      warn "usbmon not available; continuing without USB capture"
    fi
  else
    warn "scanner $VIDPID not found on the host"
  fi
  dmesg 2>/dev/null >"$out/dmesg-before.txt" || true
  { echo "start $(date +%T.%N)"
    timeout 180 "$0" --run env QEMU_STRACE=1 scanimage -A >"$out/scanimage-A.txt" 2>"$out/qemu-strace.txt"
    echo "exit=$? end $(date +%T.%N)"; } >"$out/run.txt" 2>&1 || true
  [ -n "$mpid" ] && kill "$mpid" 2>/dev/null
  dmesg 2>/dev/null >"$out/dmesg-after.txt" || true
  diff "$out/dmesg-before.txt" "$out/dmesg-after.txt" >"$out/dmesg-new.txt" || true
  collect_state "$out"
  tar -C "$HERE" -czf "$out.tar.gz" "$(basename "$out")"
  log "Done. Please send: $out.tar.gz"
}

# ------------------------------------------------------------------ main ----
case "${1:-}" in
  --run)      shift; in_root "$@"; exit $? ;;
  --umount)   do_umount; exit 0 ;;
  --diagnose) diagnose "$([ "${2:-}" = "--scan" ] && echo 1 || echo 0)"; exit 0 ;;
  --snapshot) snapshot; exit 0 ;;
  --trace-open) trace_open; exit 0 ;;
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
