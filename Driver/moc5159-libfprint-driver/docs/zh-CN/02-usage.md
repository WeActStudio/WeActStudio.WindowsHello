[English](../en/02-usage.md) · **简体中文**

# 02 · 使用说明

装好驱动之后，日常用到的就是下面这些。所有命令都在包根目录下执行。

---

## 1. 一图流：从零到能用

```bash
sudo ./scripts/install.sh --with-pam                # ① 装驱动 + 接 PAM
sudo ./tools/fp-enroll.sh right-index-finger 右手食指   # ② 登记 1 根手指
./tools/fp-verify.sh right-index-finger             # ③ 验证一次
# ④ Super+L 锁屏，用同一根手指解锁
```



---

## 2. 登记指纹

### 2.1 桌面脚本（推荐，带按压提示）

```bash
sudo ./tools/fp-enroll.sh right-index-finger 右手食指
```

- 先弹提示给 10 秒准备，然后开始登记（4~8 次按压）；
- **登记完成或超时后提示框立刻自动关闭**；
- 终端打印 `登记结果: 成功` 并列出当前已登记的指纹。

手指名（libfprint 约定，必须用这些名字）：

```
left-thumb            left-index-finger   left-middle-finger
left-ring-finger      left-little-finger
right-thumb           right-index-finger  right-middle-finger
right-ring-finger     right-little-finger
```

### 2.2 纯命令行

```bash
sudo fprintd-enroll $USER -f right-index-finger
```

### 2.3 GNOME 设置面板

`设置 → 系统 → 用户 → 指纹 → +`，按提示按压即可。  
若面板卡在"正在启动指纹服务"，改用命令行登记（原因通常是 polkit 对话框与  
`pam_fprintd` 抢占，不影响实际使用）。

### 2.4 按压要点（直接影响成功率）

传感器是**小面积玻璃盖**，命中率跟按压姿势强相关：

| 要点     | 说明                    |
| ------ | --------------------- |
| 用指腹正中  | 不要指尖、不要侧面斜着按          |
| 垂直下压   | 盖住整个传感器窗口，稍微用点力       |
| 每次 1 秒 | 按实约 1 秒，然后**完全抬离**再按  |
| 保持一致   | 每次位置/角度尽量一样，别一次偏左一次偏右 |

> 登记时的按压方式 = 以后解锁时要重复的方式。录得"松"就会解锁得"松"。

---

## 3. 验证

```bash
./tools/fp-verify.sh right-index-finger 右手食指
# Verify result: verify-match (done)      ← 成功
# Verify result: verify-no-match (done)   ← 不匹配
```

⚠️ **不要**用不带 `-f` 的 `fprintd-verify`：  
它只比对列表里的第一条，多指纹时会给出误导性结果。

命令行直接验证（不带桌面提示）：

```bash
fprintd-verify $USER -f right-index-finger
```

---

## 4. 查看当前状态

```bash
./tools/fp-list.sh          # 普通用户：看 fprintd 登记了什么
sudo ./tools/fp-list.sh     # root：额外读芯片库里的记录
sudo ./scripts/doctor.sh    # 全量环境自检
```

输出示例：

```
== fprintd 里用户 guocai 的指纹 ==
User guocai has 1 fingerprint(s) registered:
  - #0: right-index-finger

== 芯片模板库 ==
  已用 1 / 10
  [0] 模板号=1  计数器=1
```

如果两边条数不一致，说明存储和芯片对不上 —— 见 `03-troubleshooting.md`。

---

## 5. 接进 登录 / 锁屏 / sudo（PAM）

### 5.1 一键启用（Debian / Ubuntu）

```bash
sudo pam-auth-update --enable fprintd
# 或者安装时直接: sudo ./scripts/install.sh --with-pam
```

它会往 `/etc/pam.d/common-auth` 写一行 `pam_fprintd.so`，`gdm-password`、  
`login`、`sudo`、锁屏都通过 `@include common-auth` 继承。

### 5.2 让指纹可以多试几次（重要）

默认 `max-tries=1` 意味着**按错一次就直接跳密码框**。改成连续 5 次：

```bash
sudo sed -i 's|pam_fprintd.so|pam_fprintd.so max-tries=5 timeout=10|' /etc/pam.d/common-auth
grep pam_fprintd /etc/pam.d/common-auth
```

| 参数            | 含义           | 建议              |
| ------------- | ------------ | --------------- |
| `max-tries=N` | 连续允许 N 次指纹尝试 | 5（够用又不至于一直卡在指纹） |
| `timeout=N`   | 每次尝试等待秒数     | 10              |
| `try-first`   | 先指纹后密码（默认行为） | 不需要改            |

### 5.3 其它发行版

没有 `pam-auth-update` 的系统（Fedora / Arch / openSUSE），编辑 `/etc/pam.d/system-auth`  
（或 `password-auth`），在 `auth` 段**靠前**位置加：

```
auth      sufficient    pam_fprintd.so max-tries=5 timeout=10
```

`auth sufficient` 的意思是：指纹过了就直接放行，没过继续走后面的密码认证 ——  
这就是"失败自动回落密码"的关键。**不要**用 `required`，否则指纹失败密码也进不去。

### 5.4 关闭指纹登录

```bash
sudo pam-auth-update --disable fprintd      # Debian/Ubuntu
# 或手动删掉 common-auth / system-auth 里的 pam_fprintd 行
```

---

## 6. 锁屏 / 登录的行为

| 场景     | 表现                                           |
| ------ | -------------------------------------------- |
| 锁屏界面   | 直接按手指即可解锁，无需先点"用指纹登录"                        |
| GDM 登录 | 指纹图标可点，或直接按压传感器                              |
| `sudo` | 提示 `Password or swipe finger:` 时按手指          |
| 指纹失败   | 等满 `timeout` 秒后自动再等一次，共 `max-tries` 次，然后弹密码框 |

如果锁屏界面看不到指纹提示，通常是 PAM 没启用（第 5 节），跟驱动无关。

---

## 7. ⚠️ 多手指的限制（必读）

**这颗芯片固件在验证时只比对模板库里的第一条记录。**

实测（每一种都反复验证过）：

| 库内情况          | 结果                          |
| ------------- | --------------------------- |
| 只有 1 条模板      | 正常，稳定匹配                     |
| 2 条模板（先食指后中指） | 食指 match，中指 **永远 no-match** |
| 2 条模板，删掉第一条   | 剩下的那条**立刻可用**               |
| 只下发第 2 条模板当候选 | 仍然不匹配（D9 不能指定比对对象）          |

原因在固件，不在驱动：Windows 官方驱动的抓包里，库中 5 条记录其实全是**同一个  
模板号的 4 个样本**（自适应写回累积），从来没有真正跑过多模板比对；  
`DB_QUERY_BY_SUBJECT` 之类"按主体选模板"的命令在验证流程里根本不会被调用。  
技术细节见 `04-protocol-notes.md`。

### 那我想换一根手指怎么办

**先清空，再录新的**：

```bash
sudo ./tools/fp-wipe.sh                                  # 清芯片库 + 本机存储，需输入 yes
sudo ./tools/fp-enroll.sh left-thumb 左手拇指              # 录你想用的那根
```

### 已经录了两根怎么办

```bash
sudo fprintd-delete $USER -f right-middle-finger   # 删掉不要的那根
./tools/fp-list.sh                                  # 确认只剩一根
```

删完后，剩下的那根会立即恢复正常（因为比对的是"第一条"，删掉它前面的就行）。

---

## 8. 删除指纹 / 清空

```bash
# 删单根
sudo fprintd-delete $USER -f right-index-finger

# 全清（芯片 + 本机存储）
sudo ./tools/fp-wipe.sh
```

`fp-wipe.sh` 会发 `0xD7`（DB_WIPE）给芯片，并删掉  
`/var/lib/fprint/<用户>/moc5159/*/` 下的文件。**不可恢复**，所以要求输入 `yes` 确认。

---

## 9. 服务与日志

```bash
systemctl status fprintd          # 状态（D-Bus 按需启动，平时不跑是正常的）
sudo systemctl restart fprintd    # 重启
sudo journalctl -t fprintd -f     # 实时看驱动日志
sudo journalctl -t fprintd --since '-10 min' | grep -E '比对结果|质量|模板'
```

驱动侧的调试输出（`fp_info`）会以 `fprintd` tag 进 journal，常见行：

| 日志                 | 含义                     |
| ------------------ | ---------------------- |
| `模板库已挂载 (ev=0x27)` | 库头正常                   |
| `模板库已挂载 (ev=0x29)` | 库头为空（需要重建，驱动会自动发 0xDA） |
| `设备内模板数 N`         | 从 `D5` 读到的已用条数         |
| `比对结果: 命中 / 未命中`   | 芯片返回的判定                |
| `验证采集质量 0x5D`      | 图像质量分（0x5D~0x64 正常）    |

---

## 10. 数据存放与迁移

| 内容                       | 位置                                                    |
| ------------------------ | ----------------------------------------------------- |
| 模板本体                     | **芯片 flash 内部**（match-on-chip，主机拿不到特征）                |
| fprintd 记录（手指名、print id） | `/var/lib/fprint/<用户>/moc5159/<设备>/<id>`              |
| 驱动安装记录                   | `/var/lib/moc5159-driver/`（meta + 文件清单 + system 模式备份） |

- 换系统 / 重装系统：**芯片里的模板还在**，但 fprintd 的记录会丢。  
  重装驱动后如果 `fprintd-list` 显示没有指纹，用 `probe list` 看芯片里是否还有；  
  有的话重新登记一次同手指即可（会新建记录），或者直接继续用（不影响解锁，  
  只是 GNOME 面板里看不到对应手指名）。
- 换用户：指纹是**按用户**登记的，另一个用户需要用同一根手指重新登记一次。
- 换机器：芯片跟着设备走，把设备插到新机器上、装好驱动，重新登记即可。

---

## 11. 自动化 / 无人值守场景

不需要桌面提示时，直接用标准命令：

```bash
# 非交互登记（自己负责在提示时按压）
sudo fprintd-enroll $USER -f right-index-finger

# 判断结果
if fprintd-verify $USER -f right-index-finger | grep -q 'verify-match'; then
    echo "匹配成功"
fi
```

`tools/fp-*.sh` 在没有 `DISPLAY` / `zenity` 时会自动退化为纯终端输出，不会卡住。

---

## 12. 命令速查表

| 目的     | 命令                                                                                                |
| ------ | ------------------------------------------------------------------------------------------------- |
| 装驱动    | `sudo ./scripts/install.sh [--with-pam]`                                                          |
| 卸载     | `sudo ./scripts/uninstall.sh`                                                                     |
| 登记     | `sudo ./tools/fp-enroll.sh <finger> [中文名]`                                                        |
| 验证     | `./tools/fp-verify.sh <finger>`                                                                   |
| 看状态    | `./tools/fp-list.sh` / `sudo ./scripts/doctor.sh`                                                 |
| 换手指    | `sudo ./tools/fp-wipe.sh` 然后重新登记                                                                  |
| 删一根    | `sudo fprintd-delete $USER -f <finger>`                                                           |
| 多试几次指纹 | `sudo sed -i 's\|pam_fprintd.so\|pam_fprintd.so max-tries=5 timeout=10\|' /etc/pam.d/common-auth` |
| 看驱动日志  | `sudo journalctl -t fprintd -f`                                                                   |
| 独立诊断   | `sudo ./scripts/build-probe.sh && sudo ./build/probe list`                                        |
