**English** · [简体中文](README.zh-CN.md)

# tools/sim — MOC-5159 module simulator

Run the **whole driver + libfprint stack** without plugging anything in.

Two pieces:

1. `mocsim-dev.c` — a software MOC-5159: the 128-byte frame protocol, the on-chip
   template database and the match verdict, modelled after the real firmware (down to the
   0xFF tail frame after every command);
2. `libmocsim.so` — an `LD_PRELOAD` shim that replaces libusb and routes every
   bulk/control transfer from libgusb/libfprint into that state machine.

**The driver is not modified in any way.** `fprintd`, the GNOME panel and `pam_fprintd` still
talk to the same libfprint; only the device on the other end is fake.

## Quick start

```bash
cd tools/sim
./build.sh                                   # libmocsim.so + two test programs

# 1) protocol-level self-test (no device, no libfprint needed)
LD_PRELOAD=./build/libmocsim.so ./build/mocsim-selftest

# 2) end to end: libfprint → moc5159 driver → simulated chip
LD_PRELOAD=./build/libmocsim.so ./build/mocsim-e2e

# 3) the whole regression matrix (run this after driver changes)
./run-tests.sh
```

`mocsim-e2e` walks: discovery → open (SETUP + DB_OPEN) → list → (enroll if the chip is
empty) → verify → identify → delete. Exit code 0 means everything behaved as expected, so it
works as a regression test.

## The three binaries

| Program | What it does | Needs |
|---|---|---|
| `libmocsim.so` | fake libusb + device state machine | libusb headers |
| `mocsim-selftest` | 12 protocol assertions (SETUP/D5/D9/C0/C1/image block/wipe…) | libusb only |
| `mocsim-e2e` | full libfprint round trip | the driver installed (libfprint 1.94.100) |

`mocsim-selftest` is the first thing to run when something looks wrong: if it fails the
simulator is at fault; if it passes but `mocsim-e2e` fails, the problem is in the driver or
libfprint.

## Scenarios

```bash
# match / no match
LD_PRELOAD=./build/libmocsim.so MOCSIM_MATCH=0 ./build/mocsim-e2e

# empty chip → full enrollment (5 capture stages + commit)
LD_PRELOAD=./build/libmocsim.so MOCSIM_TEMPLATES=0 ./build/mocsim-e2e

# log every command
LD_PRELOAD=./build/libmocsim.so MOCSIM_LOG=1 ./build/mocsim-e2e
```

| Variable | Default | Meaning |
|---|---|---|
| `MOCSIM_MATCH` | `1` | Next `0xC1` verdict: 1 = match, 0 = no match |
| `MOCSIM_TEMPLATES` | `1` | Templates preloaded in the chip (`0` = empty) |
| `MOCSIM_FAULT` | — | Comma-separated fault injection, see below |
| `MOCSIM_LOG` | `0` | One line per command on stderr |
| `MOCSIM_DB` | `0` | Dump the simulated database on exit |
| `MOCSIM_SPEED` | `0` | Artificial delay per transfer, in ms |

### Fault injection

| Value | Effect | Expected driver behaviour |
|---|---|---|
| `stale` | Leave one frame in the IN pipe | Driver drains it, everything still succeeds |
| `notail` | Omit the 0xFF tail frame | Drain times out immediately, everything still succeeds |
| `quality` | First `0xC4` answers `0xA0` | Driver asks for a re-press and retries, final run succeeds |
| `badcrc` | Corrupt reply checksum | Driver drops the frame, retries, reports "no answer to 0xDB" |
| `c1timeout` | `0xC1` never answers | Driver reports "no answer to 0xC1", verify/identify fail (the simulator does not wait for wall-clock timeouts, so it returns instantly; real hardware takes ~24 s) |
| `gone` | Every transfer times out | Open fails (device not present) |

### Measured results

VMware / Ubuntu 26.04 / libfprint 1.94.100. `./run-tests.sh` is 11/11 green, plus a 50-run stress loop
(mixed template counts + 20 back-to-back enroll runs) with zero failures and zero GLib
warnings.

| Scenario | Result |
|---|---|
| selftest | 12/12 |
| default (1 template, match) | 5/5, exit 0 |
| `MOCSIM_MATCH=0` | 5/5, exit 0 |
| `MOCSIM_TEMPLATES=0` (full enrollment) | 7/7, exit 0 |
| `MOCSIM_TEMPLATES=3` | 5/5, exit 0 |
| `FAULT=stale` (with `T=0`) | 7/7, exit 0 |
| `FAULT=notail` (with `T=0`) | 7/7, exit 0 |
| `FAULT=quality` (with `T=0`) | 7/7, exit 0 (one retry in the middle) |
| `FAULT=c1timeout` | 3 pass, 2 fail, exit 1, correct message |
| `FAULT=badcrc` | Open fails with the correct message |
| `FAULT=gone` | Open fails with the correct message |

> `notail`, `stale` and `quality` only cover the enrollment path together with
> `MOCSIM_TEMPLATES=0` — when a template already exists `mocsim-e2e` skips enrollment and
> runs verify/identify/delete only.

## Why intercept libusb

libfprint has no plugin mechanism — the driver is compiled into `libfprint-2.so.2`, libgusb
sits below it and libusb below that. Modifying the driver would pollute the shipped code and
faking `GUsbDevice` is impossible (private GObject internals), so the shim hooks libusb: a
small API surface, public struct layouts, and libgusb treats it as a black box.

libgusb 0.4 uses the **asynchronous** libusb interface (`libusb_submit_transfer` +
`libusb_handle_events_*`), so the shim keeps a pending-transfer queue and completes it in the
event pump. The synchronous entry points (`libusb_bulk_transfer`) are exported too, which is
why `tools/moc5159-probe` can run against the simulator as well.

Two timing traps to keep in mind when touching the shim:

- **The queues must be locked.** Real libusb serialises internally, but libgusb pumps events
  from its event thread *and* from the main context — two threads can pop the same transfer and
  complete the same GTask twice, which shows up as a random `SIGSEGV` plus
  `g_task_return_int: assertion '!task->ever_returned' failed`. The shim guards every queue
  operation with one **recursive** mutex; recursive because the pump holds it while calling
  `sim()`, and `sim()` takes it again on first initialisation.
- **Completion and callback are two separate dispatches.** Servicing a transfer only moves it
  to the ready queue; the callback runs on the *next* pump call. Calling back inside the same
  stack would let libgusb retire the GTask while libfprint is still in its own completion
  path — the same assertion again.

## Known issues

1. **The test exits with `_exit()`** — tearing down `FpContext` joins libgusb's event thread,
   which hangs against an `LD_PRELOAD` backend. The test is over at that point.
2. **No wall-clock timeout emulation** — "device does not answer" fails immediately instead of
   waiting out the USB timeout, so `FAULT=c1timeout` returns instantly rather than taking the
   ~24 s real hardware needs.
3. **No hotplug** (`libusb_has_capability` returns 0) — the device exists for the lifetime of
   the process.
4. **No biometric matching** — the chip compares templates internally, so the simulator only
   produces the `0xC1` structure the driver judges. It validates **protocol and timing**, not
   the algorithm.

## Coexistence with real hardware

`LD_PRELOAD` only affects the process that sets it, so the real module can stay plugged in.
Within one process only the simulated device is visible.

```
tools/sim/
├── mocsim.h          device model API + fault bits
├── mocsim-dev.c      protocol state machine
├── mocsim-shim.c     LD_PRELOAD libusb replacement
├── mocsim-selftest.c protocol self-test
├── mocsim-e2e.c      libfprint end-to-end test
├── build.sh          build
├── run-tests.sh      regression matrix (run this after driver changes)
└── README.md / README.zh-CN.md
```
