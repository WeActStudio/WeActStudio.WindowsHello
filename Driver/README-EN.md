# Driver Guide

**English** · [简体中文](./README-ZH.md)

This fingerprint module uses **two completely different drivers** on Windows and Linux.  
Pick the section for the system you are on:

| System                    | Driver                                                            | File                               | What you do                                                                                  |
| ------------------------- | ----------------------------------------------------------------- | ---------------------------------- | -------------------------------------------------------------------------------------------- |
| **Windows 10 / 11 (x64)** | Petaic official WBDI driver (Microsoft WHQL signed, v4.0.105.5)   | `MOC-release-signed-4.0.105.5.zip` | Usually installs itself when plugged in; otherwise point Windows at the INF — about a minute |
| **Linux**                 | Open-source libfprint driver `moc5159` (source + install scripts) | `moc5159-libfprint-driver/`        | One command to build and install, 1–3 minutes                                                |

> **Hardware note**: USB 2.0 device, ID `28e9:5159`. Plug it into a **native USB-A port  
> on the back of the machine**, not a hub, extension cable, or front-panel port — insufficient power  
> shows up as "nothing happens" or as intermittent recognition.

---

## 1. Windows

### 1.1 First choice: let Windows install it

1. Plug the module into a native USB port;
2. With the machine online, wait **10 s – 5 min** (Windows Update matches the official driver and installs it);
3. Open **Device Manager** → **Biometric devices** → `Fingerprint Device` should be listed with no yellow mark;
4. **Settings → Accounts → Sign-in options → Fingerprint recognition (Windows Hello)** → *Add* →  
   press the sensor repeatedly as prompted.

Fingerprint unlock, sign-in and UAC confirmation then work.

### 1.2 If it did not install itself (works offline)

Unzip `MOC-release-signed-4.0.105.5.zip` to get the `GD-MOC-release-signed-4.0.105.5` folder with five  
files inside. Use **one** of the following:

**Option A: command line (recommended, Windows 10 1809+)**

Open PowerShell **as Administrator**, change into the extracted  
`GD-MOC-release-signed-4.0.105.5` folder (use your own path), then:

```powershell
cd "C:\path\to\GD-MOC-release-signed-4.0.105.5"
pnputil /add-driver .\PetaicTouchFP-MOC-5159.inf /install
```

A success message such as "Driver package added successfully" means it worked. Then replug the module,  
or run:

```powershell
pnputil /scan-devices
```

**Option B: Device Manager**

1. Right-click Start → **Device Manager**;
2. Find the module under *Other devices* or *Biometric devices* — it may show as `Fingerprint Device`,  
   an unknown device, or an entry with a yellow mark. Check its **hardware ID** is `USB\VID_28E9&PID_5159` if unsure;
3. Right-click → **Update driver** → **Browse my computer for drivers** → select the extracted  
   `GD-MOC-release-signed-4.0.105.5` folder (the one containing the `.inf` file) → Next;
4. Accept the signature prompt.

> ⚠️ **Do not right-click the INF and choose "Install".** This is a PnP driver and the INF has no  
> `DefaultInstall` section; that route only produces "The INF file you selected does not support this  
> method of installation". Use one of the two options above.

### 1.3 Verify the install (any one of these)

```powershell
Get-PnpDevice -Class Biometric        # Status should be OK
Get-Service PetaicSrv                 # should be Running — the engine service; stop it and enrollment fails
```

Or simply open **Settings → Accounts → Sign-in options** — the fingerprint entry should be clickable  
and allow adding a finger.

### 1.4 Uninstall / reinstall

```powershell
# find the package name (note the oemXX.inf value)
pnputil /enum-drivers | Select-String -Pattern "Petaic" -Context 3,3

# remove it (replace oem12.inf with the name you found)
pnputil /delete-driver oem12.inf /uninstall /force
```

Alternatively: Device Manager → right-click the device → **Uninstall device** → tick  
"Attempt to remove the driver for this device" → OK, then reboot.

> Enrolled fingerprints live **inside the module's flash chip**, so uninstalling the driver does not  
> erase them. If Windows no longer lists a fingerprint after reinstalling, re-enroll — the Windows-side  
> user binding lives in `WinBioDatabase`.

### 1.5 What is in the archive

| File                         | Purpose                                                               | Installed to                         |
| ---------------------------- | --------------------------------------------------------------------- | ------------------------------------ |
| `PetaicTouchFP-MOC-5159.inf` | Installation description, matches hardware ID `USB\VID_28E9&PID_5159` | Driver store (`DriverStore`)         |
| `petaictouchfp.cat`          | Microsoft signature catalog covering the three binaries               | Same driver store                    |
| `PetaicTouchFP.dll`          | UMDF user-mode driver, speaks the module's private USB protocol       | `C:\Windows\System32\drivers\UMDF\`  |
| `PetaicTouchFPEngine.dll`    | Windows Biometric engine adapter (sensor / engine / storage)          | `C:\Windows\System32\WinBioPlugins\` |
| `PetaicSrv.exe`              | Biometric service process, starts automatically                       | `C:\Windows\System32\PetaicSrv.exe`  |

All four binaries carry a valid **Microsoft Windows Hardware Compatibility Publisher** signature, so  
Windows 10/11 will not block them and Windows Update can match the package automatically.

### 1.6 Windows troubleshooting

| Symptom                                                  | Cause                                                                          | Fix                                                                                                                                 |
| -------------------------------------------------------- | ------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------- |
| Nothing happens at all; Device Manager stays empty       | Not enough power / hub or front-panel port / bad cable                         | Use a rear native USB-A port, try another cable, reboot and replug                                                                  |
| Yellow mark in Device Manager                            | Driver missing or version mismatch                                             | Install manually per §1.2; if it still fails, uninstall the old driver first                                                        |
| `pnputil` says it cannot add the package                 | Not elevated, or antivirus removed a file                                      | Use an Administrator PowerShell; verify the folder still has all five files                                                         |
| Device is fine but there is no fingerprint option        | Biometric service disabled, or blocked by policy                               | `Get-Service WbioSrvc`; if not Running, `net start WbioSrvc`. On managed machines check the "allow biometrics" policy               |
| Enrollment fails halfway                                 | Small sensor (GSL6150), finger not covering it                                 | Press the **whole fingertip flat over the sensor window** and hold until prompted; moisten a dry finger; retry with the same finger |
| Fingerprint does not work, or a new finger never matches | Stale templates on the chip (firmware compares only the first record — see §3) | Delete all fingerprints in Settings, reboot, enroll **one** finger only                                                             |
| ARM64 laptop                                             | The official package is x64-only                                               |                                                                                                                                     |

---

## 2. Linux

There is no upstream support for this module. `moc5159-libfprint-driver/` is a libfprint driver  
written by reverse-engineering the official Windows driver; once installed, the whole `fprintd`  
stack works (CLI, GNOME panel, `pam_fprintd`).

### 2.1 Requirements

| Item            | Requirement                                                                                                                             |
| --------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| Architecture    | x86_64 / aarch64                                                                                                                        |
| Distribution    | Tested on **Ubuntu 26.04** with fprintd 1.94.5; should work on Debian 12+, Ubuntu 22.04+, Fedora 39+, Arch, openSUSE                    |
| Dependencies    | Installed by the script (needs sudo and network for packages; the libfprint source ships with the repo, so compiling itself is offline) |
| Virtual machine | Pass the USB device through to the guest first (VMware: VM → Removable Devices → Connect)                                               |

### 2.2 Install (three commands)

```bash
cd moc5159-libfprint-driver

sudo ./scripts/install.sh          # build + install, 1-3 min the first time
sudo ./scripts/doctor.sh           # 8-point self-check — run this first whenever something is off
sudo ./tools/fp-enroll.sh right-index-finger "Right index"   # enroll; a desktop prompt asks for a press
```

Lock the screen (`Super+L`) and unlock it with the finger you just enrolled. `fprintd-verify` works too.

**To let fingerprints handle sign-in / lock screen / sudo as well:**

```bash
sudo ./scripts/install.sh --with-pam
```

What the installer does: install dependencies → build against the bundled libfprint 1.94.100 source →  
register the driver → install into `/usr/local` (**without touching your distribution's libfprint  
files**, so system upgrades cannot revert it) → restart `fprintd`.

### 2.3 Uninstall

```bash
sudo ./scripts/uninstall.sh            # remove; enrolled fingerprints are kept
sudo ./scripts/uninstall.sh --purge    # also delete the build directory
```

### 2.4 Linux troubleshooting

| Symptom                                                | Fix                                                                                                                                                                                                                                                 |
| ------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `fprintd` reports no device                            | Does `lsusb` list `28e9:5159`? If not, it is hardware/passthrough. If yes, run `sudo ./scripts/doctor.sh` — the usual cause is the shared library not being picked up (the `libfprint-2` entry in `ldconfig -p` should point at `/usr/local` first) |
| Same finger works intermittently                       | Small sensor: press flat and stay still, then re-enroll. Also make sure the chip holds exactly one template (`sudo ./tools/fp-list.sh`)                                                                                                             |
| Enrolled a second finger and the first stopped working | Firmware limitation, see §3                                                                                                                                                                                                                         |
| Fingerprint files keep disappearing                    | Bug in earlier versions; the current release aligns print ids with chip template ids                                                                                                                                                                |

Detailed troubleshooting and protocol notes: `moc5159-libfprint-driver/docs/en/`.

---

## 3. How the two platforms relate (important)

**Templates live in the module's flash chip, not on the computer.** The Windows and Linux drivers read  
and write the **same on-chip database**, therefore:

- A finger enrolled under Windows is **usually recognised** after installing the Linux driver (and vice versa);
- But the per-OS user binding is separate (Windows uses `WinBioDatabase`, Linux uses `/var/lib/fprint/`),  
  so after reinstalling an OS you still go through "add fingerprint" once — even if a template is already on the chip;
- **To change fingers you must wipe first, then enroll.** The firmware compares only the first record in  
  the database:
  ```bash
  # Linux
  sudo ./tools/fp-wipe.sh
  # Windows: Settings → Accounts → Sign-in options → Fingerprint → remove all enrolled fingers
  ```
  With two or more templates stored, only the **oldest** one ever matches; later ones never do. This is  
  firmware behaviour, not a driver bug — verified against USB captures of the official Windows driver  
  (see `moc5159-libfprint-driver/docs/en/04-protocol-notes.md`).

**One module, one machine.** A desktop and a laptop each need their own module.

---

## 4. More

- Enclosure, pinout, 3D models and dimension drawings: [Hardware/](../Hardware/) in this repository;
- Linux driver source, full documentation and changelog: `moc5159-libfprint-driver/`;
- Driver versions: Windows `4.0.105.5` (2025-02-24), Linux `1.0.0`.
