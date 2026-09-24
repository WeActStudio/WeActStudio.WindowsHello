Fingerprint Module Manual

[简体中文](./README-ZH.md)

---

---

<img title="" src="file:///G:/WeAct_Git/WeActStudio.WindowsHello/Images/WindowsHelloPackage1.png" alt="  " data-align="center">

###### Finished Version with Case  --  WindowsHelloPackage1

Complete finished device equipped with high-quality non-slip ABS shell and dedicated USB cable. Plug and play, dustproof and drop-resistant with exquisite workmanship. Perfect for daily desktop use and portable carrying. No extra accessories needed, ready to use out of the box, ideal for ordinary users.

---

---

![   ](G:\WeAct_Git\WeActStudio.WindowsHello\Images\WindowsHelloPackage2.png)

###### Bare Board DIY Version  -- WindowsHelloPackage2

Pure circuit board design without shell and cable, ultra-small size. Suitable for DIY modification, embedded development, equipment integration, industrial control equipment renovation and built-in chassis modification. Extremely cost-effective, 
meeting the customization needs of technical enthusiasts and professional developers.

---

---

### 📌 Overview

The WeActStudio Windows Hello Fingerprint Module is a **Microsoft officially certified native USB biometric device** that adopts the standard HID protocol and driver-free design. After connecting to a computer, the Windows system can automatically match, download and install official biometric drivers without any third-party driver tools. It is plug-and-play and fully compatible with Windows Hello fingerprint unlock and system login functions.

### ✅ Supported System

Windows 10 / Windows 11 (x64)

*Not compatible with Android, macOS and Linux systems temporarily*

### 🚀 Usage Guide

1. **Hardware Connection** Connect the module USB interface directly to the native USB-A port of the computer. It is not recommended to use hubs or extension cables to avoid recognition exceptions caused by insufficient power supply.

2. **Automatic Driver Installation** With network access, the system will automatically download and install the biometric driver. Wait 10–300 seconds until the system prompts that the device is ready in the lower-right corner.

3. **Driver Verification** Go to Device Manager > Biometric devices. The driver installation succeeds if the device is displayed normally without yellow exclamation marks.

4. **Fingerprint Enrollment** Go to Settings > Accounts > Sign-in options > Fingerprint recognition (Windows Hello), follow the guide to complete enrollment, and you can use fingerprint to unlock and log in to the system.

### 🔧 Troubleshooting

- **No response after plugging in**: Switch to the native USB port, enable Windows Update, or restart the computer and try again

- **Driver abnormality / yellow exclamation mark**: Uninstall the abnormal device and its driver, restart the computer for automatic reinstallation; or manually install the offline driver`Driver/MOC-release-signed-4.0.105.5`

- **Windows Hello device not recognized**: Update the system, restart the Windows Biometric Service, clear old fingerprint data and re-enroll fingerprints

### 💡 Notes

- Network connection is recommended for the first use to ensure the system can normally obtain official drivers

- Avoid frequent hot-swapping to prevent sensor damage caused by instantaneous current

- Do not use third-party driver tools to avoid functional failure caused by protocol conflicts

### ✨ Features

- **Native Driver-Free**: Natively supported by Windows system with fully automatic driver deployment

- **Safe and Reliable**: Complies with Microsoft standard biometric protocols with system-level security encryption and authentication

- **Ultra Easy to Use**: Plug and play without configuration, ready to use out of the box
