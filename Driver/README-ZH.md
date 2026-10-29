# 驱动使用说明

[English](./README-EN.md) · **简体中文**

这个指纹模块在 Windows 和 Linux 上用的是**两套完全不同的驱动**，按你用的系统选一节看就行：

| 系统                       | 用什么驱动                                    | 文件                                 | 大概要做什么                         |
| ------------------------ | ---------------------------------------- | ---------------------------------- | ------------------------------ |
| **Windows 10 / 11（x64）** | Petaic 官方 WBDI 驱动（微软 WHQL 签名，v4.0.105.5） | `MOC-release-signed-4.0.105.5.zip` | 插上基本会自动装；没自动装就手动指一下 INF，约 1 分钟 |
| **Linux**                | 开源 libfprint 驱动 `moc5159`（含源码 + 安装脚本）    | `moc5159-libfprint-driver/`        | 一条命令编译安装，约 1~3 分钟              |

> **硬件要求**：USB 2.0 设备，ID 为 `28e9:5159`。请插在主板背面 / 机箱后面的**原生 USB-A 口**上，  
> 不要用 USB Hub、延长线或前置面板口 —— 供电不足会导致"插上没反应"或识别时好时坏。

---

## 一、Windows 安装

### 1.1 首选：联网让它自动装

1. 把模块插到原生 USB 口；
2. 确保电脑联网，等 **10 秒 ~ 5 分钟**（Windows Update 会匹配到官方驱动并自动安装）；
3. 打开**设备管理器** → 展开 **生物识别设备** → 应出现 `Fingerprint Device`，图标上没有黄色感叹号；
4. **设置 → 账户 → 登录选项 → 指纹识别（Windows Hello）** → 点"添加"→ 按提示反复按压传感器，完成录入。

装完就能用指纹解锁屏幕、登录系统和确认 UAC 弹窗了。

### 1.2 没自动装：手动安装（离线可用）

解压 `MOC-release-signed-4.0.105.5.zip`，得到 `GD-MOC-release-signed-4.0.105.5` 文件夹，里面 5 个文件。  
下面两种方式**任选一种**：

**方式 A：命令行（推荐，Windows 10 1809 及以上）**

用**管理员身份**打开 PowerShell，进到解压出来的 `GD-MOC-release-signed-4.0.105.5` 文件夹执行  
（路径换成你自己的）：

```powershell
cd "C:\path\to\GD-MOC-release-signed-4.0.105.5"
pnputil /add-driver .\PetaicTouchFP-MOC-5159.inf /install
```

看到"已成功添加驱动程序包"之类提示即成功。然后重新拔插一次模块，或执行：

```powershell
pnputil /scan-devices
```

**方式 B：设备管理器点选**

1. 右键"开始"→ **设备管理器**；
2. 在"其他设备"或"生物识别设备"里找到这个模块 —— 名字可能是 `Fingerprint Device`、未知设备，  
   或者带黄色感叹号；不确定的话看它的**硬件 ID** 是否为 `USB\VID_28E9&PID_5159`；
3. 右键 → **更新驱动程序** → **浏览我的电脑以查找驱动程序** → 选中解压出来的  
   `GD-MOC-release-signed-4.0.105.5` 文件夹（就是有 `.inf` 文件的那一层）→ 下一步；
4. 弹出签名确认时选"安装"。

> ⚠️ **不要右键 INF 文件选"安装"。** 这是 PnP 驱动，INF 里没有 `DefaultInstall` 段，  
> 右键安装只会得到"所选 INF 文件不支持这种安装方法"的提示。必须走上面两种方式之一。

### 1.3 确认装好了（三条任一即可）

```powershell
Get-PnpDevice -Class Biometric        # Status 应为 OK
Get-Service PetaicSrv                 # 应为 Running，这是引擎服务，停了就录不了指纹
```

或者直接看 **设置 → 账户 → 登录选项**，"指纹识别"那一项能点进去并开始添加手指。

### 1.4 卸载 / 重装

```powershell
# 先看装的是哪个包（记下 oemXX.inf 这个名字）
pnputil /enum-drivers | Select-String -Pattern "Petaic" -Context 3,3

# 卸载（把 oem12.inf 换成上面查到的名字）
pnputil /delete-driver oem12.inf /uninstall /force
```

也可以在设备管理器里右键设备 → **卸载设备** → 勾选"尝试删除此设备的驱动程序"→ 确定，然后重启。

> 已经录好的指纹保存在**模块芯片内部**，卸载重装驱动**不会**丢；但重装后如果系统侧看不到指纹，  
> 需要重新录入（Windows 的用户绑定在 `WinBioDatabase` 里）。

### 1.5 压缩包里是什么

| 文件                           | 作用                                         | 装到哪里                                 |
| ---------------------------- | ------------------------------------------ | ------------------------------------ |
| `PetaicTouchFP-MOC-5159.inf` | 安装描述文件，按硬件 ID `USB\VID_28E9&PID_5159` 匹配设备 | 驱动库（`DriverStore`）                   |
| `petaictouchfp.cat`          | 微软签名目录，校验下面三个文件没被改过                        | 同驱动库                                 |
| `PetaicTouchFP.dll`          | UMDF 用户态驱动，负责和模块的 USB 私有协议通信               | `C:\Windows\System32\drivers\UMDF\`  |
| `PetaicTouchFPEngine.dll`    | Windows 生物识别引擎适配器（传感器 / 引擎 / 存储三合一）        | `C:\Windows\System32\WinBioPlugins\` |
| `PetaicSrv.exe`              | 生物识别服务进程，自动启动                              | `C:\Windows\System32\PetaicSrv.exe`  |

四个可执行文件都是 **Microsoft Windows Hardware Compatibility Publisher** 的有效签名，  
所以 Windows 10/11 不会拦截，也能被 Windows Update 自动匹配。

### 1.6 Windows 常见问题

| 症状                     | 原因                        | 处理                                                                          |
| ---------------------- | ------------------------- | --------------------------------------------------------------------------- |
| 插上完全没反应，设备管理器里什么都没有    | 供电不足 / 用了 Hub 或前置口 / 线材问题 | 换主板后面的原生 USB-A 口，换线，重启再插                                                    |
| 设备管理器里有黄色感叹号           | 驱动没装上或版本不匹配               | 按 §1.2 手动装；装完仍不行就先卸载旧驱动再装                                                   |
| `pnputil` 报"无法添加驱动程序包" | 没管理员权限 / 文件被杀软删了          | 用管理员 PowerShell；检查解压目录是否完整（5 个文件）                                           |
| 设备正常但没有"指纹"选项          | 生物识别服务被禁用，或组策略关掉了         | `Get-Service WbioSrvc`，不是 Running 就 `net start WbioSrvc`；企业机检查组策略"允许使用生物识别" |
| 录入到一半失败                | 传感器面积小（GSL6150），手指没压满     | 指腹**压满整个传感器窗口**并停留到提示抬起；指腹干燥时先哈一口气；同一根手指重录                                  |
| 装好后指纹用不了，或换手指无效        | 芯片里已有旧模板（固件只比对第一条记录，见 §3） | 设置里删掉所有指纹，重启，重新录入**唯一**一根手指                                                 |
| 换到 ARM64 笔记本           | 官方包只有 x64 驱动              |                                                                             |

---

## 二、Linux 安装

Linux 上游没有这个模块的支持，`moc5159-libfprint-driver/` 是逆向官方 Windows 驱动后写的  
libfprint 驱动，装上后 `fprintd` 全套可用（命令行、GNOME 设置面板、`pam_fprintd`）。

### 2.1 支持范围

| 项目  | 要求                                                                                           |
| --- | -------------------------------------------------------------------------------------------- |
| 架构  | x86_64 / aarch64                                                                             |
| 发行版 | 已实测 **Ubuntu 26.04** + fprintd 1.94.5；理论可用 Debian 12+、Ubuntu 22.04+、Fedora 39+、Arch、openSUSE |
| 依赖  | 脚本会自动装（需要 sudo 和联网；源码包已自带，编译本身不需要联网）                                                         |
| 虚拟机 | 需要先把 USB 设备直通给客户机（VMware：虚拟机 → 可移动设备 → 连接）                                                   |

### 2.2 安装（三条命令）

```bash
cd moc5159-libfprint-driver

sudo ./scripts/install.sh          # 编译 + 安装，首次 1~3 分钟
sudo ./scripts/doctor.sh           # 8 项自检，出任何问题先跑这个
sudo ./tools/fp-enroll.sh right-index-finger 右手食指   # 录指纹，桌面会弹按压提示
```

装完锁屏（`Super+L`）直接用刚录的手指解锁；也可以随时 `fprintd-verify` 验证。

**想把指纹用到登录 / 锁屏 / sudo 上：**

```bash
sudo ./scripts/install.sh --with-pam
```

安装脚本做的事：自动装构建依赖 → 用自带的 libfprint 1.94.100 源码编译 → 注册驱动 →  
装到 `/usr/local`（**不动发行版自带的 libfprint 文件**，系统升级也不会被打回原形）→ 重启 `fprintd`。

### 2.3 卸载

```bash
sudo ./scripts/uninstall.sh            # 卸载，已录指纹保留
sudo ./scripts/uninstall.sh --purge    # 连构建目录一起清掉
```

### 2.4 Linux 常见问题

| 症状               | 处理                                                                                                                                          |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `fprintd` 说找不到设备 | `lsusb` 里有没有 `28e9:5159`？没有就是硬件/直通问题；有的话跑 `sudo ./scripts/doctor.sh`，常见原因是动态库没生效（`ldconfig -p` 输出的 `libfprint-2` 那一行应该是 `/usr/local` 排在最前面） |
| 同一根手指时好时坏        | 传感器小，指腹要压满并保持不动；重新录入一次。另外确认芯片库里只有 1 条模板（`sudo ./tools/fp-list.sh`）                                                                          |
| 录了第二根手指，第一根反而失效  | 固件限制，见 §3                                                                                                                                   |
| 指纹文件总是被删         | 老版本的 bug，升级到当前版本即可（print id 与芯片模板号已对齐）                                                                                                      |

更细的排错和协议细节见 `moc5159-libfprint-driver/docs/zh-CN/`。

---

## 三、两个平台之间的关系（重要）

**指纹模板存在模块芯片里，不在电脑里。** Windows 和 Linux 驱动读写的是**同一个芯片模板库**，  
所以：

- 在 Windows 里录过的手指，装好 Linux 驱动后**一般可以直接识别**（反之亦然）；
- 但系统侧的用户绑定是各自独立的（Windows 用 `WinBioDatabase`，Linux 用 `/var/lib/fprint/`），  
  重装系统需要在系统里重新走一遍"添加指纹"流程 —— 尽管模板有时已经躺在芯片里了；
- **要换手指，必须先清空，再录新的**。原因是这颗芯片的固件在验证时只比对库里第一条记录：


  ```bash
  # Linux
  sudo ./tools/fp-wipe.sh
  # Windows：设置 → 账户 → 登录选项 → 指纹识别 → 删除所有已录入的指纹
  ```
  库里有 2 条及以上时，只有**最早**那条生效，后录的永远匹配不上。这是固件行为，不是驱动 bug ——  
  我们抓过官方 Windows 驱动的 USB 流量验证过这一点（详见 `moc5159-libfprint-driver/docs/zh-CN/04-protocol-notes.md`）。

**一个模块只能插在一台机器上。** 想让台式机和笔记本都用指纹，需要各配一个模块。

---

## 四、其他

- 模块外观、引脚定义、3D 模型、尺寸图见仓库 [Hardware/](../Hardware/)；
- Linux 驱动源码、完整文档、更新记录见 `moc5159-libfprint-driver/`；
- 驱动版本：Windows `4.0.105.5`（2025-02-24），Linux `1.0.0`。
