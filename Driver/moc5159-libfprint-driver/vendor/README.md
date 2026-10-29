**English** · [简体中文](README.zh-CN.md)

# vendor/ — bundled libfprint source

`libfprint-1.94.100.tar.gz` (~12 MB) is the **unmodified upstream libfprint 1.94.100 source
tree**, packed so that this repository can build and install completely offline.

It is third-party code, not part of this project: libfprint is © its respective authors and
licensed under **LGPL-2.1-or-later** (the full text is inside the archive, as
`libfprint-1.94.100/COPYING`). This project's own code uses the same licence, which is what
allows the driver to be built into the library.

## Why the source has to be shipped at all

libfprint has **no external plugin mechanism**: a driver only exists once it has been
compiled into `libfprint-2.so.2`. `scripts/install.sh` therefore needs a libfprint source
tree, not just the runtime package. Shipping the tree here means:

```bash
git clone <this repo>
cd moc5159-libfprint-driver
sudo ./scripts/install.sh          # no network access needed
```

## If you would rather not keep 12 MB around

| Option | Command |
|---|---|
| Use your own source tree | `sudo ./scripts/install.sh --libfprint-src /path/to/libfprint` |
| Delete the archive, let the installer clone upstream | `rm vendor/libfprint-1.94.100.tar.gz && sudo ./scripts/install.sh` |
| Re-fetch it later | `./scripts/fetch-libfprint.sh` |

Anything other than the bundle is downloaded from upstream tag `v1.94.100`.

## One patch is applied at build time

Upstream 1.94.100 has a `foreach` over a dict in `tests/meson.build` that meson ≥ 1.10
rejects (`Foreach expects exactly 2 variables`). The archive in this directory is already
fixed, and `scripts/install.sh` re-checks and patches any tree you point it at, so builds
work on both old and new meson.

## Why 1.94.100 is pinned

The driver is written against the libfprint **1.94 series driver API**. Upstream 1.95 and
later changed parts of the `FpDevice` / `FpiSsm` interfaces, which can break the build.
Pinning also makes "it compiles" reproducible regardless of distribution packaging.
