#!/usr/bin/env bash
# Phase 1b: add box64 (x86_64 dynarec for ARM64) to the Kodak x86 chroot as a
# faster alternative to qemu-user (ADR-009).
#
# Debian's own box64 needs glibc >= 2.39, but the Kodak chroot is bookworm
# (2.36). So box64 is built from upstream source in a separate, disposable
# arm64 bookworm build chroot ($BUILD), then copied into the Kodak chroot
# ($ROOT), which gets the arm64 runtime libs via multiarch. The qemu path is
# untouched: `kodak-x86 scanimage …` still uses qemu, `kodak-x86 box64 scanimage …`
# uses box64.
#
# Usage (as root, on the Pi, after setup-x86-chroot.sh):
#   sudo ./setup-box64.sh            # build (~20-40 min on a Pi 4) + install
#   sudo ./setup-box64.sh --clean    # delete the build chroot afterwards
set -euo pipefail

ROOT="${ROOT:-/opt/kodak-x86}"
BUILD="${BUILD:-/opt/box64-build}"
SUITE="${SUITE:-bookworm}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
BOX64_REPO="https://github.com/ptitSeb/box64"
BOX64_TAG="${BOX64_TAG:-v0.4.4}"
JOBS="${JOBS:-3}"   # -j4 can run a 2 GB Pi out of memory
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log()  { printf '\033[1;34m[box64]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[box64] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
[ "$(uname -m)" = "aarch64" ] || die "box64 needs an aarch64 host"
[ "$(getconf PAGESIZE)" = "4096" ] || die "box64 needs 4K pages (Pi 5 kernels use 16K: boot kernel8.img)"
[ -x "$ROOT/usr/bin/apt-get" ] || die "$ROOT missing: run setup-x86-chroot.sh first"
command -v kodak-x86 >/dev/null || die "kodak-x86 helper missing: run setup-x86-chroot.sh first"

# Only /proc is mounted into the build chroot (plain proc mount, no rbind, so
# no propagation to the host; see F-023).
build_run() {
  mountpoint -q "$BUILD/proc" || mount -t proc proc "$BUILD/proc"
  chroot "$BUILD" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root "$@"
}
build_umount() { mountpoint -q "$BUILD/proc" && umount "$BUILD/proc" || true; }

build_box64() {
  if [ -x "$BUILD/usr/bin/apt-get" ]; then
    log "Build chroot $BUILD exists, reusing it"
  else
    log "Creating native arm64 $SUITE build chroot in $BUILD"
    debootstrap --arch=arm64 --variant=minbase "$SUITE" "$BUILD" "$MIRROR"
  fi
  cp /etc/resolv.conf "$BUILD/etc/resolv.conf"
  trap build_umount EXIT
  build_run apt-get update -qq
  build_run env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake git python3 ca-certificates
  if [ ! -d "$BUILD/src/box64/.git" ]; then
    build_run git clone --depth 1 --branch "$BOX64_TAG" "$BOX64_REPO" /src/box64
  fi
  # Local fix: dlopen(NULL) must return a real link_map; Kodak's
  # COsCfg::GetModuleName() walks ->l_next of it and crashed on box64's
  # index handle (F-032).
  cp "$HERE/box64-dlopen-null-linkmap.patch" "$BUILD/src/"
  if build_run git -C /src/box64 apply --check /src/box64-dlopen-null-linkmap.patch 2>/dev/null; then
    build_run git -C /src/box64 apply /src/box64-dlopen-null-linkmap.patch
    log "Applied box64-dlopen-null-linkmap.patch"
  fi
  log "Building box64 $BOX64_TAG with -j$JOBS"
  build_run sh -c "cd /src/box64 && mkdir -p build && cd build && \
    cmake .. -DRPI4ARM64=1 -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -j$JOBS"
  [ -x "$BUILD/src/box64/build/box64" ] || die "build produced no box64 binary"
}

install_box64() {
  log "Adding arm64 runtime libs to the Kodak chroot (multiarch)"
  kodak-x86 dpkg --add-architecture arm64
  kodak-x86 apt-get update -qq
  # libc/libgcc/libstdc++ for box64 itself; the rest lets box64 use native
  # ARM builds of common libraries instead of emulating the x86 ones.
  kodak-x86 env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libc6:arm64 libgcc-s1:arm64 libstdc++6:arm64 zlib1g:arm64 libpng16-16:arm64
  install -m 755 "$BUILD/src/box64/build/box64" "$ROOT/usr/local/bin/box64"
  install -m 644 "$BUILD/src/box64/system/box64.box64rc" "$ROOT/etc/box64.box64rc"
  log "Installed: $(kodak-x86 /usr/local/bin/box64 --version 2>&1 | head -n1)"
  kodak-x86 umount
}

case "${1:-}" in
  --clean) build_umount; rm -rf --one-file-system "$BUILD"; log "removed $BUILD" ;;
  "")      build_box64; install_box64
           log "Done. Try: sudo kodak-x86 box64 scanimage -L" ;;
  *)       die "unknown argument: $1" ;;
esac
