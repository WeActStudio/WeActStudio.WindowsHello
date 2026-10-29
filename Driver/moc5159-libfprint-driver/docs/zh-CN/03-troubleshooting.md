[English](../en/03-troubleshooting.md) · **简体中文**

# 03 · 排错手册

先跑一遍自检，大部分问题它会直接指出来：

```bash
sudo ./scripts/doctor.sh
```

下面按**现象**查。

---

## A. 设备层

### A1. `lsusb` 里看不到 28e9:5159

| 情况 | 处理 |
|---|---|
| 虚拟机 | USB 设备没直通给客户机。VMware：虚拟机 → 可移动设备 → 找到 GDMicroelectronics → 连接；VirtualBox：设备 → USB → 勾选 |
| 直通过 Windows | 直通后宿主机 Windows Hello 会不可用，属正常 |
| 换过 USB 口 | 换回原来的口，或重新直通一次 |
| 完全没反应 | 拔插一次；仍无 → 硬件/线材问题 |

### A2. 设备在，但驱动打不开（`probe` 报 busy / timeout）

多半是 **USB 通道被挂死**（此前某次异常命令留下的状态），典型表现：
所有 bulk OUT 都超时，`probe info` 的 SETUP 字节从 `0x5C` 变成 `0x00`。

- 先试（能救回"通道 NAK"的情况，救不回"命令把设备搞死"的情况）：
  ```bash
  sudo systemctl stop fprintd
  sudo ./build/probe --clear-halt raw 30
  ```
- 救不回来：**物理拔插一次**（或虚拟机里断开再连接设备）。
  ⚠️ 不要对直通的设备做 USB reset，会让宿主机把它标成 phantom，只能物理拔插。

---

## B. 驱动层

### B1. 装完了但 fprintd 说没有设备（`No device available` / 列表为空）

按顺序查：

```bash
# 1) 编译产物里有没有这台设备
/usr/local/bin/fprint-list-supported-devices 2>/dev/null | grep 5159
# 或者用包内的构建目录
/var/tmp/moc5159-build/build/libfprint/fprint-list-supported-devices | grep 5159

# 2) 动态链接器到底用哪一份 libfprint
ldconfig -p | grep 'libfprint-2\.so\.2'
# 第一行必须是 /usr/local/...；如果是 /usr/lib/... 说明优先级没生效

# 3) fprintd 实际加载的是哪份
sudo grep -o '/[^ ]*libfprint-2\.so\.2[^ ]*' /proc/$(pgrep -x fprintd)/maps | head -1
```

修法：

```bash
find /usr/local -name 'libfprint-2.so.2.0.0'     # 确认装在哪
# 假设是 /usr/local/lib/x86_64-linux-gnu
sudo sh -c 'echo /usr/local/lib/x86_64-linux-gnu > /etc/ld.so.conf.d/00-moc5159-libfprint.conf'
sudo ldconfig
sudo systemctl restart fprintd
```

> 文件名带 `00-` 前缀是为了让它在 `ld.so.conf.d/*.conf` 排序里排第一，
> 这样 `/usr/local` 的库优先于发行版路径。

### B2. 编译失败

| 报错 | 原因 / 处理 |
|---|---|
| `OpenSSL is required for uru4000` | 缺 openssl 开发包。`sudo apt install libssl-dev` / `dnf install openssl-devel`。**或者用 `--drivers moc5159` 跳过其它驱动** |
| `Dependency "gusb" not found` | 缺 libgusb 开发包：`libgusb-dev` / `libgusb-devel` |
| `Dependency "libudev" not found` | `libudev-dev` / `systemd-devel` |
| `Meson version is too old` | 需要 meson ≥ 0.62：`pip3 install --break-system-packages "meson>=1.0"` 或 `pipx install meson` |
| `Foreach expects exactly 2 variables for iterating over objects of type dict` | meson ≥ 1.10 解析上游 `tests/meson.build` 的旧写法报错。`install.sh` 会自动修补；手动编译时执行：`sed -i 's/foreach driver_test *: *drivers_tests/foreach driver_test, _args : drivers_tests/' <源码树>/tests/meson.build` |
| `driver_sources 表找不到` | 你给的 libfprint 源码树版本太老/太新，结构变了。改用 1.94.100（`./scripts/fetch-libfprint.sh`） |
| 链接时 `undefined reference to fpi_*` | 源码树里 `moc5159` 没被注册（`integrate.sh` 没跑成功）；重新运行 `./scripts/integrate.sh <源码树>` 并确认输出 |

### B3. 装的是驱动但指纹数据莫名消失

历史问题（**1.0.0 已修**）。症状：`/var/lib/fprint/<用户>/moc5159/0/` 里的文件
突然被删，日志里出现：

```
Deleted stored finger 7 ... as it is unknown to device
```

原因：fprintd 每次 Claim 设备都会把「存储里的 print id」和「设备报上来的 id」比对，
对不上就**主动删文件**。旧驱动在登记后读回模板号的长度判据写错（84 vs 88 字节），
导致 id 是垃圾值（7、8、0x5A 之类），于是每次 Claim 都可能删掉指纹。

1.0.0 已修（长度判据改为 `4 + 80`，id 统一取记录里的模板号）。
若你遇到，说明装的是旧版本，请重装后重新登记。

---

## C. 采集 / 匹配层

### C1. 按压没反应（登记卡住、验证超时）

- `fprintd` 日志里**完全没有** `验证采集` / `C0` 相关行 → 手指根本没触发采集；
- 采集行有，但质量分很低（`0x00`~`0x30`）→ 按得太轻/太偏。

处理：
1. 手指**指腹正中、垂直下压**，盖住整个传感器窗口，按实约 1 秒；
2. 每次完全抬离再按（别一直贴着）；
3. 传感器表面有油污/水渍时擦干；
4. 手太干（冬天）时可以稍微哈气。

> 本机型的 `0x30`（会话状态字）恒为 `0x00`，**不能**用来判断"手指在不在"，
> 所以驱动不依赖它 —— 看到它一直是 0 不代表设备坏了。

### C2. 每次都 `verify-no-match`，但手指明明是对的

按可能性从高到低：

1. **库里有多条模板**（最常见）。本固件只比对第一条记录。
   ```bash
   ./tools/fp-list.sh        # 看 fprintd 里有几根
   sudo ./tools/fp-list.sh   # 看芯片里有几条
   ```
   有多条 → 只留一根：`sudo fprintd-delete $USER -f <多余的手指>`，
   或者彻底重来：`sudo ./tools/fp-wipe.sh` 后重新登记。

2. **登记时的姿势和现在不一致**。用同样的姿势重录一次
   （`fp-wipe.sh` → `fp-enroll.sh`）。

3. **采集质量不够**。日志里质量分低于 `0x50` 基本就是没按实。

4. **换了个手指**。模板是按登记的那根手指建的，换手指必然不过。

### C3. 提示 "Verify result: verify-retry-scan"

采集质量不足，芯片要求重按。重新按一次即可，连续多次说明按压不稳定。

### C4. 为什么小面积传感器总是不太稳

这颗是 64×80 的小面积传感器（约 5mm 宽），可重复性天然比大面积差。
把"登记时怎么按"和"解锁时怎么按"保持一致，成功率会明显提高。
建议只录**最容易按到的那根**（一般是右手或左手食指）。

---

## D. PAM / 登录层

### D1. 锁屏还是只能输密码

```bash
grep pam_fprintd /etc/pam.d/common-auth
```

- 没有这行 → 没启用：`sudo pam-auth-update --enable fprintd`
- 有这行但仍走密码 → 检查是不是 `required`（应该是 `sufficient`），
  以及 `gdm-password` 是否 `@include common-auth`
- 指纹尝试次数太少（按一次错就跳密码）→ 加 `max-tries=5`（见 `02-usage.md` 第 5.2 节）

### D2. `sudo` 每次都要按指纹，很烦

只想让登录/锁屏用指纹、`sudo` 用密码：把 `pam_fprintd` 那一行从
`common-auth` 挪到 `/etc/pam.d/gdm-password` / `gdm-launch-environment` 里单独配置；
或者在 `/etc/pam.d/sudo` 里显式跳过 `common-auth` 的指纹行（进阶操作，改前先备份）。

### D3. GNOME 设置面板里「指纹」一直转圈 / 打不开

已知现象：polkit 认证对话框和 `pam_fprintd` 抢输入法时会互相卡住。
**不影响实际功能** —— 用命令行登记即可：

```bash
sudo ./tools/fp-enroll.sh right-index-finger 右手食指
```

登记后重开设置面板就能看到（有时需要重启 `gnome-control-center`）。

### D4. 指纹明明匹配了，但还要再输密码

PAM 栈里 `pam_fprintd` 位置不对（太靠后，前面的模块已经要求密码了）。
`sufficient` 必须放在密码认证模块**之前**。用 `pam-auth-update` 生成的一般是对的。

---

## E. 与 Windows 共存

- 驱动使用 **和 Windows 相同的 DB GUID** 打开模板库，所以能看到 Windows 里录的模板。
- **清库会连 Windows 那边的模板一起清掉**（`fp-wipe.sh` / `0xD7`）。
  如果你还要用 Windows Hello，谨慎清库。
- 匹配成功时驱动会做一次 `0xD8`（模板自适应写回），和 Windows 的行为一致，
  不会破坏 Windows 侧的模板。
- 同一台设备不要两边同时用（Windows 在跑、Linux 也在跑），会互相抢 USB。

---

## F. 收集信息（报问题时用）

```bash
{
  echo "=== 系统 ==="; cat /etc/os-release | head -3; uname -a
  echo "=== 设备 ==="; lsusb | grep -i 28e9
  echo "=== 库 ==="; ldconfig -p | grep libfprint-2.so.2
  echo "=== 安装记录 ==="; cat /var/lib/moc5159-driver/meta 2>/dev/null
  echo "=== fprintd ==="; systemctl status fprintd --no-pager 2>&1 | head -10
  echo "=== 指纹 ==="; fprintd-list $USER 2>&1
  echo "=== PAM ==="; grep pam_fprintd /etc/pam.d/common-auth 2>/dev/null
  echo "=== 日志 ==="; sudo journalctl -t fprintd --since '-30 min' --no-pager | tail -60
} > /tmp/moc5159-report.txt 2>&1
```

把 `/tmp/moc5159-report.txt` 贴出来即可。
