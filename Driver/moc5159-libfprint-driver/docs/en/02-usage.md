**English** · [简体中文](../zh-CN/02-usage.md)

# 02 · Usage

Everything below is day-to-day operation once the driver is installed. All commands are
run from the repository root.

---

## 1. From zero to working, in one block

```bash
sudo ./scripts/install.sh --with-pam                # 1) install driver + hook up PAM
sudo ./tools/fp-enroll.sh right-index-finger "Right index"   # 2) enroll one finger
./tools/fp-verify.sh right-index-finger             # 3) verify once
# 4) Super+L to lock, then unlock with the same finger
```

---

## 2. Enrolling a finger

### 2.1 Desktop script (recommended, shows a press prompt)

```bash
sudo ./tools/fp-enroll.sh right-index-finger "Right index"
```

- A prompt gives you 10 seconds to get ready, then enrollment starts (4-8 presses);
- **the prompt closes itself immediately when enrollment finishes or times out**;
- the terminal prints `登记结果: 成功` (enrollment succeeded) and lists the enrolled fingers.

Finger names (libfprint convention, these exact strings are required):

```
left-thumb            left-index-finger   left-middle-finger
left-ring-finger      left-little-finger
right-thumb           right-index-finger  right-middle-finger
right-ring-finger     right-little-finger
```

### 2.2 Plain command line

```bash
sudo fprintd-enroll $USER -f right-index-finger
```

### 2.3 GNOME Settings panel

*Settings → System → Users → Fingerprint → +*, then press as prompted.
If the panel hangs on "starting fingerprint service", enroll from the command line instead
(the usual cause is a polkit dialog and `pam_fprintd` fighting over input; it does not
affect actual use).

### 2.4 Pressing technique (directly affects the success rate)

The sensor is a **small glass-covered area**, and the hit rate depends heavily on how you press:

| Point | Detail |
| ------ | --------------------- |
| Centre of the fingertip pad | not the tip, not the side at an angle |
| Press straight down | cover the whole sensor window, apply a little pressure |
| About 1 second per press | hold for ~1 s, then **lift completely** before pressing again |
| Be consistent | same position and angle every time; do not alternate left/right of centre |

> How you press during enrollment is how you must press when unlocking. Enroll "loosely" and
> you will have to unlock "loosely".

---

## 3. Verification

```bash
./tools/fp-verify.sh right-index-finger "Right index"
# Verify result: verify-match (done)      <- match
# Verify result: verify-no-match (done)   <- no match
```

⚠️ **Do not** use `fprintd-verify` without `-f`: it only compares against the first entry in
the list and gives misleading results when several fingers are enrolled.

Verification straight from the CLI (no desktop prompt):

```bash
fprintd-verify $USER -f right-index-finger
```

---

## 4. Checking current state

```bash
./tools/fp-list.sh          # normal user: what fprintd has registered
sudo ./tools/fp-list.sh     # root: additionally reads on-chip records
sudo ./scripts/doctor.sh    # full environment self-check
```

Sample output:

```
== fprintd 里用户 guocai 的指纹 ==
User guocai has 1 fingerprint(s) registered:
  - #0: right-index-finger

== 芯片模板库 ==
  已用 1 / 10
  [0] 模板号=1  计数器=1
```

If the two counts disagree, storage and chip are out of sync — see
[03-troubleshooting.md](03-troubleshooting.md).

---

## 5. Wiring into login / lock screen / sudo (PAM)

### 5.1 One-shot enable (Debian / Ubuntu)

```bash
sudo pam-auth-update --enable fprintd
# or during installation: sudo ./scripts/install.sh --with-pam
```

It adds a `pam_fprintd.so` line to `/etc/pam.d/common-auth`; `gdm-password`, `login`,
`sudo` and the lock screen all inherit it through `@include common-auth`.

### 5.2 Allow several fingerprint attempts (important)

The default `max-tries=1` means **one wrong press jumps straight to the password prompt**.
Change it to five consecutive tries:

```bash
sudo sed -i 's|pam_fprintd.so|pam_fprintd.so max-tries=5 timeout=10|' /etc/pam.d/common-auth
grep pam_fprintd /etc/pam.d/common-auth
```

| Option | Meaning | Suggested |
| ------------- | ------------ | --------------- |
| `max-tries=N` | allow N consecutive fingerprint attempts | 5 (enough, without getting stuck on fingerprints) |
| `timeout=N` | seconds to wait per attempt | 10 |
| `try-first` | fingerprint before password (default behaviour) | leave as is |

### 5.3 Other distributions

On systems without `pam-auth-update` (Fedora / Arch / openSUSE), edit
`/etc/pam.d/system-auth` (or `password-auth`) and add near the **top** of the `auth` section:

```
auth      sufficient    pam_fprintd.so max-tries=5 timeout=10
```

`auth sufficient` means: a successful fingerprint lets you straight through, a failure falls
through to the password modules below — that is the key to "falls back to password
automatically". **Do not** use `required`, or a failed fingerprint blocks the password too.

### 5.4 Disabling fingerprint login

```bash
sudo pam-auth-update --disable fprintd      # Debian/Ubuntu
# or delete the pam_fprintd line from common-auth / system-auth manually
```

---

## 6. Lock screen / login behaviour

| Scenario | Behaviour |
| ------ | -------------------------------------------- |
| Lock screen | just press the finger; no need to click "log in with fingerprint" first |
| GDM login | the fingerprint icon is clickable, or simply press the sensor |
| `sudo` | when it prints `Password or swipe finger:`, press the finger |
| Fingerprint fails | waits `timeout` seconds and tries again, `max-tries` times in total, then shows the password prompt |

If the lock screen shows no fingerprint hint, PAM is usually not enabled (section 5) —
it has nothing to do with the driver.

---

## 7. ⚠️ The multi-finger limitation (must read)

**The firmware compares only against record #0 of the template database during verification.**

Measured (each case verified repeatedly):

| Database state | Result |
| ------------- | --------------------------- |
| exactly 1 template | normal, stable matching |
| 2 templates (index first, then middle) | index matches, middle **never matches** |
| 2 templates, delete the first one | the remaining one **works immediately** |
| load only template #2 as candidate | still no match (D9 cannot select the comparison target) |

The cause is the firmware, not the driver. In captures of the official Windows driver, the
5 records in the database are in fact **4 samples of the same template id** (accumulated by
adaptive write-back); Windows never actually compared two different templates. Commands such
as `DB_QUERY_BY_SUBJECT` — "select template by subject" — are never called in the
verification path. Technical details in [04-protocol-notes.md](04-protocol-notes.md).

### I want a different finger — what now?

**Wipe first, then enroll the new one:**

```bash
sudo ./tools/fp-wipe.sh                                  # clear on-chip DB + local storage; asks for "yes"
sudo ./tools/fp-enroll.sh left-thumb "Left thumb"         # enroll the one you want
```

### I already enrolled two — what now?

```bash
sudo fprintd-delete $USER -f right-middle-finger   # delete the unwanted one
./tools/fp-list.sh                                  # confirm only one remains
```

The remaining finger recovers immediately (because comparison uses "the first record", so
removing whatever is in front of it is enough).

---

## 8. Deleting fingerprints / wiping

```bash
# delete a single finger
sudo fprintd-delete $USER -f right-index-finger

# wipe everything (chip + local storage)
sudo ./tools/fp-wipe.sh
```

`fp-wipe.sh` sends `0xD7` (DB_WIPE) to the chip and deletes the files under
`/var/lib/fprint/<user>/moc5159/*/`. **Not recoverable**, hence the `yes` confirmation.

---

## 9. Service and logs

```bash
systemctl status fprintd          # status (D-Bus activated; not running while idle is normal)
sudo systemctl restart fprintd    # restart
sudo journalctl -t fprintd -f     # follow driver logs
sudo journalctl -t fprintd --since '-10 min' | grep -E '比对结果|质量|模板'
```

Driver debug output (`fp_info`) reaches the journal under the `fprintd` tag. Common lines:

| Log line | Meaning |
| ------------------ | ---------------------- |
| `模板库已挂载 (ev=0x27)` | database header healthy |
| `模板库已挂载 (ev=0x29)` | empty header (needs rebuilding; the driver sends 0xDA automatically) |
| `设备内模板数 N` | used record count read from `D5` |
| `比对结果: 命中 / 未命中` | match verdict returned by the chip |
| `验证采集质量 0x5D` | image quality score (0x5D-0x64 is normal) |

---

## 10. Where data lives, and migration

| Content | Location |
| ------------------------ | ----------------------------------------------------- |
| Template itself | **inside the chip's flash** (match-on-chip; the host never sees the features) |
| fprintd record (finger name, print id) | `/var/lib/fprint/<user>/moc5159/<device>/<id>` |
| Driver install record | `/var/lib/moc5159-driver/` (meta + file list + system-mode backups) |

- Reinstalling the OS: **templates stay on the chip**, but fprintd's records are lost.
  If `fprintd-list` shows nothing after reinstalling the driver, check the chip with
  `probe list`; if templates are there, re-enroll the same finger once (creates a new
  record) or simply keep using it (unlocking still works, only the GNOME panel will not
  show the finger name).
- Different user: fingerprints are enrolled **per user**; another user must enroll the same
  finger again.
- Different machine: the chip travels with the device; plug it into the new machine,
  install the driver, enroll again.

---

## 11. Automation / unattended use

When you do not want desktop prompts, use the standard commands:

```bash
# non-interactive enrollment (you are responsible for pressing when prompted)
sudo fprintd-enroll $USER -f right-index-finger

# test the result
if fprintd-verify $USER -f right-index-finger | grep -q 'verify-match'; then
    echo "matched"
fi
```

Without `DISPLAY` / `zenity`, `tools/fp-*.sh` degrade to plain terminal output and never block.

---

## 12. Cheat sheet

| Goal | Command |
| ------ | ------------------------------------------------------------------------------------------------- |
| Install | `sudo ./scripts/install.sh [--with-pam]` |
| Uninstall | `sudo ./scripts/uninstall.sh` |
| Enroll | `sudo ./tools/fp-enroll.sh <finger> ["Label"]` |
| Verify | `./tools/fp-verify.sh <finger>` |
| Status | `./tools/fp-list.sh` / `sudo ./scripts/doctor.sh` |
| Switch finger | `sudo ./tools/fp-wipe.sh`, then enroll again |
| Delete one | `sudo fprintd-delete $USER -f <finger>` |
| More fingerprint attempts | `sudo sed -i 's\|pam_fprintd.so\|pam_fprintd.so max-tries=5 timeout=10\|' /etc/pam.d/common-auth` |
| Follow logs | `sudo journalctl -t fprintd -f` |
| Standalone diagnostics | `sudo ./scripts/build-probe.sh && sudo ./build/probe list` |
