**English** · [简体中文](../zh-CN/03-troubleshooting.md)

# 03 · Troubleshooting

Run the self-check first — it pinpoints most problems on its own:

```bash
sudo ./scripts/doctor.sh
```

Below, look up by **symptom**.

> `doctor.sh` and the install scripts print their status lines in Chinese. Where a log line
> or output string matters for diagnosis, it is quoted verbatim with an English gloss.

---

## A. Device layer

### A1. `28e9:5159` is missing from `lsusb`

| Situation | Action |
|---|---|
| Virtual machine | The USB device is not passed through. VMware: *VM → Removable Devices → find GDMicroelectronics → Connect*; VirtualBox: *Devices → USB → tick it* |
| Passed through to Windows | Windows Hello on the host stops working while passed through — expected |
| Changed USB port | Move it back to the original port, or pass it through again |
| No reaction at all | Unplug and replug once; if still nothing, it is a hardware or cable problem |

### A2. Device present, but the driver cannot open it (`probe` reports busy / timeout)

Usually the **USB channel is wedged** (left over from an earlier abnormal command). Typical
symptom: every bulk OUT times out, and the SETUP byte in `probe info` changes from `0x5C` to `0x00`.

- Try this first (recovers a "channel NAK" state, not a "command killed the device" state):
  ```bash
  sudo systemctl stop fprintd
  sudo ./build/probe --clear-halt raw 30
  ```
- If that does not help: **physically unplug and replug** (or disconnect and reconnect the
  device in the VM).
  ⚠️ Do not USB-reset a passed-through device — the host then flags it as a phantom and only
  a physical replug fixes it.

---

## B. Driver layer

### B1. Installed, but fprintd says no device (`No device available` / empty list)

Check in this order:

```bash
# 1) does the build contain this device?
/usr/local/bin/fprint-list-supported-devices 2>/dev/null | grep 5159
# or use the build directory
/var/tmp/moc5159-build/build/libfprint/fprint-list-supported-devices | grep 5159

# 2) which libfprint does the dynamic linker actually use?
ldconfig -p | grep 'libfprint-2\.so\.2'
# the first line must be /usr/local/...; if it is /usr/lib/... the priority is not in effect

# 3) which one did fprintd actually load?
sudo grep -o '/[^ ]*libfprint-2\.so\.2[^ ]*' /proc/$(pgrep -x fprintd)/maps | head -1
```

Fix:

```bash
find /usr/local -name 'libfprint-2.so.2.0.0'     # confirm where it was installed
# assume /usr/local/lib/x86_64-linux-gnu
sudo sh -c 'echo /usr/local/lib/x86_64-linux-gnu > /etc/ld.so.conf.d/00-moc5159-libfprint.conf'
sudo ldconfig
sudo systemctl restart fprintd
```

> The `00-` prefix makes the file sort first in `ld.so.conf.d/*.conf`, so the `/usr/local`
> library wins over the distribution path.

### B2. Build failures

| Error | Cause / fix |
|---|---|
| `OpenSSL is required for uru4000` | Missing openssl development package. `sudo apt install libssl-dev` / `dnf install openssl-devel`. **Or use `--drivers moc5159` to skip the other drivers** |
| `Dependency "gusb" not found` | Missing libgusb development package: `libgusb-dev` / `libgusb-devel` |
| `Dependency "libudev" not found` | `libudev-dev` / `systemd-devel` |
| `Meson version is too old` | Requires meson ≥ 0.62: `pip3 install --break-system-packages "meson>=1.0"` or `pipx install meson` |
| `Foreach expects exactly 2 variables for iterating over objects of type dict` | meson ≥ 1.10 rejects the old syntax in upstream `tests/meson.build`. `install.sh` patches it automatically; for a manual build run: `sed -i 's/foreach driver_test *: *drivers_tests/foreach driver_test, _args : drivers_tests/' <source-tree>/tests/meson.build` |
| `driver_sources` table not found | Your libfprint source tree is too old or too new; the structure changed. Use 1.94.100 (`./scripts/fetch-libfprint.sh`) |
| `undefined reference to fpi_*` at link time | `moc5159` was not registered in the tree (`integrate.sh` did not succeed). Re-run `./scripts/integrate.sh <tree>` and check its output |

### B3. Fingerprint data disappears unexpectedly

Historical issue (**fixed in 1.0.0**). Symptom: files under `/var/lib/fprint/<user>/moc5159/0/`
suddenly vanish, with this in the log:

```
Deleted stored finger 7 ... as it is unknown to device
```

Cause: on every Claim, fprintd compares the stored print id against the id reported by the
device and **deletes the file** when they disagree. The old driver used a wrong length
predicate when reading the template id back after enrollment (84 vs 88 bytes), so the id was
garbage (7, 8, 0x5A, ...) and any Claim could wipe the fingerprint.

Fixed in 1.0.0 (predicate is now `4 + 80`, and the id is taken from the record's template
number). If you hit this, you are running an old build — reinstall and enroll again.

---

## C. Capture / matching layer

### C1. Pressing does nothing (enrollment hangs, verification times out)

- **No** `验证采集` / `C0` lines at all in the fprintd log → the finger never triggered capture;
- capture lines appear but the quality score is very low (`0x00`-`0x30`) → pressed too lightly
  or off-centre.

Remedies:
1. **Centre of the fingertip pad, straight down**, covering the whole sensor window, held for
   about 1 second;
2. lift completely between presses (do not keep the finger resting on it);
3. wipe the sensor surface if it is greasy or wet;
4. in winter, breathe on the finger if the skin is very dry.

> On this model, `0x30` (session status word) is always `0x00` and **cannot** be used to detect
> a finger, which is why the driver does not rely on it — seeing it stay at 0 does not mean the
> device is broken.

### C2. Always `verify-no-match`, even with the right finger

In descending order of likelihood:

1. **Multiple templates in the database** (most common). This firmware compares only the
   first record.
   ```bash
   ./tools/fp-list.sh        # how many fingers fprintd knows
   sudo ./tools/fp-list.sh   # how many records are on the chip
   ```
   If more than one → keep a single finger: `sudo fprintd-delete $USER -f <extra finger>`,
   or start over: `sudo ./tools/fp-wipe.sh` then enroll again.

2. **Enrolled with a different posture**. Re-enroll with the same posture
   (`fp-wipe.sh` → `fp-enroll.sh`).

3. **Insufficient capture quality**. A quality score below `0x50` in the log basically means
   the press was not firm enough.

4. **A different finger**. Templates are built for the finger that was enrolled; another
   finger can never match.

### C3. `Verify result: verify-retry-scan`

Capture quality was insufficient and the chip asks for another press. Press again; several
retries in a row mean your pressing is inconsistent.

### C4. Why small-area sensors are never very stable

This is a 64×80 small-area sensor (about 5 mm wide), so repeatability is inherently worse than
large-area sensors. Keeping "how you press at enrollment" and "how you press when unlocking"
consistent noticeably improves the success rate. Enroll **the finger you can reach most
comfortably** (usually the index finger of your dominant hand).

---

## D. PAM / login layer

### D1. Lock screen still only accepts a password

```bash
grep pam_fprintd /etc/pam.d/common-auth
```

- No line → not enabled: `sudo pam-auth-update --enable fprintd`
- Line present but password still required → check it is `sufficient` (not `required`), and
  that `gdm-password` does `@include common-auth`
- Too few fingerprint attempts (one miss jumps to password) → add `max-tries=5`
  (see [02-usage.md](02-usage.md) section 5.2)

### D2. `sudo` asks for the fingerprint every time — annoying

If you only want fingerprints for login/lock screen and a password for `sudo`: move the
`pam_fprintd` line out of `common-auth` into `/etc/pam.d/gdm-password` /
`gdm-launch-environment` and configure it there; or explicitly skip the fingerprint line in
`/etc/pam.d/sudo` (advanced; back up first).

### D3. GNOME Settings keeps spinning on "Fingerprint" / will not open

Known issue: the polkit dialog and `pam_fprintd` can deadlock over the input method.
**Functionality is unaffected** — enroll from the command line:

```bash
sudo ./tools/fp-enroll.sh right-index-finger "Right index"
```

Reopen Settings afterwards to see it (sometimes `gnome-control-center` needs a restart).

### D4. Fingerprint matched, but the password is still requested

`pam_fprintd` is in the wrong position in the PAM stack (too late; an earlier module already
demanded a password). `sufficient` must come **before** the password modules. Stacks generated
by `pam-auth-update` are usually correct.

---

## E. Coexistence with Windows

- The driver opens the template database with **the same DB GUID as Windows**, so it can see
  templates enrolled under Windows.
- **Wiping the database also wipes the Windows templates** (`fp-wipe.sh` / `0xD7`). If you
  still use Windows Hello, be careful with wipes.
- On a successful match the driver performs one `0xD8` (adaptive template write-back), matching
  Windows' behaviour; it does not damage the Windows-side template.
- Do not use the device from both sides at once (Windows running and Linux running) — they
  fight over USB.

---

## F. Collecting information (for bug reports)

```bash
{
  echo "=== system ==="; cat /etc/os-release | head -3; uname -a
  echo "=== device ==="; lsusb | grep -i 28e9
  echo "=== library ==="; ldconfig -p | grep libfprint-2.so.2
  echo "=== install record ==="; cat /var/lib/moc5159-driver/meta 2>/dev/null
  echo "=== fprintd ==="; systemctl status fprintd --no-pager 2>&1 | head -10
  echo "=== fingerprints ==="; fprintd-list $USER 2>&1
  echo "=== PAM ==="; grep pam_fprintd /etc/pam.d/common-auth 2>/dev/null
  echo "=== logs ==="; sudo journalctl -t fprintd --since '-30 min' --no-pager | tail -60
} > /tmp/moc5159-report.txt 2>&1
```

Attach `/tmp/moc5159-report.txt` to your report.
