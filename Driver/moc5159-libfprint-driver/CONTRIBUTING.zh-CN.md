[English](CONTRIBUTING.md) · **简体中文**

# 参与改进

欢迎提 issue 和 PR。这个驱动是逆向出来的，**任何一台实机上的验证结果都有价值**。

## 报问题前

先跑自检，把输出贴进 issue：

```bash
sudo ./scripts/doctor.sh
```

再补这几条（不同问题需要的不一样，有就贴）：

```bash
lsusb | grep -i 28e9                    # 设备在不在
lsusb -d 28e9:5159 -v | head -25        # 描述符 / 端点
fprintd-list $USER                      # fprintd 看到了什么
journalctl -u fprintd -b --no-pager | tail -50
dmesg | tail -30
uname -a; cat /etc/os-release | head -3
```

能复现的话，说明**具体步骤**和**期望 vs 实际**。涉及匹配问题的，说清是第几次登记、
库里有几条模板（`sudo ./tools/fp-list.sh` 会打印芯片侧记录数）。

## 代码提交

- 驱动代码在 `src/`，风格跟随 libfprint（GNU 风格缩进、`g_autoptr` 等）；
- 改完至少编过一次：

  ```bash
  # vendor/libfprint-1.94.100.tar.gz 已随仓库提供，不需要再取
  sudo ./scripts/install.sh --drivers moc5159
  sudo ./scripts/doctor.sh
  ```

- 改了协议相关逻辑，请在 PR 描述里附上抓包或实机日志（USBPcap / usbmon 都行），
  并说明是哪台机器、哪个固件版本；
- **不要提交 `vendor/` 下解压出来的 libfprint 源码** —— 那里只该有锁定版本
  `vendor/libfprint-1.94.100.tar.gz` 这一个文件，且版本必须保持 1.94.100（见 `.gitignore`）；
- 不要提交 `build/` 下的产物。

## 关于逆向资料

`docs/zh-CN/04-protocol-notes.md` 里记的是**已经验证过**的部分。如果你想补充新命令的结论，
请连证据一起给：

1. 触发条件（发了什么命令、设备处于什么状态）；
2. 完整应答（128 字节十六进制，别截断）；
3. 对照组（不同条件下同一命令的应答差异）。

只有现象没有对照的结论，很容易变成下一个人踩的坑。

## 已知的"这不是 bug"

以下都是固件行为，PR 改驱动解决不了（详见 `docs/zh-CN/04-protocol-notes.md` §6）：

- 登记第二根手指不生效 —— 固件只比对库内第一条记录；
- `0x30` 恒为 `0x00` —— 不能用来判断手指按下；
- 建第二个模板库会让设备失去响应，只能拔插。

## 许可

提交的代码默认以 **LGPL-2.1-or-later** 授权，与项目一致。
