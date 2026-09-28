# Phase 1: Kodak vendor driver on the Raspberry Pi (Path A)

Goal: find out whether Kodak's **x86_64** Linux driver (v4.14) can drive the
i2600 from the **aarch64** Pi through emulation. The result decides whether we
get an interim working scan station before the native driver exists.

## How it works

```
Pi (aarch64, Raspberry Pi OS)
└─ /opt/kodak-x86   Debian bookworm amd64 chroot (isolated; delete it to undo)
     ├─ sane-utils (scanimage, saned)   ← from Debian amd64
     ├─ Kodak driver files              ← unpacked from the vendor .debs (no vendor installer, ADR-006)
     └─ runs through qemu-user (binfmt_misc), USB via the bind-mounted /dev/bus/usb
```

- Isolation: the Pi's own system only gains `debootstrap`, `qemu-user-static`/`binfmt-support`, `time`, and the `/usr/local/sbin/kodak-x86` helper.
- The vendor `.deb`s are **merged safely** (`tar --keep-directory-symlink --no-overwrite-dir`). A plain `dpkg-deb -x` would replace the `/lib → usr/lib` symlink on usrmerged systems. We hit this in the dry run (F-021).
- The chroot's SANE loads **only** the `kds_i2000` backend (fast start under emulation).

## Run it (on the Pi)

```bash
cd kodak_i2600/pi/phase1
sudo ./setup-x86-chroot.sh                 # ~10–30 min the first time (debootstrap under emulation)
sudo ./setup-x86-chroot.sh --diagnose      # detection tests, no scanning
sudo ./setup-x86-chroot.sh --diagnose --scan   # also scans ONE sheet: put one page in the feeder first
```

Each `--diagnose` writes `phase1-results-<timestamp>.tar.gz` next to the
script. **Send that file back.** It contains the command outputs, timings,
the Kodak logs from `/var/kodak`, and (with `--scan`) the scanned TIFF.
The tarball is git-ignored, so check what's inside before committing any of it.

Manual use afterwards:
```bash
sudo kodak-x86 scanimage -L
sudo kodak-x86 scanimage -A                     # all options, including buttons if exposed
sudo kodak-x86 umount                           # release the bind mounts
```

## If `sudo` says "unable to allocate pty"
An earlier version of the script unmounted the host's `/dev/pts` through mount propagation (fixed, F-023). **Reboot the Pi** to restore it, then `git pull`.

## Undo everything
```bash
sudo kodak-x86 umount
grep kodak-x86 /proc/mounts || sudo rm -rf --one-file-system /opt/kodak-x86
sudo rm /usr/local/sbin/kodak-x86
```
(`--one-file-system` together with the mount check makes sure nothing under
the bind-mounted `/dev` or `/sys` can be touched.)

## What the diagnostics test

| Step | Checks | Tells us |
|---|---|---|
| `01-chroot-uname` | `x86_64` inside the chroot | emulation works |
| `02-chroot-lsusb` | scanner visible from x86 userland | /dev + sysfs passthrough |
| `03-deviceprobe` | Kodak's own probe (`deviceprobe_i2000`, prints a device list ending in `@END@`) | libopenusb + libudev0 enumeration under emulation |
| `04-scanimage-L` | SANE lists `kds_i2000:…` | full chain SANE → TWAIN → kds.ds |
| `05-scanimage-A` | option dump | feature list, the SANE option names, whether buttons are exposed |
| `06-scan-gray300` | one page | real USB I/O (usbfs ioctls through qemu), image pipeline speed |

## Verified so far (dry run on x86_64, no scanner, 2026-09-28)
- Chroot builds; the Kodak files merge with `/lib` intact; the script is idempotent.
- SANE loads `kds_i2000` → `libtwaindsm` → `kds.ds`; device discovery spawns `deviceprobe_i2000` → `libopenusb` → `openusb_backend/linux.so`.
- With no device: `deviceprobe` prints `@END@`, and `scanimage -L` finds 0 devices (expected).
- **Not yet verified:** anything under qemu on aarch64, and any real USB I/O.

## If it fails / next options
- qemu too slow or USB ioctls unsupported → **box64** variant (a dynarec, much faster; needs the same x86 files, run from the host with box64).
- As a last resort for Path A: an x86 mini-PC running the vendor stack as `saned`, with the Pi using the SANE `net` backend.
- Architecture note for Phase 5: an x86 `saned` inside the chroot can serve SANE over localhost, so **native** aarch64 tools (scanbd, python-sane) use the `net` backend. That keeps "everything above the driver talks SANE" (ADR-001).
