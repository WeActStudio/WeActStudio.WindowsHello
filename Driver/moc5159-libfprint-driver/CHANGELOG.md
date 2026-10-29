**English** · [简体中文](CHANGELOG.zh-CN.md)

# Changelog

Versioning: major = incompatible driver interface or behaviour change; minor = new features;  
patch = bug fixes.

## 1.0.0 — 2026-10-25

First public release.

### Driver

- libfprint driver `moc5159` for `28e9:5159` (Petaic MOC-5159 / GD32W515 + GSL6150,  
  match-on-chip; templates and matching stay on the chip and never enter system memory);
- implements the full `FpDevice` interface — `open` / `close` / `enroll` / `verify` /  
  `identify` / `list` / `delete` — using libfprint's SSM state machine;
- transport layer handles the three hard requirements: drain leftover IN frames before each  
  command, keep the bulk IN buffer at exactly 128 bytes, do not append a ZLP;
- match verdict derived from the internal structure of the `0xC1` payload (the `ev` byte is  
  unrelated to the result);
- template id is `max(tid)+1`, and `0xC3` submits the complete 80-byte record;
- the print id of an enrolled fingerprint is taken from the template id, so fprintd no longer  
  silently deletes stored fingerprints on an id mismatch.

### Install / operations

- `scripts/install.sh`: one-shot install across distributions (apt / dnf / yum / zypper /  
  pacman / apk), installs into `/usr/local` by default and uses `ld.so.conf.d` to make this  
  driver take priority — **without touching the distribution's libfprint files**;
- `--system` mode can overwrite `/usr`, backing up every file first so it can be fully restored;
- `scripts/uninstall.sh`: clean uninstall in both modes, never touching enrolled fingerprint data;
- `scripts/doctor.sh`: 8-point environment self-check;
- `scripts/build-probe.sh`: builds the standalone `moc5159-probe` diagnostic tool (libusb only,  
  bypasses fprintd);
- `scripts/fetch-libfprint.sh`: re-downloads the libfprint 1.94.100 source into `vendor/`  
  (only needed if you deleted the bundled archive or want to refresh it);
- `vendor/libfprint-1.94.100.tar.gz`: the pinned upstream source tree ships with the  
  repository (~12 MB), so a plain `git clone` installs **without network access**;
- upstream `v1.94.100`'s `tests/meson.build` iterates a dict with a single variable, which  
  meson ≥ 1.10 rejects; `install.sh` and `fetch-libfprint.sh` apply the compatibility patch  
  automatically (affects test cases only, not the driver);
- `tools/fp-enroll.sh` / `fp-verify.sh` / `fp-list.sh` / `fp-wipe.sh`: one-shot operations with  
  desktop prompts.

### Documentation

- Four documents — installation, usage, troubleshooting, protocol — plus this file and  
  `CONTRIBUTING.md`, available in English and Simplified Chinese.

### Simulator (test without hardware)

- `tools/sim/`: a software MOC-5159 — 128-byte frame protocol, on-chip template database and
  the `0xC1` verdict modelled on real firmware, plus an `LD_PRELOAD` libusb shim, so the
  unmodified driver runs end to end on a machine without the module;
- `mocsim-selftest` (12 protocol assertions) and `mocsim-e2e` (libfprint discovery → open →
  list → enroll → verify → identify → delete);
- fault injection: `stale` (leftover IN frame), `notail` (missing `0xFF` tail frame),
  `quality` (first capture rejected), `c1timeout`, `badcrc` (bad checksum), `gone`;
- `tools/sim/run-tests.sh`: 11-scenario regression matrix, green on Ubuntu 26.04 /
  libfprint 1.94.100.

### Known limitations (firmware behaviour the driver cannot bypass)

- **During verification the firmware compares only record #0** of the template database, so a  
  second enrolled finger never takes effect;
- the `0x30` reply is always `0x00` on this model and cannot be used to detect a finger press;
- a multi-template database (creating a second database with `0xDA`) makes the device  
  unresponsive until it is physically replugged.

### Tested environment

Ubuntu 26.04.1 (kernel 7.0.0) + fprintd 1.94.5, VMware Workstation USB passthrough;  
the distribution's libfprint 1.95.1 coexists fine with this driver's 1.94.100.
