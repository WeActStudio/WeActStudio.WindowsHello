[English](README.md) · **简体中文**

# tools/sim —— MOC-5159 模块模拟器

不用插硬件，也能把**驱动 + libfprint 的整条链路**跑一遍。

它做两件事：

1. `mocsim-dev.c`：一个软件版的 MOC-5159 —— 128 字节帧协议、芯片模板库、匹配判定，
   行为对齐真实固件（包括"每条命令后追加 0xFF 尾帧"这种细节）；
2. `libmocsim.so`：用 `LD_PRELOAD` 拦掉 libusb，把 `libgusb` / `libfprint` 的每一次
   bulk/control 传输转给上面的状态机。

所以**驱动源码一行都不用改**，`fprintd`、GNOME 面板、`pam_fprintd` 看到的都是同一份
`libfprint`，只是设备换成了假的。

## 快速开始

```bash
cd tools/sim
./build.sh                                   # libmocsim.so + 两个测试程序

# 1) 纯协议层自检（不插设备、不要 libfprint）
LD_PRELOAD=./build/libmocsim.so ./build/mocsim-selftest

# 2) 端到端：libfprint → moc5159 驱动 → 模拟芯片
LD_PRELOAD=./build/libmocsim.so ./build/mocsim-e2e

# 3) 整个回归矩阵（改完驱动就跑这个）
./run-tests.sh
```

`mocsim-e2e` 依次跑：设备发现 → 打开（SETUP + DB_OPEN）→ 列模板 →
（库为空则登记）→ 验证 → 识别 → 删除，全部用真实驱动代码。

```
== mocsim end-to-end (expect MATCH) ==
  device: Petaic MOC-5159 (GD32W515) Touch Fingerprint Sensor
  ok   driver discovered through libfprint
  ok   device open (SETUP + DB_OPEN)
  info 1 stored print(s) on the chip
  ok   verify returned MATCH as configured
  ok   identify matched a gallery entry
  ok   delete removed the template

5 passed, 0 failed
```

退出码 0 = 全部符合预期，可以直接拿去当回归测试。

## 三个可执行文件

| 程序 | 干什么 | 需要什么 |
|---|---|---|
| `libmocsim.so` | libusb 假实现 + 设备状态机 | libusb 头文件 |
| `mocsim-selftest` | 12 项协议层断言（SETUP/D5/D9/C0/C1/图像块/清库…） | 只依赖 libusb |
| `mocsim-e2e` | 走完 libfprint 全流程 | 需要装好本驱动（libfprint 1.94.100） |

`mocsim-selftest` 是排错的第一站：它挂了说明是模拟器自己的问题；它过了但 `mocsim-e2e` 挂，
说明问题在驱动或 libfprint 层。

## 场景与环境变量

```bash
# 命中 / 不命中
LD_PRELOAD=./build/libmocsim.so MOCSIM_MATCH=0 ./build/mocsim-e2e

# 空库（会走完整的登记流程：5 级采集 + 提交）
LD_PRELOAD=./build/libmocsim.so MOCSIM_TEMPLATES=0 ./build/mocsim-e2e

# 库里有 3 条模板
LD_PRELOAD=./build/libmocsim.so MOCSIM_TEMPLATES=3 ./build/mocsim-e2e

# 打印每条命令
LD_PRELOAD=./build/libmocsim.so MOCSIM_LOG=1 ./build/mocsim-e2e
```

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOCSIM_MATCH` | `1` | 下一次 `0xC1` 比对的结果：1 命中 / 0 未命中 |
| `MOCSIM_TEMPLATES` | `1` | 芯片库里预置几条模板（`0` = 空库） |
| `MOCSIM_FAULT` | 空 | 故障注入，见下表，逗号分隔 |
| `MOCSIM_LOG` | `0` | 每条命令打一行到 stderr |
| `MOCSIM_DB` | `0` | 退出时打印模拟芯片里的库内容 |
| `MOCSIM_SPEED` | `0` | 每次传输人为延迟多少毫秒（用来复现"设备很慢"） |

### 故障注入

| 值 | 效果 | 期望的驱动行为 |
|---|---|---|
| `stale` | 在 IN 管道里留一帧残留 | 驱动先 drain 掉再发命令，流程照常成功 |
| `notail` | 不发 0xFF 尾帧 | 驱动 drain 直接超时，流程照常成功 |
| `quality` | 第一次 `0xC4` 返回 `0xA0` | 驱动报告"请抬起手指后重新按压"并重试，最后一次通过 |
| `badcrc` | 应答帧校验和错误 | 驱动丢弃该帧、重试到上限，报"设备未返回命令 0xDB 的应答" |
| `c1timeout` | `0xC1` 永不应答 | 驱动报"设备未返回命令 0xC1 的应答"，验证/识别失败（模拟器不等墙钟超时，瞬间返回；真机约 24 秒） |
| `gone` | 所有传输超时 | 打开设备就失败（模拟"设备没插"） |

### 实测结果

VMware / Ubuntu 26.04 / libfprint 1.94.100，`./run-tests.sh` 11/11 全绿，
另加 50 次压力轮次（混合模板数 + 连续 20 次空库登记）零失败、零 GLib 告警。

| 场景 | 结果 |
|---|---|
| selftest | 12/12 |
| 默认（1 条模板，命中） | 5/5，退出码 0 |
| `MOCSIM_MATCH=0` | 5/5，退出码 0 |
| `MOCSIM_TEMPLATES=0`（含完整登记） | 7/7，退出码 0 |
| `MOCSIM_TEMPLATES=3` | 5/5，退出码 0 |
| `FAULT=stale`（配合 `T=0`） | 7/7，退出码 0 |
| `FAULT=notail`（配合 `T=0`） | 7/7，退出码 0 |
| `FAULT=quality`（配合 `T=0`） | 7/7，退出码 0（中间有一次重试） |
| `FAULT=c1timeout` | 3 过 2 失，退出码 1，报错文案正确 |
| `FAULT=badcrc` | 打开设备失败，报错文案正确 |
| `FAULT=gone` | 打开设备失败，报错文案正确 |

> `notail` / `stale` / `quality` 要配合 `MOCSIM_TEMPLATES=0` 才能覆盖登记流程 ——
> 库里已有模板时 `mocsim-e2e` 会跳过登记，只跑验证/识别/删除。

## 为什么拦在 libusb 这一层

libfprint 没有插件机制，驱动是被编进 `libfprint-2.so.2` 的；再下面一层是 libgusb，
最底下是 libusb。三个可选切面：

- **改驱动**：要在源码里塞 ifdef，污染交付代码 —— 不要；
- **拦 GUsb**：要伪造 GObject（`GUsbDevice` 内部结构不公开）—— 做不到；
- **拦 libusb**：API 面小、结构体都是公开头文件里定义的、libgusb 只当它是黑盒 —— **选这个**。

libgusb 0.4 走的是 libusb 的**异步**接口（`libusb_submit_transfer` +
`libusb_handle_events_*`），所以 shim 里实现的是一个 transfer 队列：提交时入队，
事件泵里算出结果再回调。同步接口（`libusb_bulk_transfer`）也导出了，这样
`tools/moc5159-probe` 同样能跑在假设备上。

两个时序上的坑，改 shim 时别踩：

- **队列必须加锁**。真实 libusb 内部是串行的，但 libgusb 会同时从事件线程和主上下文
  泵事件 —— 两个线程可能 pop 到同一个 transfer、把同一个 GTask 完成两次，
  现象是随机 `SIGSEGV` 和 `g_task_return_int: assertion '!task->ever_returned' failed`。
  shim 里用一把**递归**互斥量（`pthread_mutex`，recursive）保护全部队列操作；
  递归是因为事件泵持锁期间会调 `sim()`，而 `sim()` 首次调用也要取锁。
- **完成和回调分两次 dispatch**。服务完一个 transfer 只把它挪进 ready 队列，
  callback 留到下一次泵调用才执行 —— 在同一次调用栈里回调会让 libgusb 在 libfprint
  自己的完成路径上回收 GTask，同样会踩上面的断言。

## 已知问题

1. **进程退出靠 `_exit()`**。`FpContext` 析构时会 join libgusb 的事件线程，在
   LD_PRELOAD 后端上这一步会卡住，所以 `mocsim-e2e` 打印完结果直接退出。
   这是测试工具，不影响被测代码。
2. **不做墙钟超时等待**。模拟器的"设备不应答"直接立刻返回失败，不会真的等
   USB 超时时间，所以 `FAULT=c1timeout` 是瞬间失败而不是真机的 24 秒。
3. **没有实现热插拔**（`libusb_has_capability` 返回 0）。设备在进程启动时就存在，
   插拔模拟请重开进程。
4. **模拟器不校验指纹内容**。真实固件在芯片内做比对，模拟器只按 `MOCSIM_MATCH`
   给出 `0xC1` 的判定结构，所以它验证的是**协议与时序**，不是算法。

## 和真设备一起用

两者不冲突：`LD_PRELOAD` 只在设了它的进程里生效，真设备照旧插着。
但**同一个进程**里只能看到一个设备（模拟器的），不会同时看到两个。

想在不摘设备的情况下跑一次"真机对照"：

```bash
# 真机
sudo ./tools/fp-list.sh
# 假机（同一个库、同一个驱动）
LD_PRELOAD=./build/libmocsim.so ./build/mocsim-e2e
```

## 文件

```
tools/sim/
├── mocsim.h          设备模型接口 + 环境变量/故障位定义
├── mocsim-dev.c      协议状态机（帧解析、模板库、C1 判定、故障注入）
├── mocsim-shim.c     LD_PRELOAD 的 libusb 假实现
├── mocsim-selftest.c 协议层自检
├── mocsim-e2e.c      libfprint 端到端测试
├── build.sh          构建
├── run-tests.sh      回归矩阵（改完驱动跑这个）
└── README.md / README.zh-CN.md
```
