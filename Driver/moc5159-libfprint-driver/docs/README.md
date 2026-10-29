**English** · [简体中文](#zh-cn)

# Documentation

Everything is available in English and Simplified Chinese. Each page carries a language switch
at the very top.

| # | Document | English | 简体中文 | What it covers |
|---|---|---|---|---|
| 01 | Installation | [en/01-install.md](en/01-install.md) | [zh-CN/01-install.md](zh-CN/01-install.md) | Prerequisites, one-shot install, install modes, post-install verification, manual build, upgrade, uninstall, offline bundles, distro packaging, test record |
| 02 | Usage | [en/02-usage.md](en/02-usage.md) | [zh-CN/02-usage.md](zh-CN/02-usage.md) | Enrollment, pressing technique, verification, status, PAM (login / lock screen / sudo), the multi-finger limit, wiping, logs, data locations, automation, cheat sheet |
| 03 | Troubleshooting | [en/03-troubleshooting.md](en/03-troubleshooting.md) | [zh-CN/03-troubleshooting.md](zh-CN/03-troubleshooting.md) | Look up by symptom: device layer, driver layer, capture/matching layer, PAM/login layer, Windows coexistence, what to collect for a report |
| 04 | Protocol notes | [en/04-protocol-notes.md](en/04-protocol-notes.md) | [zh-CN/04-protocol-notes.md](zh-CN/04-protocol-notes.md) | Frame format, command table, 80-byte record layout, real timing, evidence chain for the one-finger limit, C1 verdict logic |

Outside this directory:

| Document | English | 简体中文 |
|---|---|---|
| Project overview | [../README.md](../README.md) | [../README.zh-CN.md](../README.zh-CN.md) |
| Changelog | [../CHANGELOG.md](../CHANGELOG.md) | [../CHANGELOG.zh-CN.md](../CHANGELOG.zh-CN.md) |
| Contributing | [../CONTRIBUTING.md](../CONTRIBUTING.md) | [../CONTRIBUTING.zh-CN.md](../CONTRIBUTING.zh-CN.md) |
| vendor/ note | [../vendor/README.md](../vendor/README.md) | [../vendor/README.zh-CN.md](../vendor/README.zh-CN.md) |

## Reading order

- **Just want it working**: [01 Installation](en/01-install.md) → [02 Usage](en/02-usage.md) §1-§4.
- **Fingerprint login doesn't prompt / behaves oddly**: [02 Usage](en/02-usage.md) §5-§6.
- **A second finger doesn't work**: that is firmware behaviour, see
  [02 Usage](en/02-usage.md) §7 and [04 Protocol notes](en/04-protocol-notes.md) §6.
- **Something is broken**: run `sudo ./scripts/doctor.sh`, then
  [03 Troubleshooting](en/03-troubleshooting.md).
- **Want to modify the driver**: [04 Protocol notes](en/04-protocol-notes.md) first.

---

<a id="zh-cn"></a>

## 简体中文

所有文档都有英文和简体中文两个版本，每页顶部都有语言切换链接。

| # | 文档 | English | 简体中文 | 内容 |
|---|---|---|---|---|
| 01 | 安装 | [en/01-install.md](en/01-install.md) | [zh-CN/01-install.md](zh-CN/01-install.md) | 前置条件、一键安装、两种安装模式、安装后验证、手动编译、升级、卸载、离线包、打发行版包、实测记录 |
| 02 | 使用 | [en/02-usage.md](en/02-usage.md) | [zh-CN/02-usage.md](zh-CN/02-usage.md) | 登记、按压要点、验证、查看状态、PAM（登录/锁屏/sudo）、多手指限制、清空、日志、数据位置、自动化、速查表 |
| 03 | 排错 | [en/03-troubleshooting.md](en/03-troubleshooting.md) | [zh-CN/03-troubleshooting.md](zh-CN/03-troubleshooting.md) | 按现象查：设备层、驱动层、采集/匹配层、PAM/登录层、与 Windows 共存、报问题要收集什么 |
| 04 | 协议 | [en/04-protocol-notes.md](en/04-protocol-notes.md) | [zh-CN/04-protocol-notes.md](zh-CN/04-protocol-notes.md) | 帧格式、命令表、80 字节记录布局、真实时序、"只能一根手指"的证据链、C1 判定逻辑 |

本目录以外的文档：

| 文档 | English | 简体中文 |
|---|---|---|
| 项目主页 | [../README.md](../README.md) | [../README.zh-CN.md](../README.zh-CN.md) |
| 更新记录 | [../CHANGELOG.md](../CHANGELOG.md) | [../CHANGELOG.zh-CN.md](../CHANGELOG.zh-CN.md) |
| 参与改进 | [../CONTRIBUTING.md](../CONTRIBUTING.md) | [../CONTRIBUTING.zh-CN.md](../CONTRIBUTING.zh-CN.md) |
| vendor/ 说明 | [../vendor/README.md](../vendor/README.md) | [../vendor/README.zh-CN.md](../vendor/README.zh-CN.md) |

### 阅读顺序

- **只想装起来用**：[01 安装](zh-CN/01-install.md) → [02 使用](zh-CN/02-usage.md) 第 1~4 节。
- **指纹登录不弹提示 / 行为古怪**：[02 使用](zh-CN/02-usage.md) 第 5~6 节。
- **第二根手指不生效**：固件行为，见 [02 使用](zh-CN/02-usage.md) 第 7 节与
  [04 协议](zh-CN/04-protocol-notes.md) 第 6 节。
- **出问题了**：先跑 `sudo ./scripts/doctor.sh`，再看 [03 排错](zh-CN/03-troubleshooting.md)。
- **想改驱动**：先读 [04 协议](zh-CN/04-protocol-notes.md)。
