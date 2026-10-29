**English** · [简体中文](README.zh-CN.md)

# moc5159-libfprint-driver

A libfprint driver for the **Petaic MOC-5159** fingerprint module (`28e9:5159`).  
Once installed, the whole `fprintd` stack works: the CLI, the GNOME Settings  
panel, and `pam_fprintd` (login / lock screen / passwordless `sudo`).

This module ships with Windows Hello on some laptops and mini-PCs and has  
**no upstream Linux support**. The driver speaks the module's private 128-byte  
USB protocol, reverse-engineered from the signed Windows driver package.  
Tested on Ubuntu 26.04 with fprintd 1.94.5.

> **Firmware limitation: only the first template in the on-chip database is ever  
> compared — enroll exactly one finger.** See [one finger only](#one-finger-limit).

## What this device is

| Item                 | Value                                                                                                                           |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| USB ID               | `28e9:5159`                                                                                                                     |
| `lsusb` line         | `ID 28e9:5159 GDMicroelectronics Touch Fingerprint Sensor`                                                                      |
| Module               | MOC-5159 (Petaic)                                                                                                               |
| Chips                | GD32W515 MCU + Silead GSL6150 sensor                                                                                            |
| Operating mode       | **match-on-chip** — templates live in the chip's flash and matching happens on-chip; fingerprint images never reach host memory |
| Original environment | Windows Hello module (Windows driver package `GD-MOC-release-signed-4.0.105.5`)                                                 |

After installing the driver the device shows up as  
`Petaic MOC-5159 (GD32W515) Touch Fingerprint Sensor`. The vendor name  
`GDMicroelectronics` in the `lsusb` line comes from the USB ID database, not  
from this driver.

<a id="one-finger-limit"></a>

## ⚠️ Read this first: one finger only

**The firmware compares against record #0 of the template database, and nothing else.**

- 1 template in the database → works, stable matching;
- 2 or more templates → only the **oldest** one matches; later enrollments never will.

This is firmware behaviour, not a driver bug. We captured USB traffic from the  
official Windows driver, brute-forced the command space, and loaded candidate  
templates individually — all three approaches agree. The official Windows driver  
never used multiple fingers on this device either.

To switch fingers, wipe first:

```bash
sudo ./tools/fp-wipe.sh                          # clear on-chip DB + local fprintd storage
sudo ./tools/fp-enroll.sh left-thumb Left thumb   # enroll the finger you actually want
```

## Features

- `fprintd-enroll` / `fprintd-verify` / `fprintd-list` / `fprintd-delete`;
- GNOME *Settings → Users → Fingerprint* enrollment and removal;
- `pam_fprintd` for login, lock screen and passwordless `sudo`  
  (falls back to password after 5 failed attempts);
- Templates are stored **on the chip** plus under `/var/lib/fprint/<user>/moc5159/`,  
  so reinstalling the driver loses nothing;
- A standalone diagnostic tool `moc5159-probe` (libusb only, bypasses fprintd)  
  that tells "driver problem" apart from "device problem".

## Quick start

```bash
git clone https://github.com/<your-user>/moc5159-libfprint-driver.git
cd moc5159-libfprint-driver

sudo ./scripts/install.sh                        # build + install, 1-3 min the first time
sudo ./scripts/doctor.sh                         # 8-point self-check; run this when in doubt
sudo ./tools/fp-enroll.sh right-index-finger Right index   # enroll (desktop shows a press prompt)

fprintd-verify                                    # verify
```

Then lock the screen (`Super+L`) and unlock it with the finger you just enrolled.

**Let fingerprints handle login / lock screen / sudo too:**

```bash
sudo ./scripts/install.sh --with-pam
```

On Debian/Ubuntu this goes through `pam-auth-update`; on other distributions the  
script prints the manual steps. PAM is configured with `max-tries=5`, so five  
consecutive misses are needed before the password prompt appears — a single  
misread will not force you to type it (see [docs/en/02-usage.md](docs/en/02-usage.md) §5.2).

### What the installer does

1. Installs build dependencies for your distribution (apt / dnf / yum / zypper / pacman / apk);
2. Extracts the **libfprint 1.94.100** source tree that ships in `vendor/`  
   (12 MB archive, so the install works fully offline; if you delete it, the script  
   falls back to `git clone` upstream);
3. Registers the driver with libfprint's build system (libfprint has no external  
   plugin mechanism — drivers must be compiled into the library);
4. **Builds all libfprint drivers by default (281 devices)** so you do not break any  
   other fingerprint reader on the machine; falls back to `moc5159` only if  
   openssl and friends are missing;
5. Installs into `/usr/local` and drops an `ld.so.conf.d` entry so the dynamic  
   linker prefers it — **no file from your distribution's `libfprint-2-2` package is  
   touched**, so system upgrades cannot revert it;
6. Restarts `fprintd`, optionally enables PAM.

### Two install modes

|                              | `local` (default)                                      | `--system`                                                         |
| ---------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------ |
| Installs to                  | `/usr/local`                                           | `/usr`                                                             |
| Distribution libfprint files | **untouched**                                          | overwritten (backed up to `/var/lib/moc5159-driver/backup/` first) |
| Takes effect via             | `ld.so.conf.d/00-moc5159-libfprint.conf`               | direct replacement                                                 |
| Uninstall                    | delete files, distribution version resumes immediately | restore from backup                                                |
| Best for                     | **everyone**                                           | full replacement / integration testing                             |

Both modes uninstall cleanly with `sudo ./scripts/uninstall.sh`  
(add `--purge` to drop the build directory too). Enrolled fingerprint data is never deleted.

## Compatibility

| Item             | Requirement                                                                                                    |
| ---------------- | -------------------------------------------------------------------------------------------------------------- |
| Architecture     | x86_64 / aarch64 (plain C, no assembly)                                                                        |
| glibc            | ≥ 2.28                                                                                                         |
| GLib             | ≥ 2.56                                                                                                         |
| libgusb          | ≥ 0.2                                                                                                          |
| meson / ninja    | meson ≥ 0.62                                                                                                   |
| **Tested**       | **Ubuntu 26.04.1** (kernel 7.0.0) + fprintd 1.94.5 + `pam_fprintd`, VMware USB passthrough                     |
| Expected to work | Debian 12+, Ubuntu 22.04+, Fedora 39+, Arch, openSUSE Tumbleweed                                               |
| Untested         | other distributions — the scripts handle several package managers; run `scripts/doctor.sh` if something breaks |

The driver targets the libfprint **1.94 driver API**. 1.95+ changed parts of it,  
which is why the installer pins 1.94.100.

> Inside a **virtual machine** you must pass the USB device through to the guest  
> first (VMware: *VM → Removable Devices → Connect*). While passed through,  
> Windows Hello on the host stops working.

## Common commands

```bash
# Status
sudo ./scripts/doctor.sh                # environment self-check
./tools/fp-list.sh                      # enrolled fingers (use sudo to also read on-chip records)

# Enroll / verify / delete
sudo ./tools/fp-enroll.sh right-index-finger Right index
./tools/fp-verify.sh right-index-finger
sudo fprintd-delete $USER -f right-index-finger

# Wipe (destructive, asks for "yes")
sudo ./tools/fp-wipe.sh

# Standalone diagnostics: talk to the chip directly, no fprintd involved
sudo ./scripts/build-probe.sh
sudo systemctl stop fprintd
sudo ./build/probe info      # firmware / sensor info
sudo ./build/probe db        # database header state (ev=0x27 is healthy)
sudo ./build/probe list      # list on-chip templates
sudo systemctl start fprintd
```

Full `build/probe` subcommand set: `info`, `db`, `list`, `dump`, `raw`, `seq`,  
`ident`, `match`. It needs **neither the driver nor fprintd** — plug the device in  
and run it. Use it first when you need to tell "driver problem" from "hardware or  
cable problem".

## Running without hardware (simulator)

`tools/sim/` contains a software MOC-5159: the fixed-frame protocol, the on-chip template
database and the `0xC1` verdict all follow real firmware behaviour, with `LD_PRELOAD`
replacing libusb. **Not a single line of the driver changes** — libfprint / fprintd still see
the same `libfprint`, only the device is fake. Handy for regression runs on a machine without
the module.

```bash
cd tools/sim
./build.sh                 # libmocsim.so + two test programs
./run-tests.sh             # full regression matrix (11 scenarios)

LD_PRELOAD=./build/libmocsim.so ./build/mocsim-e2e                     # match
LD_PRELOAD=./build/libmocsim.so MOCSIM_MATCH=0 ./build/mocsim-e2e      # no match
LD_PRELOAD=./build/libmocsim.so MOCSIM_TEMPLATES=0 ./build/mocsim-e2e  # empty, full enrollment
```

Faults can be injected too (`MOCSIM_FAULT=stale,notail,quality,c1timeout,badcrc,gone`) to
exercise the driver's retry and error paths. See
[`tools/sim/README.md`](tools/sim/README.md) for variables and measured results.

## Repository layout

```
moc5159-libfprint-driver/
├── README.md                  this file
├── README.zh-CN.md            简体中文
├── CHANGELOG.md               release history
├── CONTRIBUTING.md            what to include in a report, PR requirements
├── LICENSE                    LGPL-2.1
├── VERSION
├── docs/
│   ├── README.md              documentation index (both languages)
│   ├── en/
│   │   ├── 01-install.md          install / upgrade / uninstall / manual build / distro packaging
│   │   ├── 02-usage.md            enrollment, verification, lock screen, PAM, sudo, multi-finger limits
│   │   ├── 03-troubleshooting.md  troubleshooting by symptom (device / driver / matching / PAM)
│   │   └── 04-protocol-notes.md   protocol notes and the evidence for the one-finger limit
│   └── zh-CN/                 same four documents in Chinese
├── src/
│   ├── moc5159.c              the libfprint driver (FpDevice subclass + SSM state machine)
│   ├── moc5159.h              device / protocol constants
│   ├── moc5159-proto.c        128-byte fixed-frame encode/decode (libfprint-independent)
│   └── moc5159-proto.h
├── scripts/
│   ├── install.sh             one-shot installer (multi-distro)
│   ├── uninstall.sh           uninstall / restore
│   ├── integrate.sh           register the driver into any libfprint source tree
│   ├── fetch-libfprint.sh     re-download libfprint 1.94.100 into vendor/ (only if you deleted it)
│   ├── build-probe.sh         build the standalone diagnostic tool
│   └── doctor.sh              environment self-check
├── tools/
│   ├── moc5159-probe.c        standalone diagnostic tool (libusb only)
│   ├── fp-enroll.sh           enroll (desktop press prompt, closes itself when done)
│   ├── fp-verify.sh           verify
│   ├── fp-list.sh             show fprintd state + on-chip database state
│   ├── fp-wipe.sh             wipe
│   ├── fp-prompt.sh           shared prompt helpers
│   └── sim/                   module simulator — run the driver with no hardware
│       ├── mocsim-dev.c       protocol state machine + on-chip database model
│       ├── mocsim-shim.c      LD_PRELOAD shim that fakes libusb (builds libmocsim.so)
│       ├── mocsim-selftest.c  protocol-level self-test
│       ├── mocsim-e2e.c       libfprint end-to-end test
│       ├── build.sh           build
│       ├── run-tests.sh       regression matrix (11 scenarios)
│       └── README.md / README.zh-CN.md
└── vendor/
    ├── libfprint-1.94.100.tar.gz   upstream libfprint source, pinned (12 MB, LGPL-2.1+)
    ├── README.md              what this archive is and how the installer uses it
    └── README.zh-CN.md
```

## How it works (short version)

The module exposes exactly two bulk endpoints (`0x01` OUT / `0x81` IN, 64 bytes)  
and no interrupt endpoint. Every command uses a **128-byte fixed frame**:  
`[0]` = payload length, `[2]` = command code, `[6:]` = payload, and the last byte  
holds the sum of the first 126.

A full verification round trip takes about 1.4 s:

```
DB_SIZE → DB_DUMP_RECORD ×N → VERIFY_CAPTURE(0xC0) → image block(0x13) → commit(0x31)
        → VERIFY_MATCHING(0xC1)
```

Three transport rules you cannot skip (ignoring them looks like "the device died"):

1. **Drain leftover IN frames before each command** — an unread frame makes the  
   firmware reject new bulk OUT transfers, so OUT NAKs until it times out;
2. **The bulk IN buffer must be exactly 128 bytes** — replies are two full 64-byte  
   packets with no short packet terminating them;
3. **Do not append a ZLP.**

The match result comes from the relationship between two blocks inside the `0xC1`  
payload, **not** from the `ev` byte (which is always `0x01`). Command table, the  
80-byte record layout, and the full evidence chain for the one-finger limit are in  
[docs/en/04-protocol-notes.md](docs/en/04-protocol-notes.md).


## FAQ

**`fprintd` says there is no device after installing?**  
Run `sudo ./scripts/doctor.sh` first, then see  
[docs/en/03-troubleshooting.md](docs/en/03-troubleshooting.md) §B1. The usual cause is  
that the active library is not the one in `/usr/local` (`ldconfig -p | grep libfprint-2`  
should list `/usr/local` first).

**Same finger, intermittent results?**  
The GSL6150 is a small-area sensor, so finger position and angle matter a lot. See the  
pressing tips in [docs/en/02-usage.md](docs/en/02-usage.md) §2.4. Also make sure the  
database holds exactly one template.

**Enrolled a second finger and the first one stopped working?**  
Firmware limitation — see [one finger only](#one-finger-limit).

**Fingerprint files keep disappearing?**  
`fprintd` compares print ids at Claim time and silently deletes stored entries that do  
not match. This is fixed in the current version (print id is taken from the on-chip  
template id); if it still happens see  
[docs/en/03-troubleshooting.md](docs/en/03-troubleshooting.md) §B3.

**Dual-booting with Windows?**  
Templates live in the chip's flash, so Windows and Linux share the same database —  
but **both only honour record #0**, and fprintd's local storage (`/var/lib/fprint/`)  
is Linux-only. Do not run both sides at once; they fight over the USB device.

## Out of scope

- No kernel module (this is a pure userspace driver);
- No image capture (match-on-chip means the raw image is unavailable);
- No multi-finger support (firmware limitation, not laziness);
- No alternative to Windows Hello (use the official driver there).

## Documentation

All documents are available in both languages. Each page has a language switch at the top.

|                     | English                                                        | 简体中文                                                                 |
| ------------------- | -------------------------------------------------------------- | -------------------------------------------------------------------- |
| Documentation index | [docs/README.md](docs/README.md)                               | [docs/README.md](docs/README.md)                                     |
| Install             | [docs/en/01-install.md](docs/en/01-install.md)                 | [docs/zh-CN/01-install.md](docs/zh-CN/01-install.md)                 |
| Usage               | [docs/en/02-usage.md](docs/en/02-usage.md)                     | [docs/zh-CN/02-usage.md](docs/zh-CN/02-usage.md)                     |
| Troubleshooting     | [docs/en/03-troubleshooting.md](docs/en/03-troubleshooting.md) | [docs/zh-CN/03-troubleshooting.md](docs/zh-CN/03-troubleshooting.md) |
| Protocol notes      | [docs/en/04-protocol-notes.md](docs/en/04-protocol-notes.md)   | [docs/zh-CN/04-protocol-notes.md](docs/zh-CN/04-protocol-notes.md)   |
| Changelog           | [CHANGELOG.md](CHANGELOG.md)                                   | [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md)                             |
| Contributing        | [CONTRIBUTING.md](CONTRIBUTING.md)                             | [CONTRIBUTING.zh-CN.md](CONTRIBUTING.zh-CN.md)                       |

> The helper scripts print their progress messages in Chinese. Commands, flags,  
> exit codes and log lines from libfprint/fprintd are unchanged.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). A verification report from any real machine  
is valuable, especially from **other distributions** and **other firmware versions**.

## License

Driver and scripts are released under **LGPL-2.1-or-later**, matching libfprint. See [LICENSE](LICENSE).

This project is not affiliated with Petaic, GigaDevice or Silead. It is an independent  
driver based on the publicly available Windows driver package and USB traffic analysis;  
it contains no vendor code.

## Acknowledgements

- [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) — the driver framework;  
  the `egismoc` / `goodixmoc` match-on-chip drivers were a structural reference;
- [fprintd](https://gitlab.freedesktop.org/libfprint/fprintd), [usbutils](https://github.com/gregkh/usbutils),  
  [capstone](https://www.capstone-engine.com/) — runtime and reverse-engineering toolchain.
