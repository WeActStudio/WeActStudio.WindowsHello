### WeActStudio Windows Hello Fingerprint Module Driver Installation Guide

**Supported Systems**: Windows 10 / Windows 11 **Features**: Standard USB Windows Hello fingerprint module, supports native automatic driver installation and plug-and-play. No third-party driver software required.

---

## 📌 Introduction

This fingerprint module is a **Windows Hello native-compatible biometric device** that complies with Microsoft biometric protocol standards. After connecting via USB, the system can automatically recognize, match, and install official drivers without manual downloading, extraction or installation. Ready to use out of the box for Windows fingerprint unlock and system login verification.

---

## ✅ Automatic Driver Installation Steps

### 1. Hardware Connection

Plug the fingerprint module USB cable directly into the computer’s native USB-A port. It is recommended to use the built-in front or rear USB ports of the computer to avoid recognition failures caused by insufficient power from USB hubs or extension cables.

### 2. Automatic Driver Installation

After inserting the device, Windows will automatically search, download and install the **official biometric device driver** online.
System pop-up prompts: `Installing device driver software` → `Your device is ready to use`.

No manual operation is required. Please wait 10–300 seconds for the driver installation to complete.

### 3. Verify Driver Installation

Right-click the Start Menu → **Device Manager** → Expand **Biometric devices**.
If the fingerprint sensor appears normally without yellow exclamation marks or unknown devices, the driver is installed successfully.

### 4. Enable Windows Hello Fingerprint Login

Settings → Accounts → Sign-in options → Select **Fingerprint recognition (Windows Hello)**. Follow the system prompts to complete fingerprint enrollment. You can then use fingerprint unlock and system login.

---

## 🔧 Troubleshooting

### Problem 1: No response after plugging in, no automatic driver installation

- Use the computer’s native USB port instead of hubs or extension cables

- Run Windows Update to install optional system driver patches

- Restart the computer and reconnect the device

### Problem 2: Yellow exclamation mark / driver error in Device Manager

- Right-click the abnormal device →**Uninstall device**

- Check "Delete the driver software for this device" (if available)

- Restart the computer, Windows will automatically reinstall the official driver

- Or manually install the offline driver: **MOC-release-signed-4.0.105.5**

### Problem 3: System prompts “No compatible Windows Hello fingerprint device found”

- Update Windows system to the latest version

- Restart the biometric service: Press Win+R, enter `services.msc`, restart **Windows Biometric Service**

- Delete existing fingerprint data and re-enroll your fingerprint

---

## 💡 Notes

- This module **only supports Windows native Windows Hello**, not compatible with Android, MacOS or Linux systems

- No third-party driver tools are required to avoid incompatibility and functional failure

- Internet access is recommended for the first use to ensure Windows can download official biometric drivers

- Avoid frequent hot-swapping to prevent instantaneous current damage to the module

---

## 📄 Features Summary

- **Plug-and-Play**: Automatic system driver deployment via USB

- **Native Windows Support**: Standard Windows Hello protocol with system-level security authentication

- **Stable & Reliable**: Pure official drivers without third-party bundling
