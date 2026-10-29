[English](CHANGELOG.md) · **简体中文**

# 更新记录

版本号规则：主版本变动 = 驱动接口或行为不兼容；次版本 = 新增功能；修订号 = 修 bug。

## 1.0.0 — 2026-10-25

首个公开版本。

### 驱动

- libfprint 驱动 `moc5159`，支持 `28e9:5159`（Petaic MOC-5159 / GD32W515 + GSL6150，
  match-on-chip；模板与比对都在芯片内，不进系统内存）；
- 实现 `open` / `close` / `enroll` / `verify` / `identify` / `list` / `delete`
  全套 `FpDevice` 接口，走 libfprint 的 SSM 状态机；
- 传输层处理三个硬要求：命令前 drain 残留 IN 帧、bulk IN 缓冲固定 128 字节、不补 ZLP；
- 按 `0xC1` 应答的 payload 内部结构判断匹配成败（`ev` 字节与结果无关）；
- 模板号取 `max(tid)+1`，`0xC3` 提交完整 80 字节记录；
- 已登记指纹的 print id 统一取模板号，避免 fprintd 因 id 对不上而静默删除指纹文件。

### 安装 / 运维

- `scripts/install.sh`：多发行版（apt / dnf / yum / zypper / pacman / apk）一键安装，
  默认装 `/usr/local` 并用 `ld.so.conf.d` 让本驱动优先，**不动发行版的 libfprint 文件**；
- `--system` 模式可覆盖 `/usr`，安装前逐文件备份，可完整还原；
- `scripts/uninstall.sh`：两种模式都能干净卸载，不碰已登记的指纹数据；
- `scripts/doctor.sh`：8 项环境自检；
- `scripts/build-probe.sh`：编出独立诊断工具 `moc5159-probe`（只依赖 libusb，不经过 fprintd）；
- `scripts/fetch-libfprint.sh`：重新下载 libfprint 1.94.100 源码到 `vendor/`
  （只在包被删掉或想刷新时用）；
- `vendor/libfprint-1.94.100.tar.gz`：锁定版本的上游源码随仓库提供（约 12 MB），
  所以直接 `git clone` 就能**不联网**完成安装；
- 上游 `v1.94.100` 的 `tests/meson.build` 用单个变量迭代 dict，meson ≥ 1.10 会配置失败；
  `install.sh` 与 `fetch-libfprint.sh` 会自动打上兼容补丁（只影响测试用例，不影响驱动）；
- `tools/fp-enroll.sh` / `fp-verify.sh` / `fp-list.sh` / `fp-wipe.sh`：带桌面提示的一键操作。

### 文档

- 安装、使用、排错、协议四份说明，外加本文件与 `CONTRIBUTING.md`，**中英双语**（`docs/en/`、`docs/zh-CN/`，每页顶部可切换）。

### 模拟器（没有硬件也能测）

- `tools/sim/`：软件版 MOC-5159 —— 128 字节帧协议、芯片模板库、`0xC1` 命中判定都按真机行为
  建模，再用 `LD_PRELOAD` 顶掉 libusb，**驱动源码不用改**就能在没有模块的机器上跑完整流程；
- `mocsim-selftest`（12 项协议断言）与 `mocsim-e2e`（libfprint 发现 → 打开 → 列模板 →
  登记 → 验证 → 识别 → 删除）；
- 故障注入：`stale`（IN 侧残留帧）、`notail`（缺 `0xFF` 尾帧）、`quality`（首次采集被拒）、
  `c1timeout`、`badcrc`（校验和错）、`gone`（设备不存在）；
- `tools/sim/run-tests.sh`：11 个场景的回归矩阵，Ubuntu 26.04 / libfprint 1.94.100 全绿。

### 已知限制（固件行为，驱动无法绕过）

- **验证时固件只比对模板库里的第一条记录**，登记第二根手指不会生效；
- `0x30` 应答在本机型恒为 `0x00`，不能用于判断手指是否按下；
- 多模板库（`0xDA` 建第二个库）会让设备失去响应，只能物理拔插恢复。

### 实测环境

Ubuntu 26.04.1（内核 7.0.0）+ fprintd 1.94.5，VMware Workstation USB 直通；
发行版 libfprint 1.95.1 与本驱动 1.94.100 共存正常。
