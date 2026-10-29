[English](README.md) · **简体中文**

# moc5159-libfprint-driver

给 **Petaic MOC-5159** 指纹模组（`28e9:5159`）写的 Linux 驱动，基于 libfprint，
装好后 `fprintd` 全套可用：命令行、GNOME 设置面板、`pam_fprintd`（登录 / 锁屏 / sudo 免密）。

> **English**: A libfprint driver for the Petaic MOC-5159 fingerprint module
> (`USB 28e9:5159`, GD32W515 + GSL6150, match-on-chip). This module ships with
> Windows Hello on some laptops/mini-PCs and has **no Linux support upstream**.
> The driver speaks the module's private 128-byte USB protocol, reverse-engineered
> from the signed Windows driver. Tested on Ubuntu 26.04 with fprintd 1.94.5.
> **Firmware limitation: only the first template in the on-chip database is ever
> compared — enroll exactly one finger.**

## 这是什么设备

| 项 | 值 |
|---|---|
| USB ID | `28e9:5159` |
| `lsusb` 显示 | `ID 28e9:5159 GDMicroelectronics Touch Fingerprint Sensor` |
| 模组 | MOC-5159（Petaic） |
| 芯片 | GD32W515 主控 + Silead GSL6150 传感器 |
| 工作方式 | **match-on-chip**：模板存在芯片 flash 里，比对也在芯片内部完成，指纹图像不进系统内存 |
| 原始环境 | Windows Hello 认证模组（Windows 驱动包 `GD-MOC-release-signed-4.0.105.5`） |

装好驱动后系统里显示的名字是 `Petaic MOC-5159 (GD32W515) Touch Fingerprint Sensor`
（`lsusb` 那一行的厂商名 `GDMicroelectronics` 来自 USB ID 数据库，与驱动无关）。

## ⚠️ 先看这条：只能登记一根手指

**这颗芯片的固件在验证时只比对模板库里的第一条记录（index 0）。**

- 库里 1 条模板 → 正常，识别稳定；
- 库里 2 条及以上 → 只有**最早**那条生效，后登记的指纹永远不匹配。

这是固件行为，不是驱动 bug —— 抓过 Windows 官方驱动的 USB 流量、穷举过命令码、
单独加载过候选模板，结论一致；Windows 官方驱动在这颗设备上也从未跑过多指。

要换手指，先清空再录：

```bash
sudo ./tools/fp-wipe.sh                          # 清空芯片库 + 本机 fprintd 存储
sudo ./tools/fp-enroll.sh left-thumb 左手拇指     # 换成你想用的那根
```

## 功能

- `fprintd-enroll` / `fprintd-verify` / `fprintd-list` / `fprintd-delete` 命令行；
- GNOME「设置 → 用户 → 指纹」面板登记与删除；
- `pam_fprintd`：登录、锁屏解锁、`sudo` 免密（支持错误 5 次再回落到密码）；
- 已登记指纹存在**芯片内部** + `/var/lib/fprint/<用户>/moc5159/`，重装驱动不丢；
- 附带独立诊断工具 `moc5159-probe`（只依赖 libusb，不经过 fprintd，用来区分"驱动问题"和"设备问题"）。

## 快速开始

```bash
git clone https://github.com/<你的用户名>/moc5159-libfprint-driver.git
cd moc5159-libfprint-driver

sudo ./scripts/install.sh                       # 编译 + 安装，首次 1~3 分钟
sudo ./scripts/doctor.sh                        # 8 项自检，出问题先看这个
sudo ./tools/fp-enroll.sh right-index-finger 右手食指   # 录指纹（桌面会弹按压提示）

fprintd-verify                                   # 验证
```

装完锁屏（`Super+L`）直接用刚录的手指解锁。

**想让指纹也接管登录 / 锁屏 / sudo：**

```bash
sudo ./scripts/install.sh --with-pam
```

Debian/Ubuntu 走 `pam-auth-update`，其它发行版脚本会打印手动配置步骤。
PAM 里 `max-tries=5`：连续错 5 次才回落到密码，不会按错一次就强制密码（见 `docs/zh-CN/02-usage.md` §5.2）。

### 安装会做什么

1. 按发行版自动装构建依赖（apt / dnf / yum / zypper / pacman / apk）；
2. 解压仓库自带的 libfprint **1.94.100** 源码（`vendor/libfprint-1.94.100.tar.gz`，12 MB，
   **完全离线可装**；删掉这个包时脚本才回落到 `git clone` 上游）；
3. 把驱动注册进 libfprint 构建系统（libfprint 没有外部插件机制，只能编进库里）；
4. **默认编译 libfprint 全部驱动（281 个设备）**，不会让你机器上别的指纹传感器失效；
   缺 openssl 等依赖时自动回落到只编 `moc5159`；
5. 装到 `/usr/local` 并加一条 `ld.so.conf.d` 让动态链接器优先用它 ——
   **发行版的 `libfprint-2-2` 包一个文件都不动**，系统升级也不会被打回原形；
6. 重启 `fprintd`，可选启用 PAM。

### 两种安装模式

| | `local`（默认） | `--system` |
|---|---|---|
| 装到 | `/usr/local` | `/usr` |
| 发行版 libfprint 文件 | **不动** | 覆盖（先完整备份到 `/var/lib/moc5159-driver/backup/`） |
| 生效方式 | `ld.so.conf.d/00-moc5159-libfprint.conf` 优先 | 直接替换 |
| 卸载 | 删文件即可，发行版立刻恢复 | 从备份还原 |
| 适用 | **推荐所有人** | 想彻底替换 / 做集成测试 |

两种模式都能干净卸载：`sudo ./scripts/uninstall.sh`（加 `--purge` 连构建目录一起删），
已登记的指纹数据不会被删。

## 兼容性

| 项目 | 要求 |
|---|---|
| 架构 | x86_64 / aarch64（纯 C，无汇编） |
| glibc | ≥ 2.28 |
| GLib | ≥ 2.56 |
| libgusb | ≥ 0.2 |
| meson / ninja | meson ≥ 0.62 |
| **已实测** | **Ubuntu 26.04.1**（内核 7.0.0）+ fprintd 1.94.5 + `pam_fprintd`，VMware USB 直通 |
| 理论可用 | Debian 12+、Ubuntu 22.04+、Fedora 39+、Arch、openSUSE Tumbleweed |
| 未实测 | 其它发行版 —— 脚本已做多发行版适配，遇到问题跑 `scripts/doctor.sh` |

驱动按 libfprint **1.94 系列驱动 API** 编写；1.95+ 改过部分接口，所以安装脚本锁定 1.94.100。

> 在**虚拟机**里用需要先把 USB 设备直通给客户机（VMware：虚拟机 → 可移动设备 → 连接）。
> 直通期间宿主机 Windows Hello 会不可用。

## 常用命令

```bash
# 状态
sudo ./scripts/doctor.sh                # 环境自检
./tools/fp-list.sh                      # 已登记指纹（加 sudo 连芯片侧记录一起看）

# 登记 / 验证 / 删除
sudo ./tools/fp-enroll.sh right-index-finger 右手食指
./tools/fp-verify.sh right-index-finger
sudo fprintd-delete $USER -f right-index-finger

# 清空（危险，需输 yes）
sudo ./tools/fp-wipe.sh

# 独立诊断：不经过 fprintd，直接和芯片说话
sudo ./scripts/build-probe.sh
sudo systemctl stop fprintd
sudo ./build/probe info      # 固件版本 / 传感器信息
sudo ./build/probe db        # 库头状态（ev=0x27 正常）
sudo ./build/probe list      # 列出芯片内模板
sudo systemctl start fprintd
```

`build/probe` 的完整子命令：`info`、`db`、`list`、`dump`、`raw`、`seq`、`ident`、`match`。
它**不需要**驱动和 fprintd，插上就能用 —— 排查"到底是驱动问题还是硬件/线缆问题"时先跑它。

## 没有硬件也能跑（模拟器）

`tools/sim/` 里有一个软件版的 MOC-5159：固定帧协议、芯片模板库、`0xC1` 命中判定全都按真机
行为实现，再用 `LD_PRELOAD` 把 libusb 换掉。**驱动源码一行都不用改**，libfprint / fprintd
看到的还是同一份 `libfprint`，只是设备换成了假的 —— 适合在没有模块的机器上做回归。

```bash
cd tools/sim
./build.sh                 # libmocsim.so + 两个测试程序
./run-tests.sh             # 完整回归矩阵（11 个场景）

LD_PRELOAD=./build/libmocsim.so ./build/mocsim-e2e                  # 命中
LD_PRELOAD=./build/libmocsim.so MOCSIM_MATCH=0 ./build/mocsim-e2e   # 不命中
LD_PRELOAD=./build/libmocsim.so MOCSIM_TEMPLATES=0 ./build/mocsim-e2e  # 空库，走完整登记
```

还能注入故障（`MOCSIM_FAULT=stale,notail,quality,c1timeout,badcrc,gone`）来验证驱动的
重试和报错路径。用法、环境变量、实测结果见 [`tools/sim/README.zh-CN.md`](tools/sim/README.zh-CN.md)。

## 目录结构

```
moc5159-libfprint-driver/
├── README.md                  英文版（GitHub 默认）
├── README.zh-CN.md            本文件（简体中文）
├── CHANGELOG.md               更新记录（英）
├── CHANGELOG.zh-CN.md         更新记录（中）
├── CONTRIBUTING.md            报问题要带什么、怎么提 PR（英）
├── CONTRIBUTING.zh-CN.md      同上（中）
├── LICENSE                    LGPL-2.1
├── VERSION
├── docs/
│   ├── README.md              文档索引（中英对照）
│   ├── en/                    英文文档
│   │   ├── 01-install.md          详细安装 / 升级 / 卸载 / 手动编译 / 打原生包
│   │   ├── 02-usage.md            登记、验证、锁屏、PAM、sudo、多手指限制
│   │   ├── 03-troubleshooting.md  排错手册（按现象查：设备层/驱动层/匹配层/PAM 层）
│   │   └── 04-protocol-notes.md   私有协议要点与"为什么只能一根手指"的技术依据
│   └── zh-CN/                 同样四篇的中文版
├── src/
│   ├── moc5159.c              libfprint 驱动本体（FpDevice 子类 + SSM 状态机）
│   ├── moc5159.h              设备/协议常量
│   ├── moc5159-proto.c        128 字节定长帧组帧/解析（不依赖 libfprint）
│   └── moc5159-proto.h
├── scripts/
│   ├── install.sh             一键安装（多发行版）
│   ├── uninstall.sh           卸载 / 还原
│   ├── integrate.sh           把驱动注册进任意 libfprint 源码树
│   ├── fetch-libfprint.sh     重新下载 libfprint 1.94.100 到 vendor/（只在包被删时用）
│   ├── build-probe.sh         编译独立诊断工具
│   └── doctor.sh              环境自检
├── tools/
│   ├── moc5159-probe.c        独立诊断工具（只依赖 libusb）
│   ├── fp-enroll.sh           登记（桌面按压提示，完事自动关窗）
│   ├── fp-verify.sh           验证
│   ├── fp-list.sh             查看 fprintd + 芯片库状态
│   ├── fp-wipe.sh             清空
│   ├── fp-prompt.sh           提示框公共函数
│   └── sim/                   模块模拟器 —— 不插硬件也能跑驱动
│       ├── mocsim-dev.c       协议状态机 + 芯片模板库模型
│       ├── mocsim-shim.c      用 LD_PRELOAD 顶掉 libusb（编出 libmocsim.so）
│       ├── mocsim-selftest.c  协议层自检
│       ├── mocsim-e2e.c       libfprint 端到端测试
│       ├── build.sh           构建
│       ├── run-tests.sh       回归矩阵（11 个场景）
│       └── README.md / README.zh-CN.md
└── vendor/
    ├── libfprint-1.94.100.tar.gz  上游 libfprint 源码，版本锁定（12 MB，LGPL-2.1+）
    ├── README.md              这个包是什么、安装脚本怎么用它（英）
    └── README.zh-CN.md        同上（中）
```

## 工作原理（简述）

模组只暴露两个 bulk 端点（`0x01` OUT / `0x81` IN，64 字节），没有中断端点。
所有命令走 **128 字节定长帧**：`[0]` = payload 长度、`[2]` = 命令码、`[6:]` = payload、
末尾是前 126 字节的累加和。

识别一完整来回约 1.4 秒：

```
DB_SIZE → DB_DUMP_RECORD ×N → VERIFY_CAPTURE(0xC0) → 图像块(0x13) → 提交(0x31)
        → VERIFY_MATCHING(0xC1)
```

三个必须遵守的传输层细节（不照做会表现为"设备坏了"）：

1. **每条命令前 drain 残留 IN 帧** —— 上一轮没被读走的帧会让固件拒绝新的 bulk OUT，
   表现为 OUT 一直 NAK 直到超时；
2. **bulk IN 缓冲必须正好 128 字节** —— 回帧是两个满 64 字节包，没有短包收尾；
3. **不补 ZLP**。

匹配结果看 `0xC1` 应答 payload 内部两块的关系，**不是**看 `ev` 字节（它恒为 `0x01`）。
细节、命令表、80 字节记录布局、以及"只比第一条"的完整证据链都在
[`docs/zh-CN/04-protocol-notes.md`](docs/zh-CN/04-protocol-notes.md)。

## 常见问题

**装完 `fprintd` 说没有设备？**
先 `sudo ./scripts/doctor.sh`，再看 [`docs/zh-CN/03-troubleshooting.md`](docs/zh-CN/03-troubleshooting.md) §B1。
最常见的原因是生效的还不是 `/usr/local` 那份库（`ldconfig -p | grep libfprint-2` 第一行应该指向 `/usr/local`）。

**同一根手指时好时坏？**
GSL6150 是小面积传感器，按压位置和角度影响很大，见 `docs/zh-CN/02-usage.md` §2.4 的按压要点。
先确认库里只有 1 条模板。

**登记第二根手指后第一根也不灵了？**
固件限制，见上文「只能登记一根手指」。

**指纹文件莫名消失？**
`fprintd` 在 Claim 时比对 print id，对不上会静默删掉存储文件。当前版本已修正
（print id 统一取芯片模板号），若仍出现请看 `docs/zh-CN/03-troubleshooting.md` §B3。

**和 Windows 双系统？**
指纹模板在芯片 flash 里，Windows 和 Linux 共用同一份库 —— 但**两边都只认第一条记录**，
且 fprintd 的本地存储（`/var/lib/fprint/`）是 Linux 独有的。

## 不支持 / 不做

- 不写内核模块（纯用户态驱动）；
- 不抓取指纹图像（match-on-chip 拿不到原图）；
- 不支持多手指（固件限制，非驱动偷懒）；
- 不做 Windows Hello 替代方案（Windows 侧用官方驱动）。

## 文档（中英双语，每页顶部可切换）

| | English | 简体中文 |
|---|---|---|
| 文档索引 | [docs/README.md](docs/README.md) | [docs/README.md](docs/README.md) |
| 安装 | [docs/en/01-install.md](docs/en/01-install.md) | [docs/zh-CN/01-install.md](docs/zh-CN/01-install.md) |
| 使用 | [docs/en/02-usage.md](docs/en/02-usage.md) | [docs/zh-CN/02-usage.md](docs/zh-CN/02-usage.md) |
| 排错 | [docs/en/03-troubleshooting.md](docs/en/03-troubleshooting.md) | [docs/zh-CN/03-troubleshooting.md](docs/zh-CN/03-troubleshooting.md) |
| 协议 | [docs/en/04-protocol-notes.md](docs/en/04-protocol-notes.md) | [docs/zh-CN/04-protocol-notes.md](docs/zh-CN/04-protocol-notes.md) |
| 更新记录 | [CHANGELOG.md](CHANGELOG.md) | [CHANGELOG.zh-CN.md](CHANGELOG.zh-CN.md) |
| 参与改进 | [CONTRIBUTING.md](CONTRIBUTING.md) | [CONTRIBUTING.zh-CN.md](CONTRIBUTING.zh-CN.md) |

> 辅助脚本（`install.sh`、`doctor.sh`、`tools/fp-*.sh`）的进度提示输出是中文；
> 命令、参数、退出码以及 libfprint / fprintd 打印的日志不受影响。

## 贡献

见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。任何一台实机上的验证结果都有价值，
尤其是**其它发行版**和**其它固件版本**。

## 许可

驱动与脚本以 **LGPL-2.1-or-later** 发布，与 libfprint 保持一致，见 [`LICENSE`](LICENSE)。

本项目与 Petaic / GigaDevice / Silead 无关，是基于公开可获取的 Windows 驱动包与
USB 流量分析实现的独立驱动，不含厂商代码。

## 致谢

- [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) —— 驱动框架与
  `egismoc` / `goodixmoc` 等 match-on-chip 驱动的实现提供了结构参考；
- [fprintd](https://gitlab.freedesktop.org/libfprint/fprintd)、[usbutils](https://github.com/gregkh/usbutils)、
  [capstone](https://www.capstone-engine.org/) —— 运行时与逆向工具链。
