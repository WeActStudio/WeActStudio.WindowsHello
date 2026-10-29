[English](../en/01-install.md) · **简体中文**

# 01 · 安装说明（多发行版）

## 1. 前置条件

硬件：Petaic MOC-5159（`VID:PID = 28e9:5159`）。确认能被识别：

```bash
lsusb | grep -i 28e9
# Bus 003 Device 004: ID 28e9:5159 GDMicroelectronics ...
```

若在 **VMware / VirtualBox 虚拟机**里，需要先把 USB 设备直通给客户机  
（VMware：虚拟机设置 → USB 控制器 → 连接该设备；或在虚拟机窗口底部点 USB 图标）。  
直通后宿主机 Windows 的 Windows Hello 会暂时不可用。

确认系统里有 fprintd：

```bash
# Debian / Ubuntu
sudo apt install fprintd libpam-fprintd zenity
# Fedora
sudo dnf install fprintd pam fprintd-pam zenity
# Arch
sudo pacman -S fprintd zenity
# openSUSE
sudo zypper install fprintd fprintd-pam zenity
```

> `zenity` 只有桌面提示脚本要用（`tools/fp-*.sh`）。纯命令行操作不需要。

## 2. 一键安装

```bash
git clone https://github.com/<你的用户名>/moc5159-libfprint-driver.git
cd moc5159-libfprint-driver
sudo ./scripts/install.sh
```

不想用 git：在 Releases 页下载 `moc5159-libfprint-driver-<版本>.tar.gz`，解包后同样  
`sudo ./scripts/install.sh`（那份包内自带 libfprint 源码，**完全离线**可装）。

脚本会依次做：

```
==> 环境检查          识别发行版 / 架构 / 设备
==> 安装构建依赖      自动按包管理器装（--no-deps 可跳过）
==> 准备 libfprint 源码   vendor/ 有就解压，没有就 git clone 上游 v1.94.100 到 /var/tmp/moc5159-build
==> 把 moc5159 驱动注册进 libfprint 构建系统
==> 编译              meson setup + ninja（首次 1~3 分钟）
==> 安装到 /usr/local 并通过 ld.so.conf.d 让它优先
==> 重启指纹服务
```

成功后会打印库路径、安装记录位置和下一步命令。

### 常用参数

| 参数                    | 作用                                  |
| --------------------- | ----------------------------------- |
| `--with-pam`          | 安装后启用 `pam_fprintd`（登录 / 锁屏 / sudo） |
| `--system`            | 装到 `/usr`，覆盖发行版 libfprint（先备份，可还原）  |
| `--drivers moc5159`   | 只编本驱动，体积小、依赖少（会让其它指纹设备不工作）          |
| `--libfprint-src DIR` | 用你手上已有的 libfprint 源码树（会复制一份，不改原树）   |
| `--no-deps`           | 不自动装依赖（离线环境 / 已装齐时用）                |
| `--prefix DIR`        | 自定义安装前缀（默认 `/usr/local`）            |

例：

```bash
sudo ./scripts/install.sh --with-pam                 # 推荐：装驱动 + 接指纹登录
sudo ./scripts/install.sh --system --drivers moc5159 # 彻底替换发行版，只留本驱动
```

## 3. 安装后验证

### 3.1 驱动是否生效

```bash
sudo ./scripts/doctor.sh
```

重点看这几项：

- `首选库来自本驱动（/usr/local/lib/.../libfprint-2.so.2）`
- `fprintd 实际加载: /usr/local/lib/...`
- `libfprint 的设备表里有 28e9:5159`

也可以手动查：

```bash
ldconfig -p | grep libfprint-2.so.2          # 第一行应指向 /usr/local
ls /usr/local/lib*/libfprint-2.so.2.0.0
```

### 3.2 读设备（不需要手指）

```bash
sudo systemctl stop fprintd
sudo ./scripts/build-probe.sh
sudo ./build/probe db        # 期望 ev=0x27（库已建好）；0x29 = 库头为空，需要重建
sudo ./build/probe list      # 能列出芯片里已有的模板
sudo systemctl start fprintd
```

### 3.3 登记第一根手指

```bash
sudo ./tools/fp-enroll.sh right-index-finger 右手食指
```

桌面会弹提示框：先给 10 秒准备时间，然后开始登记（4~8 次按压），  
**登记一结束提示框自动关闭**。终端最后会打印 `登记结果: 成功`。

### 3.4 验证

```bash
./tools/fp-verify.sh right-index-finger 右手食指
# 期望输出: Verify result: verify-match (done)
```

## 4. 手动编译（不想用脚本时）

```bash
# 依赖（Debian/Ubuntu）
sudo apt install build-essential meson ninja-build pkg-config python3 \
     libglib2.0-dev libgusb-dev libudev-dev libssl-dev libusb-1.0-0-dev

# 1) 解压仓库自带的 libfprint 1.94.100（不需要联网）
tar xzf vendor/libfprint-1.94.100.tar.gz -C /tmp
LFP=/tmp/libfprint-1.94.100
# 注：这个包已经打过 meson >= 1.10 的兼容补丁；若你自己从上游 clone，
#     需要手动改 tests/meson.build（见 03-troubleshooting.md §B2）

# 2) 注册驱动（幂等，可重复执行）
./scripts/integrate.sh "$LFP"

# 3) 配置 + 编译
meson setup /tmp/lfp-build "$LFP" -Dprefix=/usr/local \
      -Dintrospection=false -Dudev_rules=disabled -Dudev_hwdb=disabled \
      -Ddoc=false -Dinstalled-tests=false
ninja -C /tmp/lfp-build

# 4) 确认设备表
/tmp/lfp-build/libfprint/fprint-list-supported-devices | grep 5159
# 28e9:5159 | Petaic MOC-5159 (GD32W515) Touch Fingerprint Sensor

# 5) 安装并让本驱动优先于发行版那份
sudo ninja -C /tmp/lfp-build install
LIBDIR=$(dirname "$(find /usr/local -name 'libfprint-2.so.2.0.0' | head -1)")
echo "$LIBDIR" | sudo tee /etc/ld.so.conf.d/00-moc5159-libfprint.conf
sudo ldconfig
sudo systemctl restart fprintd
```

> 自己搭环境时注意：`libfprint` 安装在哪不重要，重要的是 **`ldconfig -p` 里  
> 它的路径排在发行版那份前面**。

## 5. 升级 / 重装

同一个包重复执行 `install.sh` 是安全的（覆盖安装）。跨版本升级建议：

```bash
sudo ./scripts/uninstall.sh        # 先卸载（不动已登记的指纹数据）
sudo ./scripts/install.sh --with-pam
```

已登记的指纹保存在 `/var/lib/fprint/<用户>/moc5159/` 和**芯片内部**，重装驱动不会丢。  
唯一会丢的情况：芯片库被清（`probe raw d7`）或 fprintd 认为设备上"找不到"该模板  
（驱动 bug 已修，见 `03-troubleshooting.md` 的"指纹文件莫名消失"）。

## 6. 卸载

```bash
sudo ./scripts/uninstall.sh           # 移除驱动，恢复系统原状
sudo ./scripts/uninstall.sh --purge   # 连 /var/tmp/moc5159-build 一起删
```

- `local` 模式：删掉 `/usr/local` 下的文件和 `00-moc5159-libfprint.conf`，发行版 libfprint 立刻恢复；
- `--system` 模式：从 `/var/lib/moc5159-driver/backup/` 还原被覆盖的发行版文件；
- **已登记的指纹数据不会被删**（想清就 `sudo ./tools/fp-wipe.sh` 或手动删 `/var/lib/fprint/`）。

## 7. 离线安装 / 自制分发包

仓库**自带** libfprint 源码：`vendor/libfprint-1.94.100.tar.gz`（约 12 MB），  
所以直接 clone 下来就能装，全程不需要联网 —— `install.sh` 会自动解压，你什么都不用多做。

如果你把这个包删掉了（或者 clone 时用了 `--filter=blob:none` 跳过大文件），  
离线安装前先把它取回来：

```bash
./scripts/fetch-libfprint.sh          # 生成 vendor/libfprint-1.94.100.tar.gz
sudo ./scripts/install.sh             # 之后不再联网
```

没有这个包时，`install.sh` 会回落到 `git clone` 上游 `v1.94.100`。

要把整套东西发给不能上网的机器：

```bash
# 在能上网的机器上（源码已在 vendor/ 里，不用再取）
cd .. && tar czf moc5159-libfprint-driver-1.0.0.tar.gz \
    --exclude='.git' --exclude='build' moc5159-libfprint-driver/
sha256sum moc5159-libfprint-driver-1.0.0.tar.gz > SHA256SUMS

# 拷过去
tar xzf moc5159-libfprint-driver-1.0.0.tar.gz
cd moc5159-libfprint-driver
sudo ./scripts/install.sh --no-deps     # 依赖得先自己装好
```

## 8. 做发行版原生包（可选）

想做成 `.deb` / `.rpm` 时不要直接打包 `libfprint-2.so.2`（会和发行版包冲突）。  
推荐两条路：

1. **随发行版 libfprint 一起编**（最干净）：拿发行版的 libfprint 源码包，  
   把 `src/*.c|h` 放进 `libfprint/drivers/moc5159/`，补 `meson.build` 注册  
   （`scripts/integrate.sh` 做的就是这两处），然后按发行版流程出包；
2. **只打 driver + 包装脚本**：把 `src/`、`scripts/`、`tools/` 打成包，  
   `postinst` 里调 `install.sh`（`--system` 模式），`prerm` 里调 `uninstall.sh`。

---

## 9. 本版的实测记录

环境：**Ubuntu 26.04.1（内核 7.0.0）**，VMware Workstation + USB 直通，  
设备 `Bus 003 Device 006: 28e9:5159`，系统自带 `fprintd 1.94.5` +  
`libfprint-2-2 1.95.1`（发行版原版，未改动）。

| 测试项                             | 结果                                                                              |
| ------------------------------- | ------------------------------------------------------------------------------- |
| `install.sh` 默认 local 模式        | ✅ 装 12 个文件到 `/usr/local/lib/x86_64-linux-gnu`，`ldconfig` 首选该路径                  |
| fprintd 实际加载的库                  | ✅ `/usr/local/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0`（`/proc/<pid>/maps` 确认） |
| 与发行版 1.95.1 共存                  | ✅ 发行版文件保持原样，fprintd 1.94.5 加载 1.94.100 正常，`fprintd-list` 能看到设备与已登记指纹            |
| 全驱动构建                           | ✅ 281 个设备（含 5159），不会挤压其它指纹传感器                                                   |
| `uninstall.sh`                  | ✅ `/usr/local` 清空、`ldconfig` 回落 `/usr`、已登记指纹数据保留                                |
| `install.sh --system`           | ✅ 备份发行版 `libfprint-2.so.2.0.0`(821 KB) → 覆盖(3.6 MB) → `apt-mark hold`           |
| `uninstall.sh`（system 模式）       | ✅ 从备份还原成发行版原文件，解除 hold                                                          |
| `build-probe.sh` + `probe list` | ✅ 读出芯片模板：`模板数 1 / [0] 模板号=1 计数器=1`                                              |
| `doctor.sh`                     | ✅ 8 个检查项全部通过                                                                    |
| 重装 / 覆盖安装                       | ✅ 幂等（同模式可重跑；换模式会要求先卸载）                                                          |

**未实测**：其它发行版实机、GDM 登录界面图形化提示、多手指（固件限制，见 `02-usage.md`）。
