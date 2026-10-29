**English** · [简体中文](../zh-CN/01-install.md)

# 01 · Installation (multi-distribution)

## 1. Prerequisites

Hardware: Petaic MOC-5159 (`VID:PID = 28e9:5159`). Confirm it is detected:

```bash
lsusb | grep -i 28e9
# Bus 003 Device 004: ID 28e9:5159 GDMicroelectronics ...
```

Inside a **VMware / VirtualBox VM** you must pass the USB device through to the guest first
(VMware: *VM Settings → USB Controller → connect the device*, or click the USB icon at the
bottom of the VM window). While passed through, Windows Hello on the host is unavailable.

Make sure fprintd is present:

```bash
# Debian / Ubuntu
sudo apt install fprintd libpam-fprintd zenity
# Fedora
sudo dnf install fprintd pam fprintd-pam zenity
# Arch
sudo pacman -S fprintd zenity
# openSUSE
sudo zypper install fprintd fprintd-pam zenity
```

> `zenity` is only needed by the desktop prompt scripts (`tools/fp-*.sh`). Pure CLI use does not need it.

## 2. One-shot installation

```bash
git clone https://github.com/<your-user>/moc5159-libfprint-driver.git
cd moc5159-libfprint-driver
sudo ./scripts/install.sh
```

No git? Download `moc5159-libfprint-driver-<version>.tar.gz` from the Releases page, unpack,
and run `sudo ./scripts/install.sh` the same way (that archive bundles the libfprint source,
so it installs **fully offline**).

The script performs these steps:

```
==> Environment check      detect distribution / architecture / device
==> Build dependencies     installed via your package manager (skip with --no-deps)
==> libfprint sources      unpack vendor/ if present, otherwise git clone upstream v1.94.100 into /var/tmp/moc5159-build
==> Register moc5159       add the driver to libfprint's build system
==> Build                  meson setup + ninja (1-3 min the first time)
==> Install to /usr/local  plus ld.so.conf.d so it takes priority
==> Restart the service
```

On success it prints the library path, where the install record lives, and the next commands to run.

### Common flags

| Flag | Purpose |
| --------------------- | ----------------------------------- |
| `--with-pam` | Enable `pam_fprintd` after install (login / lock screen / sudo) |
| `--system` | Install into `/usr`, overwriting the distribution libfprint (backed up, restorable) |
| `--drivers moc5159` | Build only this driver: smaller, fewer dependencies (breaks other fingerprint devices) |
| `--libfprint-src DIR` | Use an existing libfprint source tree (it is copied, your tree is not modified) |
| `--no-deps` | Do not install dependencies (offline environments / everything already present) |
| `--prefix DIR` | Custom install prefix (default `/usr/local`) |

Examples:

```bash
sudo ./scripts/install.sh --with-pam                  # recommended: driver + fingerprint login
sudo ./scripts/install.sh --system --drivers moc5159  # fully replace the distro version, only this driver
```

## 3. Post-install verification

### 3.1 Is the driver active?

```bash
sudo ./scripts/doctor.sh
```

Watch these lines in particular:

- `首选库来自本驱动（/usr/local/lib/.../libfprint-2.so.2）` — preferred library comes from this driver
- `fprintd 实际加载: /usr/local/lib/...` — what fprintd actually loaded
- `libfprint 的设备表里有 28e9:5159` — the device table contains 28e9:5159

Or check manually:

```bash
ldconfig -p | grep libfprint-2.so.2          # the first line should point to /usr/local
ls /usr/local/lib*/libfprint-2.so.2.0.0
```

### 3.2 Query the device (no finger needed)

```bash
sudo systemctl stop fprintd
sudo ./scripts/build-probe.sh
sudo ./build/probe db        # expect ev=0x27 (database created); 0x29 = empty header, needs rebuilding
sudo ./build/probe list      # lists templates already on the chip
sudo systemctl start fprintd
```

### 3.3 Enroll the first finger

```bash
sudo ./tools/fp-enroll.sh right-index-finger "Right index"
```

A desktop prompt appears: 10 seconds to get ready, then enrollment starts (4-8 presses).
**The prompt closes itself as soon as enrollment finishes.** The terminal prints
`登记结果: 成功` (enrollment succeeded) at the end.

### 3.4 Verify

```bash
./tools/fp-verify.sh right-index-finger "Right index"
# expected: Verify result: verify-match (done)
```

## 4. Manual build (without the scripts)

```bash
# Dependencies (Debian/Ubuntu)
sudo apt install build-essential meson ninja-build pkg-config python3 \
     libglib2.0-dev libgusb-dev libudev-dev libssl-dev libusb-1.0-0-dev

# 1) Unpack the bundled libfprint 1.94.100 (no network needed)
tar xzf vendor/libfprint-1.94.100.tar.gz -C /tmp
LFP=/tmp/libfprint-1.94.100
# Note: this tarball already carries the meson >= 1.10 compatibility patch.
#       If you clone upstream yourself you must edit tests/meson.build
#       (see 03-troubleshooting.md section B2).

# 2) Register the driver (idempotent, safe to re-run)
./scripts/integrate.sh "$LFP"

# 3) Configure + build
meson setup /tmp/lfp-build "$LFP" -Dprefix=/usr/local \
      -Dintrospection=false -Dudev_rules=disabled -Dudev_hwdb=disabled \
      -Ddoc=false -Dinstalled-tests=false
ninja -C /tmp/lfp-build

# 4) Confirm the device table
/tmp/lfp-build/libfprint/fprint-list-supported-devices | grep 5159
# 28e9:5159 | Petaic MOC-5159 (GD32W515) Touch Fingerprint Sensor

# 5) Install and make this driver take priority over the distribution copy
sudo ninja -C /tmp/lfp-build install
LIBDIR=$(dirname "$(find /usr/local -name 'libfprint-2.so.2.0.0' | head -1)")
echo "$LIBDIR" | sudo tee /etc/ld.so.conf.d/00-moc5159-libfprint.conf
sudo ldconfig
sudo systemctl restart fprintd
```

> When rolling your own setup, the install location does not matter — what matters is that
> **`ldconfig -p` lists its path before the distribution one**.

## 5. Upgrade / reinstall

Re-running `install.sh` from the same package is safe (it overwrites). For version upgrades:

```bash
sudo ./scripts/uninstall.sh        # uninstall first (leaves enrolled fingerprint data alone)
sudo ./scripts/install.sh --with-pam
```

Enrolled fingerprints live in `/var/lib/fprint/<user>/moc5159/` and **on the chip**, so
reinstalling the driver loses nothing. The only ways to lose them: wiping the on-chip
database (`probe raw d7`), or fprintd deciding the template is "unknown to device"
(a driver bug, already fixed — see "fingerprint files disappear" in
[03-troubleshooting.md](03-troubleshooting.md)).

## 6. Uninstall

```bash
sudo ./scripts/uninstall.sh           # remove the driver, restore the system
sudo ./scripts/uninstall.sh --purge   # also delete /var/tmp/moc5159-build
```

- `local` mode: removes the files under `/usr/local` plus `00-moc5159-libfprint.conf`,
  and the distribution libfprint takes over immediately;
- `--system` mode: restores the overwritten distribution files from `/var/lib/moc5159-driver/backup/`;
- **Enrolled fingerprint data is not deleted** (to clear it, run `sudo ./tools/fp-wipe.sh`
  or remove `/var/lib/fprint/` manually).

## 7. Offline install / building your own bundle

The repository **ships the libfprint source** as `vendor/libfprint-1.94.100.tar.gz`
(~12 MB), so a plain clone installs without any network access — `install.sh` unpacks it
automatically. Nothing extra to do.

If you deleted that archive (or cloned with `--filter=blob:none` and skipped it), fetch it
again before installing offline:

```bash
./scripts/fetch-libfprint.sh          # produces vendor/libfprint-1.94.100.tar.gz
sudo ./scripts/install.sh             # no network access from here on
```

Without the archive `install.sh` falls back to `git clone` upstream `v1.94.100`.

To hand the whole thing to a machine without internet:

```bash
# on a machine with internet (the source is already in vendor/, nothing to fetch)
cd .. && tar czf moc5159-libfprint-driver-1.0.0.tar.gz \
    --exclude='.git' --exclude='build' moc5159-libfprint-driver/
sha256sum moc5159-libfprint-driver-1.0.0.tar.gz > SHA256SUMS

# on the target machine
tar xzf moc5159-libfprint-driver-1.0.0.tar.gz
cd moc5159-libfprint-driver
sudo ./scripts/install.sh --no-deps     # you must install the dependencies yourself
```

## 8. Building native distribution packages (optional)

When producing a `.deb` / `.rpm`, do **not** package `libfprint-2.so.2` directly — it will
conflict with the distribution's own package. Two recommended routes:

1. **Build alongside the distribution's libfprint** (cleanest): take the distribution's
   libfprint source package, drop `src/*.c|h` into `libfprint/drivers/moc5159/`, add the
   `meson.build` registration (exactly what `scripts/integrate.sh` does), then package it
   through the normal distro workflow;
2. **Package driver + wrapper scripts only**: ship `src/`, `scripts/`, `tools/`, call
   `install.sh --system` from `postinst` and `uninstall.sh` from `prerm`.

---

## 9. Test record for this release

Environment: **Ubuntu 26.04.1 (kernel 7.0.0)**, VMware Workstation with USB passthrough,
device `Bus 003 Device 006: 28e9:5159`, stock `fprintd 1.94.5` and
`libfprint-2-2 1.95.1` (distribution build, untouched).

| Test | Result |
| ------------------------------- | ------------------------------------------------------------------------------- |
| `install.sh` default local mode | ✅ installs 12 files into `/usr/local/lib/x86_64-linux-gnu`; `ldconfig` prefers that path |
| Library fprintd actually loads | ✅ `/usr/local/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0` (confirmed via `/proc/<pid>/maps`) |
| Coexistence with distro 1.95.1 | ✅ distribution files untouched; fprintd 1.94.5 loads 1.94.100 fine; `fprintd-list` sees the device and enrolled fingers |
| Full driver build | ✅ 281 devices (including 5159); other fingerprint sensors keep working |
| `uninstall.sh` | ✅ `/usr/local` cleaned, `ldconfig` falls back to `/usr`, enrolled data preserved |
| `install.sh --system` | ✅ backs up distro `libfprint-2.so.2.0.0` (821 KB) → overwrites (3.6 MB) → `apt-mark hold` |
| `uninstall.sh` (system mode) | ✅ restores the original distro file from backup, releases the hold |
| `build-probe.sh` + `probe list` | ✅ reads on-chip templates: `count 1 / [0] tid=1 counter=1` |
| `doctor.sh` | ✅ all 8 checks pass |
| Reinstall / overwrite install | ✅ idempotent (re-runnable in the same mode; switching modes requires uninstalling first) |

**Not tested**: real hardware on other distributions, graphical prompts on the GDM login
screen, multiple fingers (firmware limitation — see [02-usage.md](02-usage.md)).
