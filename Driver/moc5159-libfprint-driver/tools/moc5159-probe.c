/*
 * moc5159-probe.c — MOC-5159 独立诊断工具 (只依赖 libusb-1.0)
 *
 * 用途: 在不安装 libfprint/fprintd 的机器上确认「设备在 Linux 下到底能不能通」,
 *       以及验证驱动依赖的 USB 硬性要求:
 *         1) 发命令前必须先把 IN 端点读空 (drain), 否则 OUT 会一直 NAK 到超时
 *         2) bulk IN 的 URB 长度必须正好 128 (回帧是两个满包, 没有短包收尾)
 *         3) 每条命令前读一次 SETUP 字节 (0xC0/0xC0, 1 字节, 期望 0x5C) 唤醒模组
 *       (早期版本认为「必须补 ZLP」, 实测已被推翻, 见 docs/zh-CN/04-protocol-notes.md)
 *
 * 编译:
 *   gcc -O2 -Wall -o moc5159-probe moc5159-probe.c ../src/moc5159-proto.c \
 *       $(pkg-config --cflags --libs libusb-1.0)
 *
 * 用法:
 *   sudo ./moc5159-probe info          # 描述符 + SETUP + 固件版本
 *   sudo ./moc5159-probe zlp           # 对比「补 ZLP / 不补 ZLP」的行为
 *   sudo ./moc5159-probe db            # DB_OPEN_DB + DB_GET_SIZE
 *   sudo ./moc5159-probe list          # 列出设备内模板记录
 *   sudo ./moc5159-probe mon -t10      # 只监听 bulk IN 10 秒 (看设备主动推什么)
 *   sudo ./moc5159-probe cap -t10      # 走一遍采集流程 + 持续监听 (卡点定位用)
 *   sudo ./moc5159-probe ctl 40 c0 0000 0000   # 任意厂商控制请求
 *   sudo ./moc5159-probe raw 24 01    # 发任意命令号 + payload (如 0x24 检测模式)
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

#include "../src/moc5159-proto.h"

#define CMD_TIMEOUT_MS   2000
#define CAPTURE_TIMEOUT  30000
#define RECV_URB_LEN     MOC5159_FRAME_SIZE   /* 关键: 必须正好 128 */
#define TRANSACT_TRIES   5    /* 刚 claim 接口时头几条命令会 NAK, 多给几次机会 */

static int g_trace = 0;
/*
 * 默认不补 ZLP。
 * 实测(2026-10-24): 每补一次 ZLP, 设备就回一帧 cmd=0xFF ev=0x9C (校验失败) ——
 * 固件把零长度包当成「一帧空数据」, 校验不过, 通道被搞脏。
 * 早期文档说「必须补 ZLP」是对现象的误判, 见 docs/zh-CN/04-protocol-notes.md。
 */
static int g_zlp = 0;
static int g_cancel_first = 0;   /* --cancel: 采集前先发 0x0C 做对照 */
static int g_clear_halt = 0;     /* --clear-halt: 打开后先清端点 halt */
static int g_timeout_ms = 0;   /* -T<ms> 覆盖单条命令的等待时间 */
static int g_force = 0;
static int g_sync_img = 0;   /* --sync-img: 先等 OUT 完成再提交 IN (对照实验) */        /* --force: 允许危险操作 (dump 0x13) */
static int g_no_cancel = 0;    /* --no-cancel: 结束时不发 0x0C (实测 CANCEL 之后传感器对按压失去反应) */
static int g_c0_timeout_ms = 25000;  /* 0xC0 采集应答等待时间 */
static int g_no_db = 0;       /* --no-db: enroll 跳过 DB_OPEN (贴合 Windows 抓包) */

static void
hexdump (const char *label, const uint8_t *b, size_t n)
{
  size_t i;

  printf ("%s (%zu bytes)", label, n);
  for (i = 0; i < n; i++)
    {
      if (i % 16 == 0)
        printf ("\n  %04zx:", i);
      printf (" %02x", b[i]);
    }
  printf ("\n");
}

static int64_t
now_ms (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- 传输层 ---------- */

/* 读 SETUP 状态: 控制 IN, bmRequestType=0xC0, bRequest=0xC0, 1 字节 */
static int
setup_read (libusb_device_handle *h, uint8_t *status)
{
  int r = libusb_control_transfer (h, 0xC0, MOC5159_SETUP_REQUEST, 0, 0,
                                   status, 1, CMD_TIMEOUT_MS);
  if (r < 0)
    return r;
  return (r == 1) ? 0 : -1;
}

/*
 * 128 字节 bulk OUT + 可选 ZLP。
 * libusb 同步接口没有 ADD_ZERO_PACKET 开关, 但「长度为 0 的 bulk 传输」
 * 本身就是一个零长度包, 效果等价 —— 本工具的 zlp 子命令就是用来实测这一点的。
 */
static int
frame_send (libusb_device_handle *h, const uint8_t *frame, int with_zlp)
{
  int transferred = 0;
  int r;

  (void) with_zlp;
  r = libusb_bulk_transfer (h, MOC5159_EP_OUT, (uint8_t *) frame,
                            MOC5159_FRAME_SIZE, &transferred, CMD_TIMEOUT_MS);
  if (r != 0 || transferred != MOC5159_FRAME_SIZE)
    {
      fprintf (stderr, "  [x] bulk OUT 失败: %s (%d/%d)\n",
               libusb_error_name (r), transferred, MOC5159_FRAME_SIZE);
      return -1;
    }

  if (g_zlp)
    {
      uint8_t dummy = 0;
      r = libusb_bulk_transfer (h, MOC5159_EP_OUT, &dummy, 0,
                                &transferred, CMD_TIMEOUT_MS);
      if (r != 0)
        fprintf (stderr, "  [!] ZLP 失败: %s (可能仍能工作)\n",
                 libusb_error_name (r));
    }

  return 0;
}

/*
 * 清空 IN 侧残留帧。
 * 设备是「一问一答」的双缓冲模型: 若上一轮还有没被读走的帧挂在 IN 端点,
 * 固件不会接受新的 bulk OUT —— 表现为 libusb_bulk_transfer(OUT) 一直 NAK,
 * 返回 LIBUSB_ERROR_TIMEOUT 且 transferred=0。
 */
static void
drain_in (libusb_device_handle *h, int per_try_ms)
{
  uint8_t buf[MOC5159_FRAME_SIZE];
  int guard = 0;

  while (guard++ < 8)
    {
      int transferred = 0;
      int r = libusb_bulk_transfer (h, MOC5159_EP_IN, buf, RECV_URB_LEN,
                                    &transferred, per_try_ms);
      if (r != 0)
        break;
      if (g_trace)
        fprintf (stderr, "  [t] 丢弃残留 %d 字节\n", transferred);
    }
}

/* 读一帧 128 字节; 忽略 0xFF 尾帧, 直到拿到 want_cmd 或超时 */
static int
frame_recv (libusb_device_handle *h, uint8_t want_cmd,
            uint8_t out[MOC5159_FRAME_SIZE], Moc5159Frame *fr, int timeout_ms)
{
  int64_t deadline = now_ms () + timeout_ms;

  for (;;)
    {
      int transferred = 0;
      int left = (int) (deadline - now_ms ());
      int r;

      if (left <= 0)
        return -1;

      memset (out, 0, MOC5159_FRAME_SIZE);
      r = libusb_bulk_transfer (h, MOC5159_EP_IN, out, RECV_URB_LEN,
                                &transferred, left);
      if (r == LIBUSB_ERROR_TIMEOUT)
        return -1;
      if (r != 0)
        {
          fprintf (stderr, "  [x] bulk IN 失败: %s\n", libusb_error_name (r));
          return -1;
        }
      if (transferred != MOC5159_FRAME_SIZE)
        continue;

      if (moc5159_frame_parse (out, (size_t) transferred, fr) != 0)
        {
          if (g_trace)
            hexdump ("坏帧", out, (size_t) transferred);
          continue;
        }

      if (g_trace)
        fprintf (stderr, "  [t] RX cmd=0x%02X b3=0x%02X ev=0x%02X b5=0x%02X len=%zu\n",
                 fr->cmd, fr->byte3, fr->event, fr->byte5, fr->payload_len);

      /* 0xFF + ev=0x00 是设备在每条命令后追加的传输尾帧, 不是应答;
         但 0xFF 带着非零 event 时是固件报错(例如 0x9C=校验失败), 必须暴露出来 */
      if (fr->cmd == 0xFF && fr->event == 0x00 && want_cmd != 0xFF)
        continue;

      return 0;
    }
}

/* _POSIX_C_SOURCE=200809L 下 usleep() 已被移除, 用 nanosleep 代替 */
static void
msleep (unsigned int ms)
{
  struct timespec ts;

  ts.tv_sec = (time_t) (ms / 1000u);
  ts.tv_nsec = (long) (ms % 1000u) * 1000000L;
  while (nanosleep (&ts, &ts) == -1 && errno == EINTR)
    ;
}

/* 等到 SETUP 回 0x5C —— 模组空闲约 2s 会进低功耗, 此时控制端点也不应答 */
static int
wake_ready (libusb_device_handle *h, int tries)
{
  uint8_t st = 0;

  for (int i = 0; i < tries; i++)
    {
      if (setup_read (h, &st) == 0 && st == MOC5159_SETUP_READY)
        return 0;
      msleep (20);
    }
  if (g_trace)
    fprintf (stderr, "  [t] 唤醒超时, 最后一次 SETUP=0x%02X\n", st);
  return -1;
}

/* 一条完整事务: 唤醒 -> 发帧 -> 收同命令号响应。发送失败时重试 (冷插拔后首条会失败) */
static int
transact (libusb_device_handle *h, uint8_t cmd,
          const uint8_t *payload, size_t paylen,
          uint8_t out[MOC5159_FRAME_SIZE], Moc5159Frame *fr,
          int timeout_ms, int with_zlp)
{
  uint8_t frame[MOC5159_FRAME_SIZE];
  uint8_t st = 0;

  (void) with_zlp;   /* 统一走全局 g_zlp, 便于 --no-zlp 对照 */
  moc5159_frame_encode (cmd, payload, paylen, frame);

  if (g_timeout_ms > 0)
    timeout_ms = g_timeout_ms;

  for (int attempt = 1; attempt <= TRANSACT_TRIES; attempt++)
    {
      drain_in (h, 30);
      setup_read (h, &st);   /* 唤醒; 失败也继续, 只是可能要多试一次 */

      if (g_trace)
        hexdump ("TX", frame, MOC5159_FRAME_SIZE);

      if (frame_send (h, frame, g_zlp) == 0)
        return frame_recv (h, cmd, out, fr, timeout_ms);

      if (attempt < TRANSACT_TRIES)
        {
          int backoff = 60 * attempt;   /* 60 / 120 / 180 / 240 ms 递增 */
          fprintf (stderr, "  [!] 第 %d 次发送失败, %dms 后重试\n", attempt, backoff);
          msleep ((unsigned) backoff);
        }
    }

  return -1;
}

/* 静默事务: 不做 drain、不读 SETUP, 直接发一条收一条。
 * 依据抓包: Windows 全程只在「会话开始」读一次 SETUP (235 秒里只有 2 次),
 * 之后每条命令都不读。SETUP 疑似是会话租约的建立动作, 每条命令前都读会
 * 把 0xC2 建立的录入会话冲掉 —— 表现为 0x30 立刻回 00、0xC4 永远不完成。 */
static int
transact_quiet (libusb_device_handle *h, uint8_t cmd,
                const uint8_t *payload, size_t paylen,
                uint8_t out[MOC5159_FRAME_SIZE], Moc5159Frame *fr,
                int timeout_ms)
{
  uint8_t frame[MOC5159_FRAME_SIZE];

  moc5159_frame_encode (cmd, payload, paylen, frame);

  if (g_timeout_ms > 0)
    timeout_ms = g_timeout_ms;

  if (g_trace)
    hexdump ("TX", frame, MOC5159_FRAME_SIZE);

  if (frame_send (h, frame, g_zlp) != 0)
    return -1;

  return frame_recv (h, cmd, out, fr, timeout_ms);
}

/* 图像数据块交换 (2048 字节; 调用方已 drain, ZLP 非必需) */
static libusb_context *g_ctx = NULL;

typedef struct
{
  int out_done;
  int in_done;
  int out_status;
  int in_status;
  int out_len;
  int in_len;
} ImgxState;

static void
imgx_cb (struct libusb_transfer *t)
{
  ImgxState *st = t->user_data;

  if (t->endpoint & LIBUSB_ENDPOINT_IN)
    {
      st->in_done = 1;
      st->in_status = t->status;
      st->in_len = t->actual_length;
    }
  else
    {
      st->out_done = 1;
      st->out_status = t->status;
      st->out_len = t->actual_length;
    }
}

static const char *
xfer_status_name (int st)
{
  switch (st)
    {
    case LIBUSB_TRANSFER_COMPLETED: return "COMPLETED";
    case LIBUSB_TRANSFER_ERROR:     return "ERROR";
    case LIBUSB_TRANSFER_TIMED_OUT: return "TIMED_OUT";
    case LIBUSB_TRANSFER_CANCELLED: return "CANCELLED";
    case LIBUSB_TRANSFER_STALL:     return "STALL";
    case LIBUSB_TRANSFER_NO_DEVICE: return "NO_DEVICE";
    case LIBUSB_TRANSFER_OVERFLOW:  return "OVERFLOW";
    default:                        return "?";
    }
}

/*
 * 一次 2048 字节块交换。抓包 (#88..#91) 里 Windows 是 OUT 和 IN 同时挂起:
 * 先提交 2048 OUT, 0.17ms 后立刻提交 2048 IN, 设备 2ms 内回满 2048。
 * 所以这里也用异步双提交, 不做 ZLP。
 */
static int
image_exchange (libusb_device_handle *h,
                const uint8_t tx[MOC5159_IMG_BLOCK_SIZE],
                uint8_t rx[MOC5159_IMG_BLOCK_SIZE])
{
  struct libusb_transfer *to = NULL;
  struct libusb_transfer *ti = NULL;
  ImgxState st = { 0 };
  int r;

  if (g_sync_img)
    {
      int transferred = 0;

      r = libusb_bulk_transfer (h, MOC5159_EP_OUT, (uint8_t *) tx,
                                MOC5159_IMG_BLOCK_SIZE, &transferred, 5000);
      printf ("   OUT(同步): %s, 实际 %d 字节\n", libusb_error_name (r), transferred);
      if (r != 0)
        return r;

      r = libusb_bulk_transfer (h, MOC5159_EP_IN, rx,
                                MOC5159_IMG_BLOCK_SIZE, &transferred, 5000);
      printf ("   IN (同步): %s, 实际 %d 字节\n", libusb_error_name (r), transferred);
      return r;
    }

  to = libusb_alloc_transfer (0);
  ti = libusb_alloc_transfer (0);
  if (!to || !ti)
    return -1;

  libusb_fill_bulk_transfer (to, h, MOC5159_EP_OUT, (uint8_t *) tx,
                             MOC5159_IMG_BLOCK_SIZE, imgx_cb, &st, 5000);
  libusb_fill_bulk_transfer (ti, h, MOC5159_EP_IN, rx,
                             MOC5159_IMG_BLOCK_SIZE, imgx_cb, &st, 5000);

  r = libusb_submit_transfer (to);
  if (r != 0)
    {
      printf ("   OUT 提交失败: %s\n", libusb_error_name (r));
      libusb_free_transfer (to);
      libusb_free_transfer (ti);
      return r;
    }

  r = libusb_submit_transfer (ti);
  if (r != 0)
    {
      printf ("   IN 提交失败: %s\n", libusb_error_name (r));
      libusb_cancel_transfer (to);
    }

  while (!st.out_done || !st.in_done)
    {
      struct timeval tv = { 1, 0 };
      r = libusb_handle_events_timeout_completed (g_ctx, &tv, NULL);
      if (r != 0 && r != LIBUSB_ERROR_INTERRUPTED)
        break;
    }

  printf ("   OUT: %s, 实际 %d 字节\n", xfer_status_name (st.out_status), st.out_len);
  printf ("   IN : %s, 实际 %d 字节\n", xfer_status_name (st.in_status), st.in_len);

  libusb_free_transfer (to);
  libusb_free_transfer (ti);

  return st.in_status == LIBUSB_TRANSFER_COMPLETED ? 0 : -1;
}

/* ---------- 子命令 ---------- */

static int
cmd_info (libusb_device_handle *h)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t st = 0;
  int r;

  r = setup_read (h, &st);
  printf ("== SETUP (0xC0/0xC0) ==  %s, status=0x%02X (期望 0x5C)\n",
          r == 0 ? "OK" : "失败", st);

  if (transact (h, MOC_SUB_CMD_GET_FW_VERSION, NULL, 0, rsp, &fr,
                CMD_TIMEOUT_MS, 1) == 0)
    {
      printf ("== GET_FW_VERSION (0x10) ==  ev=0x%02X  ", fr.event);
      for (size_t i = 0; i < fr.payload_len; i++)
        printf ("%02x ", fr.payload[i]);
      printf ("\n");
    }
  else
    printf ("== GET_FW_VERSION (0x10) ==  无响应\n");

  if (transact (h, MOC_SUB_CMD_GET_SENSOR_INFO, NULL, 0, rsp, &fr,
                CMD_TIMEOUT_MS, 1) == 0)
    {
      printf ("== GET_SENSOR_INFO (0x11) ==  ev=0x%02X  ", fr.event);
      for (size_t i = 0; i < fr.payload_len; i++)
        printf ("%02x ", fr.payload[i]);
      printf ("\n");
    }
  else
    printf ("== GET_SENSOR_INFO (0x11) ==  无响应\n");

  return 0;
}

/* 同一命令, 分别补 / 不补 ZLP, 观察设备是否应答 */
static int
cmd_zlp (libusb_device_handle *h)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t st = 0;
  int r;
  int ok_a = 0, ok_b = 0;

  printf ("== 交替 6 轮: A=补 ZLP, B=不补 ZLP ==\n");
  for (int round = 0; round < 6; round++)
    {
      setup_read (h, &st);

      g_zlp = 1;
      r = transact (h, MOC_SUB_CMD_GET_SENSOR_INFO, NULL, 0, rsp, &fr,
                    CMD_TIMEOUT_MS, 1);
      printf ("  A%d: %-8s", round, r == 0 ? "有应答" : "超时");
      if (r == 0)
        {
          ok_a++;
          printf ("  ev=0x%02X len=%zu", fr.event, fr.payload_len);
        }
      printf ("\n");

      setup_read (h, &st);
      g_zlp = 0;
      r = transact (h, MOC_SUB_CMD_GET_SENSOR_INFO, NULL, 0, rsp, &fr,
                    CMD_TIMEOUT_MS, 0);
      printf ("  B%d: %-8s", round, r == 0 ? "有应答" : "超时");
      if (r == 0)
        {
          ok_b++;
          printf ("  ev=0x%02X len=%zu", fr.event, fr.payload_len);
        }
      printf ("\n");
    }

  printf ("\nA(补 ZLP) 成功 %d/6, B(不补) 成功 %d/6\n", ok_a, ok_b);
  if (ok_a > 0 && ok_b == 0)
    printf ("结论: 补 ZLP 才能通\n");
  else if (ok_a == 0 && ok_b > 0)
    printf ("结论: 补 ZLP 反而坏 (设备把空包当坏帧) -> 不要补\n");
  else if (ok_a > 0 && ok_b > 0)
    printf ("结论: 两者都能通; 但 --zlp 会额外产生 0xFF/0x9C 噪声帧, 建议不补\n");
  return 0;
}

static int
cmd_db (libusb_device_handle *h)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t payload[36];
  const uint8_t guid[16] = MOC5159_WINBIO_DB_GUID;

  memset (payload, 0, sizeof payload);
  memcpy (payload, guid, 16);

  if (transact (h, MOC_SUB_CMD_DB_OPEN_DB, payload, sizeof payload,
                rsp, &fr, CMD_TIMEOUT_MS, 1) != 0)
    {
      printf ("== DB_OPEN_DB (0xDB) ==  无响应\n");
      return 1;
    }
  printf ("== DB_OPEN_DB (0xDB) ==  ev=0x%02X %s\n", fr.event,
          (fr.event == MOC_EV_OK ||
           fr.event == MOC_EV_DB_HEADER_DUPLICATED) ? "(OK)" : "(失败)");

  if (transact (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                CMD_TIMEOUT_MS, 1) != 0)
    {
      printf ("== DB_GET_SIZE (0xD5) ==  无响应\n");
      return 1;
    }

  if (fr.payload_len >= 12)
    {
      uint32_t cap, used;

      memcpy (&cap, fr.payload, 4);
      memcpy (&used, fr.payload + 8, 4);
      printf ("== DB_GET_SIZE (0xD5) ==  容量 %u / 已用 %u\n", cap, used);
    }
  return 0;
}

static int
cmd_list (libusb_device_handle *h)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t payload[36];
  const uint8_t guid[16] = MOC5159_WINBIO_DB_GUID;
  uint32_t cap = 0, used = 0;

  memset (payload, 0, sizeof payload);
  memcpy (payload, guid, 16);
  transact (h, MOC_SUB_CMD_DB_OPEN_DB, payload, sizeof payload,
            rsp, &fr, CMD_TIMEOUT_MS, 1);

  if (transact (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                CMD_TIMEOUT_MS, 1) != 0 || fr.payload_len < 12)
    {
      printf ("DB_GET_SIZE 失败, 无法枚举\n");
      return 1;
    }
  memcpy (&cap, fr.payload, 4);
  memcpy (&used, fr.payload + 8, 4);
  printf ("模板数: %u (容量 %u)\n", used, cap);

  for (uint32_t i = 0; i < used; i++)
    {
      uint8_t idx = (uint8_t) i;
      uint32_t slot, counter;

      if (transact (h, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &idx, 1,
                    rsp, &fr, CMD_TIMEOUT_MS, 1) != 0 || fr.payload_len < 84)
        {
          printf ("  [%u] 读取失败\n", i);
          continue;
        }

      memcpy (&slot, fr.payload + 4, 4);
      memcpy (&counter, fr.payload + 4 + MOC5159_RECORD_CNT_OFF, 4);
      printf ("  [%u] 模板号=0x%08X (%u)  计数器=%u  ev=0x%02X\n",
              i, slot, slot, counter, fr.event);
      if (getenv ("MOC_RECHEX") != NULL)
        {
          printf ("      hdr : ");
          for (int b = 0; b < 4; b++)
            printf ("%02X ", fr.payload[b]);
          printf ("\n");
          for (int row = 0; row < 5; row++)
            {
              printf ("      %3d : ", row * 16);
              for (int b = 0; b < 16 && row * 16 + b < 80; b++)
                printf ("%02X ", fr.payload[4 + row * 16 + b]);
              printf ("\n");
            }
        }
    }

  return 0;
}

/* 同会话内: GUID 开库 -> 0xDF 删除(payload 由命令行给 hex) -> 重新枚举 */
static int parse_hex (const char *s, uint8_t *out, size_t max);

static int
cmd_del (libusb_device_handle *h, const char *id_hex)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t payload[36];
  const uint8_t guid[16] = MOC5159_WINBIO_DB_GUID;
  uint32_t cap = 0, used = 0;
  uint32_t want_id;
  uint8_t rec[MOC5159_RECORD_SIZE];
  int found = -1;

  /* 参数 = 模板号 (十进制或 0x 十六进制) */
  want_id = (uint32_t) strtoul (id_hex, NULL, 0);

  memset (payload, 0, sizeof payload);
  memcpy (payload, guid, 16);
  if (transact (h, MOC_SUB_CMD_DB_OPEN_DB, payload, sizeof payload,
                rsp, &fr, CMD_TIMEOUT_MS, 1) != 0)
    {
      printf ("DB_OPEN 失败\n");
      return 1;
    }
  printf ("DB_OPEN: ev=0x%02X len=%zu\n", fr.event, fr.payload_len);

  if (transact (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                CMD_TIMEOUT_MS, 1) != 0 || fr.payload_len < 12)
    {
      printf ("DB_GET_SIZE 失败\n");
      return 1;
    }
  memcpy (&cap, fr.payload, 4);
  memcpy (&used, fr.payload + 8, 4);
  printf ("删除前: 模板数 %u (容量字段 %u)\n", used, cap);
  if (used == 0)
    {
      printf ("芯片库为空\n");
      return 1;
    }

  /* D9 遍历, 按模板号定位记录; 0xDF payload = 应答 payload[4:84] 的 80B 记录回显 */
  for (uint32_t i = 0; i < used && found < 0; i++)
    {
      uint8_t idx = (uint8_t) i;
      uint32_t rid;

      if (transact (h, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &idx, 1,
                    rsp, &fr, CMD_TIMEOUT_MS, 1) != 0 || fr.payload_len < 84)
        continue;
      memcpy (&rid, fr.payload + 4, 4);
      printf ("  [%u] 模板号=%u\n", i, rid);
      if (rid == want_id)
        {
          memcpy (rec, fr.payload + 4, MOC5159_RECORD_SIZE);
          found = (int) i;
        }
    }
  if (found < 0)
    {
      printf ("芯片内找不到模板 %u\n", want_id);
      return 1;
    }

  printf ("DEL_RECORD(0xDF) idx=%d 回显 80B 记录: ", found);
  if (transact (h, MOC_SUB_CMD_DB_DEL_RECORD, rec, sizeof rec,
                rsp, &fr, CMD_TIMEOUT_MS, 1) != 0)
    {
      printf ("无响应\n");
      return 1;
    }
  printf ("ev=0x%02X len=%zu %s\n", fr.event, fr.payload_len,
          fr.event == MOC_EV_OK ? "(成功)" : "(失败!)");

  if (transact (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                CMD_TIMEOUT_MS, 1) == 0 && fr.payload_len >= 12)
    {
      memcpy (&used, fr.payload + 8, 4);
      printf ("删除后: 模板数 %u\n", used);
    }
  return (fr.event == MOC_EV_OK) ? 0 : 1;
}

static int
cmd_dump (libusb_device_handle *h)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t sub = 0x02;
  uint8_t tx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t rx[MOC5159_IMG_BLOCK_SIZE];
  int r;

  printf ("== DUMP_DATA (0x13, sub=0x02) ==\n");
  if (!g_force)
    {
      printf ("   拒绝执行: 0x13 只在「刚刚采集成功」后才有意义。\n"
              "   没有待取图像时发 0x13 会让固件停在等图像块的状态,\n"
              "   实测只能断电拔插恢复 (clear_halt / SET_CONFIGURATION 都无效)。\n"
              "   确实要继续请加 --force。\n");
      return 1;
    }
  if (transact (h, MOC_SUB_CMD_DUMP_DATA, &sub, 1, rsp, &fr,
                CMD_TIMEOUT_MS, 1) != 0)
    {
      printf ("   无响应 (out 端被 NAK? 先看 probe zlp 的对照组)\n");
      return 1;
    }
  printf ("   ev=0x%02X len=%zu\n", fr.event, fr.payload_len);

  memset (tx, 0x00, sizeof tx);
  memset (tx + 9, 0x01, sizeof tx - 9);
  tx[0] = 0x08; tx[1] = 0x09; tx[4] = 0xF7; tx[5] = 0x07; tx[8] = 0x13;

  r = image_exchange (h, tx, rx);
  if (r != 0)
    {
      printf ("   图像块交换失败: %s\n", libusb_error_name (r));
      return 1;
    }

  printf ("   图像块首 16 字节:");
  for (int i = 0; i < 16; i++)
    printf (" %02x", rx[i]);
  printf ("\n");

  return 0;
}

/* 十六进制字符串 -> 字节数组; 返回字节数, 出错返回 -1 */
static int
parse_hex (const char *s, uint8_t *out, size_t max)
{
  size_t len = strlen (s);
  int n = 0;

  if (len % 2 != 0 || len / 2 > max)
    return -1;
  for (size_t k = 0; k < len; k += 2)
    {
      char byte[3] = { s[k], s[k + 1], 0 };
      out[n++] = (uint8_t) strtol (byte, NULL, 16);
    }
  return n;
}

static int
cmd_raw (libusb_device_handle *h, const char *cmd_hex, const char *pay_hex)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  uint8_t payload[MOC5159_PAYLOAD_MAX];
  Moc5159Frame fr;
  long cmd = strtol (cmd_hex, NULL, 16);
  int paylen = 0;

  /*
   * 0x13 (DUMP_DATA) 拿到 ack 之后必须立刻做 2048 字节块交换, 否则固件会停在
   * "等图像块"状态, 此后所有命令(含 0x30/0xD5/0xDB)全部无响应, 只能物理拔插恢复。
   * 实测踩过一次, 这里直接挡掉, 正确入口是 `dump`。
   */
  if (cmd == MOC_SUB_CMD_DUMP_DATA)
    {
      fprintf (stderr,
               "拒绝发送 0x13: 它必须紧跟 2048 字节块交换, 单独发会锁死设备。\n"
               "请用 `./probe dump`。\n");
      return 1;
    }

  if (pay_hex != NULL)
    {
      paylen = parse_hex (pay_hex, payload, sizeof payload);
      if (paylen < 0)
        {
          fprintf (stderr, "payload 必须是偶数长度十六进制, 且 <= %d 字节\n",
                   MOC5159_PAYLOAD_MAX);
          return 1;
        }
    }

  printf ("命令 0x%02lX (payload %d 字节)", cmd, paylen);
  for (int k = 0; k < paylen; k++)
    printf (" %02x", payload[k]);
  printf ("\n");

  if (transact (h, (uint8_t) cmd, paylen ? payload : NULL, (size_t) paylen,
                rsp, &fr, CMD_TIMEOUT_MS, 1) != 0)
    {
      printf ("   无响应\n");
      return 1;
    }

  printf ("   ev=0x%02X len=%zu\n", fr.event, fr.payload_len);
  if (fr.payload_len > 0)
    hexdump ("   payload", fr.payload, fr.payload_len);
  return 0;
}

/* ---------------------------------------------------------------
 * 同一会话内按顺序连发多条命令, 打印各自应答。
 * 用来判断某条命令是否会改变"当前模板 / 当前记录"这类会话状态:
 * 发它之前和之后各读一遍 D5 + D9, 内容变了说明它动了状态。
 *
 *   ./probe seq d5 d9:00 d9:01
 *   ./probe seq d0:00000001 d5 d9:00 d9:01
 * --------------------------------------------------------------- */
static int
cmd_seq (libusb_device_handle *h, const char *const *args, int n)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  uint8_t pl[MOC5159_PAYLOAD_MAX];
  Moc5159Frame fr;

  for (int k = 0; k < n; k++)
    {
      char buf[1024];
      char *colon;
      int pln = 0;
      unsigned cmd;

      strncpy (buf, args[k], sizeof buf - 1);
      buf[sizeof buf - 1] = '\0';
      colon = strchr (buf, ':');
      if (colon != NULL)
        {
          *colon = '\0';
          pln = parse_hex (colon + 1, pl, sizeof pl);
          if (pln < 0)
            {
              printf ("  %s -> payload 非法\n", args[k]);
              continue;
            }
        }
      cmd = (unsigned) strtoul (buf, NULL, 16);
      if (cmd == MOC_SUB_CMD_DUMP_DATA)
        {
          printf ("  跳过 0x13 (会锁死设备)\n");
          continue;
        }
      if (transact (h, (uint8_t) cmd, pln ? pl : NULL, (size_t) pln,
                    rsp, &fr, CMD_TIMEOUT_MS, 1) != 0)
        {
          printf ("  %02X (pl %d) -> 无应答\n", cmd, pln);
          continue;
        }
      printf ("  %02X (pl %d) ev=0x%02X len=%zu", cmd, pln, fr.event,
              fr.payload_len);
      if (fr.payload_len >= 4)
        {
          printf ("  pl[0:24]:");
          for (size_t i = 0; i < fr.payload_len && i < 24; i++)
            printf (" %02x", fr.payload[i]);
        }
      printf ("\n");
    }
  return 0;
}

/* ---------------------------------------------------------------
 * 持续监听 bulk IN, 打印设备「主动推送」的每一帧。
 *
 * 为什么要这个: 配置描述符里只有 2 个 bulk 端点、没有中断端点,
 * 所以手指触发 / 图像块 / 比对结果都是设备自己往 IN 端点推的,
 * 不是对某条命令的一问一答。用 raw 那种「发一条收一条」的模型
 * 看不到这些推送, 于是会误判成「采集没触发」。
 * --------------------------------------------------------------- */
static int
monitor_in (libusb_device_handle *h, int seconds, const char *tag)
{
  uint8_t buf[RECV_URB_LEN];
  struct timespec t0, now;
  int n_frames = 0;

  clock_gettime (CLOCK_MONOTONIC, &t0);
  printf ("-- 监听 bulk IN %d 秒 (%s), 期间请不要松手/不要动模组 --\n",
          seconds, tag);

  for (;;)
    {
      int transferred = 0;
      int r;

      clock_gettime (CLOCK_MONOTONIC, &now);
      double elapsed = (double) (now.tv_sec - t0.tv_sec)
                       + (double) (now.tv_nsec - t0.tv_nsec) / 1e9;
      if (elapsed >= (double) seconds)
        break;

      r = libusb_bulk_transfer (h, MOC5159_EP_IN, buf, RECV_URB_LEN,
                                &transferred, 500);
      if (r == LIBUSB_ERROR_TIMEOUT)
        continue;                      /* 500ms 内没东西, 正常 */
      if (r != 0)
        {
          printf ("   监听中断: %s\n", libusb_error_name (r));
          return -1;
        }
      if (transferred <= 0)
        continue;

      printf ("   [+%.2fs] IN %d 字节:", elapsed, transferred);
      for (int i = 0; i < transferred && i < 16; i++)
        printf (" %02x", buf[i]);
      if (transferred > 16)
        printf (" ...");

      if (g_trace)
        {
          printf ("\n");
          hexdump ("      整帧", buf, (size_t) transferred);
        }

      Moc5159Frame fr;
      if (transferred == MOC5159_FRAME_SIZE &&
          moc5159_frame_parse (buf, (size_t) transferred, &fr) == 0)
        {
          if (fr.cmd == 0xFF)
            printf ("   (尾帧, 忽略)\n");
          else
            {
              printf ("   cmd=0x%02X ev=0x%02X len=%zu\n",
                      fr.cmd, fr.event, fr.payload_len);
              n_frames++;
              if (fr.payload_len > 0)
                hexdump ("      payload", fr.payload,
                         fr.payload_len > 32 ? 32 : fr.payload_len);
            }
        }
      else
        printf ("   (非 128 字节定长帧 / 未识别的数据)\n");
    }

  printf ("-- 监听结束: 收到 %d 个有效帧 --\n", n_frames);
  if (n_frames == 0)
    printf ("   设备全程没推任何帧 => 传感器确实没有开始出图\n");
  return n_frames;
}

/* 完整走一遍验证采集流程, 并把设备推上来的东西全部打印出来 */
static int
cmd_cap (libusb_device_handle *h, int seconds)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t payload[36];
  uint8_t frame[MOC5159_FRAME_SIZE];
  const uint8_t guid[16] = MOC5159_WINBIO_DB_GUID;

  memset (payload, 0, sizeof payload);
  memcpy (payload, guid, 16);

  if (wake_ready (h, 50) != 0)
    fprintf (stderr, "   [!] 唤醒失败, 仍继续尝试\n");

  printf ("== 1) DB_OPEN_DB ==\n");
  if (transact (h, MOC_SUB_CMD_DB_OPEN_DB, payload, sizeof payload,
                rsp, &fr, CMD_TIMEOUT_MS, 1) != 0)
    {
      /* 设备刚被 claim 时头几条命令可能全部 NAK, 再等一会儿重来一次 */
      printf ("   第一次无响应, 300ms 后再试\n");
      msleep (300);
      if (wake_ready (h, 50) != 0)
        fprintf (stderr, "   [!] 仍唤不醒\n");
      if (transact (h, MOC_SUB_CMD_DB_OPEN_DB, payload, sizeof payload,
                    rsp, &fr, CMD_TIMEOUT_MS, 1) != 0)
        {
          printf ("   DB_OPEN 仍失败 -- 数据库没打开, 采集命令不会被接受, 先别按手指\n");
          return 1;
        }
    }
  printf ("   ev=0x%02X %s\n", fr.event,
          (fr.event == MOC_EV_OK ||
           fr.event == MOC_EV_DB_HEADER_DUPLICATED) ? "(OK)" : "(非预期)");

  /*
   * 注意: Windows 抓包里 CANCEL(0x0C) 只出现在会话结束处, 采集前从不发。
   * 之前这里无脑先 CANCEL, 很可能把传感器置成了停止态, 导致 0xC0 永远不采。
   * 默认不再发, 需要时用 --cancel 手动打开做对照。
   */
  if (g_cancel_first)
    {
      printf ("== 2) CANCEL (--cancel 指定, 对照实验) ==\n");
      printf ("   %s\n", transact (h, MOC_SUB_CMD_CANCEL, NULL, 0, rsp, &fr,
                                  CMD_TIMEOUT_MS, 1) == 0
                         ? "有应答" : "无响应");
    }
  else
    printf ("== 2) 跳过 CANCEL (与 Windows 行为一致) ==\n");

  /* capture 的 ACK 和设备主动推送可能挤在一起, 所以只发不收, 交给 monitor 统一读 */
  printf ("== 3) VERIFY_CAPTURE_AND_REPORT (0xC0) 只发不收 ==\n");
  moc5159_frame_encode (MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT, NULL, 0, frame);
  drain_in (h, 30);
  wake_ready (h, 20);
  for (int attempt = 1; ; attempt++)
    {
      if (frame_send (h, frame, g_zlp) == 0)
        break;
      if (attempt >= 3)
        {
          printf ("   发送失败 (冷插拔头几条会这样, 再跑一次或重插)\n");
          return 1;
        }
      msleep (100 * attempt);
      drain_in (h, 30);
      wake_ready (h, 20);
    }
  printf ("   已发送\n");

  /*
   * 关键(2026-10-24 由抓包确认): 0xC0/0xC4 的应答本身就是「采集完成」信号,
   * Windows 抓包里 0xC0 的上行应答在 +3.3s 才到, 0xC4 在 +0.35~0.54s。
   * 所以这里必须用长超时等一帧, 而不是等"设备主动推送"。
   */
  printf ("== 4) 现在把手指按在模组上, 最多等 %d 秒 ==\n", seconds);
  if (frame_recv (h, 0, rsp, &fr, seconds * 1000) == 0)
    {
      printf ("   应答: cmd=0x%02X ev=0x%02X len=%zu\n",
              fr.cmd, fr.event, fr.payload_len);

      if (fr.cmd == 0xFF)
        printf ("   => 固件报错帧 (ev=0x%02X), 不是采集结果\n", fr.event);
      else if (fr.event == MOC_EV_OK)
        printf ("   => 采集成功 (ev=0x01)\n");
      else if (fr.event == MOC_EV_ALGO_IMAGE_NOT_QUALIFIED)
        printf ("   => 图像质量不合格 (0xA0), 换手指/换角度重试\n");
      else if (fr.event == MOC_EV_CANCEL)
        printf ("   => 被取消 (0x0D)\n");
      else if (fr.event == MOC_EV_DEVICE_USB_TIMEOUT)
        printf ("   => 设备侧超时 (0xD0), 没检测到手指\n");
      else
        printf ("   => 其它事件码, 需要标定\n");

      if (fr.payload_len > 0)
        hexdump ("   payload", fr.payload, fr.payload_len);
    }
  else
    {
      printf ("   等了 %d 秒没有应答 —— 采集没有完成\n", seconds);
    }

  printf ("== 5) 状态轮询 0x30 ==\n");
  if (transact (h, 0x30, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS, 1) == 0 &&
      fr.payload_len >= 4)
    printf ("   status=%02X %02X %02X %02X  %s\n",
            fr.payload[0], fr.payload[1], fr.payload[2], fr.payload[3],
            fr.payload[0] ? "(有待取结果)" : "(空闲)");
  else
    printf ("   无应答\n");

  return 0;
}

/*
 * 统一收尾。实测: 发过 0x0C(CANCEL) 之后, 传感器对按压就再也不响应了
 * (0x30 一直 00, 只有断电拔插才恢复), 所以调试时一律用 --no-cancel 跳过它。
 */
static void
moc_cancel (libusb_device_handle *h)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;

  if (g_no_cancel)
    {
      printf ("   (已跳过 CANCEL)\n");
      return;
    }
  transact_quiet (h, MOC_SUB_CMD_CANCEL, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS);
}

/*
 * 高频轮询 0x30, 观察手指在位状态的跳变。
 * 依据(抓包统计): Windows 把 0x30 轮询了 341 次, 取值只有 01 和 00 两种,
 * 且每次发采集命令之前必然先看到一次 00 —— 所以它是指挥采集时序的开关。
 */
static int
cmd_poll (libusb_device_handle *h, int seconds)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  int64_t start = now_ms ();
  int64_t deadline = start + (int64_t) seconds * 1000;
  uint32_t last = 0xFFFFFFFFu;
  int n = 0, nerr = 0;

  printf ("-- 轮询 0x30 %d 秒, 请中途把手指按上去保持几秒再抬起 --\n", seconds);
  while (now_ms () < deadline)
    {
      uint32_t v;

      if (transact (h, 0x30, NULL, 0, rsp, &fr, 500, 0) != 0 ||
          fr.payload_len < 4)
        {
          nerr++;
          msleep (20);
          continue;
        }

      v = (uint32_t) fr.payload[0] | ((uint32_t) fr.payload[1] << 8) |
          ((uint32_t) fr.payload[2] << 16) | ((uint32_t) fr.payload[3] << 24);
      n++;
      if (v != last)
        {
          printf ("  [%5.2fs] status=0x%08X\n",
                  (double) (now_ms () - start) / 1000.0, v);
          last = v;
        }
      msleep (15);
    }

  printf ("-- 轮询结束: %d 次成功, %d 次失败 --\n", n, nerr);
  return nerr > n ? 1 : 0;
}

/* 建录入会话后持续轮询 0x30, 用来标定这个状态字的真实语义:
 * 手上 / 手抬起 分别对应什么值。 */
/* 分阶段标定 0x30 语义: 建一次会话, 连续轮询 24 秒, 由工具自己打阶段标记,
 * 这样「手指在位 / 抬起」和人说话的时机无关, 只和日志时间轴有关。
 *   A: 0~8s   不要碰
 *   B: 8~16s  按住不放
 *   C: 16~24s 抬起 */
static int
cmd_phase (libusb_device_handle *h, int seconds){
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  int64_t start, deadline;
  uint32_t last = 0xFFFFFFFFu;
  int n = 0, nerr = 0, phase = -1;
  int cnt[3][2] = { { 0 } };

  if (wake_ready (h, 20) != 0)
    printf ("SETUP 没回 0x5C (继续)\n");

  printf ("== 准备 15 秒: 这期间随便, 之后立刻进入阶段 A ==\n");
  fflush (stdout);
  msleep (15000);

  if (transact_quiet (h, 0xD5, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS) != 0)
    printf ("  D5 无应答\n");
  for (int i = 0; i < 3; i++)
    {
      uint8_t idx = (uint8_t) i;
      if (transact_quiet (h, 0xD9, &idx, 1, rsp, &fr, CMD_TIMEOUT_MS) != 0)
        printf ("  D9%02X 无应答\n", i);
    }
  if (transact_quiet (h, MOC_SUB_CMD_CREATE_ENROLLMENT, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0 || fr.event != MOC_EV_OK)
    {
      printf ("C2 失败, 中止\n");
      return 1;
    }
  printf ("会话已建 (C2 ok), 开始 %d 秒轮询\n", seconds);

  start = now_ms ();
  deadline = start + (int64_t) seconds * 1000;

  while (now_ms () < deadline)
    {
      int64_t el = now_ms () - start;
      uint32_t v;
      int ph = (int) (el / 8000);

      if (ph > 2)
        ph = 2;
      if (ph != phase)
        {
          phase = ph;
          printf ("\n== 阶段 %c 开始 (t=%.1fs) ==\n", 'A' + phase,
                  (double) el / 1000.0);
        }

      if (transact_quiet (h, 0x30, NULL, 0, rsp, &fr, 500) != 0 ||
          fr.payload_len < 4)
        {
          nerr++;
          msleep (20);
          continue;
        }

      v = (uint32_t) fr.payload[0] | ((uint32_t) fr.payload[1] << 8) |
          ((uint32_t) fr.payload[2] << 16) | ((uint32_t) fr.payload[3] << 24);
      n++;
      cnt[phase][v == 0 ? 0 : 1]++;
      if (v != last)
        {
          printf ("  [%5.2fs] 0x30 -> 0x%08X  (第 %d 次)\n",
                  (double) el / 1000.0, v, n);
          last = v;
        }
      msleep (2);
    }

  printf ("\n== 各阶段统计 (00 次数 / 非 00 次数) ==\n");
  for (int i = 0; i < 3; i++)
    printf ("  阶段 %c: %4d / %4d\n", 'A' + i, cnt[i][0], cnt[i][1]);
  printf ("-- 结束: %d 次成功, %d 次失败 --\n", n, nerr);

  moc_cancel (h);
  return 0;
}

/* 分阶段标定 0x30 语义（旧版占位） */
static int
cmd_p2 (libusb_device_handle *h, int seconds)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t dbsetup[36];
  const uint8_t guid[16] = MOC5159_WINBIO_DB_GUID;
  int64_t start, deadline;
  uint32_t last = 0xFFFFFFFFu;
  int n = 0, nerr = 0;

  if (wake_ready (h, 20) != 0)
    printf ("SETUP 没回 0x5C (继续)\n");

  /* 严格照抄 Windows 会话前置: 不发 DB_OPEN(0xDB), 只做 D5 + D9 x3。
   * 抓包证据 (hello_enroll_3412, 20.585~20.590s): D5 -> D9(00) -> D9(01) ->
   * D9(02) -> C2, 全程没有 0xDB。 */
  printf ("== Windows 前置: D5 + D9(00/01/02) ==\n");
  {
    int i, ok = 0;
    if (transact_quiet (h, 0xD5, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS) == 0)
      {
        printf ("  D5  -> ev=0x%02X len=%zu\n", fr.event, fr.payload_len);
        ok++;
      }
    for (i = 0; i < 3; i++)
      {
        uint8_t idx = (uint8_t) i;
        if (transact_quiet (h, 0xD9, &idx, 1, rsp, &fr, CMD_TIMEOUT_MS) == 0)
          {
            printf ("  D9%02X -> ev=0x%02X len=%zu\n", i, fr.event, fr.payload_len);
            ok++;
          }
        else
          printf ("  D9%02X -> 无应答\n", i);
      }
    printf ("  前置完成 %d/4\n", ok);
  }
  (void) dbsetup;
  (void) guid;

  if (transact_quiet (h, MOC_SUB_CMD_CREATE_ENROLLMENT, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("CREATE_ENROLLMENT 无应答, 中止\n");
      return 1;
    }
  printf ("CREATE_ENROLLMENT ev=0x%02X len=%zu\n", fr.event, fr.payload_len);

  printf ("-- 会话已建, 轮询 0x30 %d 秒。请: 空手 3 秒 -> 按住 5 秒 -> 抬起 3 秒 -> 再按住 5 秒 --\n",
          seconds);
  start = now_ms ();
  deadline = start + (int64_t) seconds * 1000;

  while (now_ms () < deadline)
    {
      uint32_t v;

      if (transact_quiet (h, 0x30, NULL, 0, rsp, &fr, 500) != 0 ||
          fr.payload_len < 4)
        {
          nerr++;
          msleep (20);
          continue;
        }

      v = (uint32_t) fr.payload[0] | ((uint32_t) fr.payload[1] << 8) |
          ((uint32_t) fr.payload[2] << 16) | ((uint32_t) fr.payload[3] << 24);
      n++;
      if (v != last)
        {
          size_t i;
          printf ("  [%5.2fs] 0x30 -> 0x%08X  (第 %d 次, len=%zu, payload=",
                  (double) (now_ms () - start) / 1000.0, v, n, fr.payload_len);
          for (i = 0; i < fr.payload_len && i < 24; i++)
            printf ("%s%02x", i ? " " : "", fr.payload[i]);
          printf (")\n");
          last = v;
        }
      msleep (2);
    }

  printf ("-- 结束: %d 次成功, %d 次失败 --\n", n, nerr);
  moc_cancel (h);
  return 0;
}

/* ---------- cap3: 常驻 IN 读线程 + 采集 ----------
 *
 * 依据: 早前 armtest 实验证明「先挂常驻 IN URB 再发 OUT, OUT 立刻成功」,
 * 说明该固件的 IN 侧必须始终有未完成的 URB; 主机临时挂一个再收, 中间的空窗
 * 会让设备推上来的帧直接丢掉。Windows 驱动同样是常驻读循环。
 * 这里用一个读线程持续重挂 128 字节 IN URB, 主线程只负责发命令。
 */
typedef struct
{
  libusb_device_handle *h;
  volatile int stop;
  volatile int nframes;
  int64_t t0;
} ReaderCtx;

static void *
reader_thread (void *arg)
{
  ReaderCtx *ctx = arg;
  uint8_t buf[MOC5159_FRAME_SIZE];

  while (!ctx->stop)
    {
      int transferred = 0;
      int r = libusb_bulk_transfer (ctx->h, MOC5159_EP_IN, buf,
                                    sizeof buf, &transferred, 300);

      if (r == 0 && transferred > 0)
        {
          int64_t dt = now_ms () - ctx->t0;
          size_t len = (transferred >= 2) ? (size_t) (buf[0] | (buf[1] << 8)) : 0;
          size_t i;

          ctx->nframes++;
          flockfile (stdout);
          printf ("  [%6.3fs] IN %d 字节  cmd=0x%02X ev=0x%02X plen=%zu\n",
                  (double) dt / 1000.0, transferred,
                  transferred > 2 ? buf[2] : 0,
                  transferred > 4 ? buf[4] : 0, len);
          printf ("            ");
          for (i = 0; i < (size_t) transferred && i < 32; i++)
            printf ("%s%02x", i ? " " : "", buf[i]);
          if (transferred > 32)
            printf (" ...");
          printf ("\n");
          funlockfile (stdout);
        }
      else if (r != 0 && r != LIBUSB_ERROR_TIMEOUT)
        {
          if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO)
            break;
        }
    }
  return NULL;
}

static int
cmd_cap3 (libusb_device_handle *h, int seconds, uint8_t capture_cmd)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  uint8_t frame[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  ReaderCtx ctx;
  pthread_t th;
  int64_t deadline;
  int i;

  /* 0) 预热: 通道在 CANCEL / 采集挂起之后要几秒才恢复, 先轮询到能应答为止,
   *    否则会把「通道没就绪」误判成「采集没触发」。 */
  printf ("== 0) 预热: 等通道可用 ==\n");
  {
    int tries;
    for (tries = 1; tries <= 30; tries++)
      {
        if (transact_quiet (h, 0x30, NULL, 0, rsp, &fr, 400) == 0)
          break;
        msleep (300);
      }
    if (tries > 30)
      {
        printf ("   通道 9 秒内未就绪, 中止 (试 clear_halt 或拔插)\n");
        return 1;
      }
    printf ("   第 %d 次尝试成功\n", tries);
  }

  /* 1) 会话前置: 与 Windows 抓包一致 (D5 + D9 x3 + C2) */
  printf ("== 1) 会话前置 D5 + D9(00/01/02) + C2 ==\n");
  transact_quiet (h, 0xD5, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS);
  for (i = 0; i < 3; i++)
    {
      uint8_t idx = (uint8_t) i;
      transact_quiet (h, 0xD9, &idx, 1, rsp, &fr, CMD_TIMEOUT_MS);
    }
  if (transact_quiet (h, MOC_SUB_CMD_CREATE_ENROLLMENT, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   C2 无应答, 中止\n");
      return 1;
    }
  printf ("   C2 ev=0x%02X\n", fr.event);

  /* 2) 启动常驻读线程, 之后所有 IN 都由它接 */
  ctx.h = h;
  ctx.stop = 0;
  ctx.nframes = 0;
  ctx.t0 = now_ms ();
  if (pthread_create (&th, NULL, reader_thread, &ctx) != 0)
    {
      printf ("   读线程创建失败\n");
      return 1;
    }
  msleep (50);

  /* 3) 发采集命令 (只发不收, 应答交给读线程) */
  printf ("== 2) 发出采集命令 0x%02X (只发不收) ==\n", capture_cmd);
  moc5159_frame_encode (capture_cmd, NULL, 0, frame);
  if (frame_send (h, frame, g_zlp) != 0)
    {
      printf ("   发送失败\n");
      ctx.stop = 1;
      pthread_join (th, NULL);
      return 1;
    }
  printf ("   已发送, 请在 %d 秒内把手指按在模组上并保持 4~5 秒\n", seconds);

  /* 4) 等待 */
  deadline = now_ms () + (int64_t) seconds * 1000;
  while (now_ms () < deadline)
    msleep (100);

  ctx.stop = 1;
  pthread_join (th, NULL);

  printf ("-- 读线程共收到 %d 帧 --\n", ctx.nframes);
  if (ctx.nframes == 0)
    printf ("   0 帧 => 设备在常驻 IN URB 下依然没有主动送任何东西\n");
  return ctx.nframes == 0 ? 1 : 0;
}

/* sweep <cmdhex>: 同一条命令按不同 payload 长度/取值各试一次 (单次、短超时)。
 * 用来判定「固件里有这条命令但参数不对」还是「根本没有这条命令」。 */
static int
cmd_sweep (libusb_device_handle *h, const char *cmd_hex)
{
  static const struct
  {
    const char *name;
    uint8_t buf[8];
    size_t len;
  } vars[] = {
    { "(空)",       { 0 },                    0 },
    { "00",         { 0x00 },                 1 },
    { "01",         { 0x01 },                 1 },
    { "0000",       { 0x00, 0x00 },           2 },
    { "00000000",   { 0x00, 0x00, 0x00, 0x00 }, 4 },
    { "01000000",   { 0x01, 0x00, 0x00, 0x00 }, 4 },
  };
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  long cmd = strtol (cmd_hex, NULL, 16);
  size_t i;
  int hits = 0;

  printf ("命令 0x%02lX 的参数扫描 (单次, 每项 500ms):\n", cmd);

  /* 预热: 通道在上一轮 CANCEL 之后可能被挂住几秒, 不预热会把
   * 「通道没就绪」误读成「命令不存在」。 */
  {
    int tries;
    for (tries = 1; tries <= 30; tries++)
      {
        if (transact_quiet (h, 0x30, NULL, 0, rsp, &fr, 400) == 0)
          break;
        msleep (300);
      }
    if (tries > 30)
      {
        printf ("  通道 9 秒内未就绪 => 本次结果无效, 请加 --clear-halt 或拔插\n");
        return 1;
      }
  }

  for (i = 0; i < sizeof vars / sizeof vars[0]; i++)
    {
      int r;
      memset (&fr, 0, sizeof fr);
      /* 必须 drain: IN 侧残留帧会让固件拒绝后续 OUT (NAK 2 秒) */
      drain_in (h, 20);
      r = transact_quiet (h, (uint8_t) cmd, vars[i].buf, vars[i].len,
                          rsp, &fr, 500);
      if (r == 0)
        {
          hits++;
          printf ("  %-10s -> ev=0x%02X len=%2zu  ", vars[i].name,
                  fr.event, fr.payload_len);
          for (size_t k = 0; k < fr.payload_len && k < 16; k++)
            printf ("%02x ", fr.payload[k]);
          printf ("\n");
        }
      else
        printf ("  %-10s -> 无应答\n", vars[i].name);
      msleep (60);
    }
  if (hits == 0)
    printf ("  结论: 该命令在本固件上不存在 (全部静默丢弃)\n");
  return hits > 0 ? 0 : 1;
}

/* regs <start-hex> <count>: 用 0x22 READ_REGISTER 批量读 32 位寄存器。
 * 这是唯一能直接观察传感器(GSL6150)状态的手段。 */
static uint32_t
le32 (const uint8_t *p)
{
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
         ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void
put_le32 (uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t) (v & 0xFF);
  p[1] = (uint8_t) ((v >> 8) & 0xFF);
  p[2] = (uint8_t) ((v >> 16) & 0xFF);
  p[3] = (uint8_t) ((v >> 24) & 0xFF);
}

static int
cmd_regs (libusb_device_handle *h, uint32_t start, uint32_t count)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t pay[4];
  uint32_t a;
  int bad = 0;

  if (count > 256)
    count = 256;

  printf ("READ_REGISTER 0x%02X 读 0x%08X 起 %u 个:\n", 0x22, start, count);
  for (a = start; a < start + count; a++)
    {
      uint32_t v;
      put_le32 (pay, a);
      drain_in (h, 20);
      if (transact_quiet (h, 0x22, pay, 4, rsp, &fr, 600) != 0 ||
          fr.payload_len < 4)
        {
          printf ("  [0x%08X] 无应答\n", a);
          bad++;
          continue;
        }
      v = le32 (fr.payload);
      printf ("  [0x%08X] = 0x%08X\n", a, v);
    }
  return bad ? 1 : 0;
}

/* 发一条任意厂商控制请求: ctl <bmRequestType> <bRequest> <wValue> <wIndex> [data-hex]
 * 全部十六进制且不需要 0x 前缀。用来找 Windows 驱动上电时写的那条寄存器配置。 */
static int
cmd_ctl (libusb_device_handle *h, char **args, int nargs)
{
  uint8_t data[64];
  int datalen = 0;
  long reqtype, request, wvalue, windex;
  int r;

  if (nargs < 4)
    {
      fprintf (stderr, "用法: ctl <bmRequestType> <bRequest> <wValue> <wIndex> [data-hex]\n");
      return 1;
    }
  reqtype = strtol (args[0], NULL, 16);
  request = strtol (args[1], NULL, 16);
  wvalue = strtol (args[2], NULL, 16);
  windex = strtol (args[3], NULL, 16);

  if (nargs >= 5)
    {
      const char *s = args[4];
      size_t len = strlen (s);

      if (len % 2 != 0 || len / 2 > sizeof data)
        {
          fprintf (stderr, "data-hex 长度必须为偶数且 <= %zu 字节\n", sizeof data);
          return 1;
        }
      for (size_t k = 0; k < len; k += 2)
        {
          char byte[3] = { s[k], s[k + 1], 0 };
          data[datalen++] = (uint8_t) strtol (byte, NULL, 16);
        }
    }

  printf ("== 控制请求 bmRequestType=0x%02lX bRequest=0x%02lX "
          "wValue=0x%04lX wIndex=0x%04lX len=%d ==\n",
          reqtype, request, wvalue, windex, datalen);

  /* 0x80 位 = 数据方向为 device→host, 即 IN; 否则是 OUT */
  if (reqtype & 0x80)
    {
      memset (data, 0, sizeof data);
      r = libusb_control_transfer (h, (uint8_t) reqtype, (uint8_t) request,
                                   (uint16_t) wvalue, (uint16_t) windex,
                                   data, (uint16_t) (datalen > 0 ? datalen : 1), 1000);
      if (r >= 0)
        {
          printf ("   读回 %d 字节:", r);
          for (int i = 0; i < r; i++)
            printf (" %02x", data[i]);
          printf ("\n");
        }
    }
  else
    {
      r = libusb_control_transfer (h, (uint8_t) reqtype, (uint8_t) request,
                                   (uint16_t) wvalue, (uint16_t) windex,
                                   data, (uint16_t) datalen, 1000);
    }

  printf ("   结果: %s\n", r >= 0 ? "成功" : libusb_error_name (r));
  return r >= 0 ? 0 : 1;
}

/* 轮询 0x30 直到返回 0 (手指在位/可采集)。
 * 判据来自抓包: 0xC2 之后 0x30 稳定回 01, 翻成 00 的那一刻 Windows 立刻发采集命令。
 * 返回 1 = 等到, 0 = 超时。 */
static int
wait_finger_ready (libusb_device_handle *h, int timeout_ms)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  int64_t start = now_ms ();
  int64_t deadline = start + timeout_ms;
  uint32_t last = 0xFFFFFFFFu;
  int npoll = 0;
  int seen_wait = 0;

  while (now_ms () < deadline)
    {
      uint32_t v;

      if (transact_quiet (h, 0x30, NULL, 0, rsp, &fr, 500) != 0 ||
          fr.payload_len < 4)
        {
          msleep (20);
          continue;
        }

      v = (uint32_t) fr.payload[0] | ((uint32_t) fr.payload[1] << 8) |
          ((uint32_t) fr.payload[2] << 16) | ((uint32_t) fr.payload[3] << 24);
      npoll++;

      if (v != last)
        {
          printf ("   [%5.2fs] 0x30 -> 0x%08X (%s), 第 %d 次轮询\n",
                  (double) (now_ms () - start) / 1000.0, v,
                  v == 0 ? "00 = 可采集" :
                  v == 1 ? "01 = 等待" : "其它值", npoll);
          last = v;
        }

      /* 必须等到 01 -> 00 的「边沿」才发采集:
         会话刚建立时 0x30 会先停在 00 一阵子, 这时发 C4 会被静默丢弃;
         Windows 抓包里每一次 C4/C0 之前都先看到 01, 再落回 00 才发。
         所以这里要求「先见到 01」, 之后第一次 00 才返回。 */
      if (v == 1)
        seen_wait = 1;
      else if (v == 0 && seen_wait)
        return 1;

      msleep (2);   /* 抓包里轮询周期约 17ms, 大头是传输本身 */
    }

  printf ("   [%5.2fs] 超时, 共轮询 %d 次, 最后一次 0x%08X\n",
          (double) (now_ms () - start) / 1000.0, npoll, last);
  return 0;
}

/* 判别 0xC0 到底是「阻塞等手指」还是「被固件直接丢弃」。
 * 手法: 发出 0xC0 后不等它的应答, 立刻在控制端点读 SETUP (控制传输不受 bulk NAK 影响),
 * 然后再发一条本来必定有应答的 GET_FW_VERSION (0x10):
 *   0x10 有应答 -> 设备空闲, 0xC0 被丢弃
 *   0x10 无应答 -> 设备被 0xC0 占住, 即固件真的在等手指
 */
static int
cmd_c0probe (libusb_device_handle *h)
{
  uint8_t frame[MOC5159_FRAME_SIZE];
  uint8_t rsp[MOC5159_FRAME_SIZE];
  uint8_t st = 0;
  Moc5159Frame fr;

  printf ("== 1) 基线: 0x10 应当有应答 ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_GET_FW_VERSION, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   基线就失败, 中止\n");
      return 1;
    }
  printf ("   OK ev=0x%02X len=%zu\n", fr.event, fr.payload_len);

  printf ("== 2) 只发 0xC0, 不接它的应答 ==\n");
  moc5159_frame_encode (MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT, NULL, 0, frame);
  if (frame_send (h, frame, g_zlp) != 0)
    {
      printf ("   0xC0 发不出去\n");
      return 1;
    }
  printf ("   已发出\n");

  printf ("== 3) 立刻读控制端点 SETUP (不受 bulk NAK 影响) ==\n");
  if (setup_read (h, &st) == 0)
    printf ("   SETUP=0x%02X\n", st);
  else
    printf ("   SETUP 读失败\n");

  printf ("== 4) 再发 0x10 探活 ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_GET_FW_VERSION, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) == 0)
    printf ("   >>> 有应答 => 设备空闲, 0xC0 被丢弃\n");
  else
    printf ("   >>> 无应答 => 设备被 0xC0 占住, 固件正等手指\n");

  printf ("== 5) 等 0xC0 的迟到应答 (5 秒) ==\n");
  if (frame_recv (h, MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT, rsp, &fr, 5000) == 0)
    {
      printf ("   cmd=0x%02X ev=0x%02X len=%zu\n", fr.cmd, fr.event,
              fr.payload_len);
      hexdump ("   meta", fr.payload, fr.payload_len);
    }
  else
    printf ("   5 秒内没有 0xC0 应答\n");

  return 0;
}

/* 完整一次「验证/识别」。
 * 严格照 Windows 抓包 #66..#107 的时序:
 *   0x30 轮询 -> 0xC0(VERIFY_CAPTURE_AND_REPORT) -> 0x13 02 + 2048 块交换
 *   -> 0x31 -> 0xD5 -> 0xC1(VERIFY_MATCHING_RESULT)
 * 注意: 这条路径 **不需要** 0xC2 录入会话, 抓包里 0xC0 之前没有任何 C2。
 */
static int
cmd_ident (libusb_device_handle *h, int seconds, int only)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t tx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t rx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t sub;
  int r;
  int64_t start;
  uint32_t v = 0xFFFFFFFFu;
  int npoll = 0;

  printf ("== 0) SETUP 建立会话 ==\n");
  if (wake_ready (h, 20) != 0)
    printf ("   警告: 没拿到 0x5C, 继续\n");
  else
    printf ("   0x5C OK\n");

  printf ("== 1) D5 + D9 全量遍历 (两轮) ==\n");
  int nrec = 3;

  if (transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) == 0 && fr.payload_len >= 12)
    {
      /* D5 语义 (2026-10-25 修正): [0:4] = 剩余空槽, [8:12] = 已用模板数。
         只对有效 index 下发 D9, 越界 index 回 0x26 会污染比对候选集。 */
      nrec = (int) (fr.payload[8] | (fr.payload[9] << 8) |
                    (fr.payload[10] << 16) | (fr.payload[11] << 24));
      printf ("   已用 %d 条模板, 全部加入比对集合\n", nrec);
    }

  for (int round = 1; round <= 2; round++)
    {
      if (transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                          CMD_TIMEOUT_MS) == 0)
        printf ("   [轮%d] D5 ev=0x%02X len=%zu\n", round, fr.event,
                fr.payload_len);
      else
        printf ("   [轮%d] D5 无应答\n", round);
      for (int i = 0; i < nrec; i++)
        {
          if (only >= 0 && i != only)
            continue;          /* 单候选实验: 只加载指定 index */
          sub = (uint8_t) i;
          /* Windows 的 D9 payload 是 16 字节 (u32 index + 12 字节 0),
           * 我们原来只发 1 字节 —— 芯片能回记录, 但可能不把该记录
           * 加入"比对集"。这里对齐 Windows 的 16 字节。 */
          {
            uint8_t d9pl[16];

            memset (d9pl, 0, sizeof d9pl);
            d9pl[0] = sub;
            if (transact_quiet (h, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX,
                                d9pl, sizeof d9pl,
                                rsp, &fr, CMD_TIMEOUT_MS) == 0)
              printf ("   [轮%d] D9 %02X ev=0x%02X len=%zu\n", round, sub,
                      fr.event, fr.payload_len);
            else
              printf ("   [轮%d] D9 %02X 无应答\n", round, sub);
          }
        }
    }

  printf ("== 2) 轮询 0x30 (最多 %d 秒, 00 = 可采集) ==\n", seconds);
  start = now_ms ();
  while (now_ms () - start < (int64_t) seconds * 1000)
    {
      if (transact_quiet (h, 0x30, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS) != 0 ||
          fr.payload_len < 4)
        {
          msleep (20);
          continue;
        }
      v = (uint32_t) fr.payload[0] | ((uint32_t) fr.payload[1] << 8) |
          ((uint32_t) fr.payload[2] << 16) | ((uint32_t) fr.payload[3] << 24);
      npoll++;
      if (v == 0 || npoll <= 3)
        printf ("   [%5.2fs] 0x30 -> 0x%08X (第 %d 次)\n",
                (double) (now_ms () - start) / 1000.0, v, npoll);
      if (v == 0)
        break;
      msleep (20);
    }
  if (v != 0)
    printf ("   轮询 %d 次没等到 00, 仍然尝试采集\n", npoll);

  printf ("== 3) VERIFY_CAPTURE_AND_REPORT (0xC0), Windows 约 3.3 秒 ==\n");
  printf ("   请把手指按在传感器上并保持 (最长等 %d 秒)\n",
          g_c0_timeout_ms / 1000);
  if (transact_quiet (h, MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT, NULL, 0,
                      rsp, &fr, g_c0_timeout_ms) != 0)
    {
      printf ("   %d 秒内无应答\n", g_c0_timeout_ms / 1000);
      return 1;
    }
  printf ("   ev=0x%02X len=%zu\n", fr.event, fr.payload_len);
  hexdump ("   meta", fr.payload, fr.payload_len);
  if (fr.event != MOC_EV_OK)
    {
      printf ("   采集未成功, 结束\n");
      return 1;
    }

  printf ("== 4) DUMP_DATA (0x13 02) + 2048 字节块交换 ==\n");
  sub = 0x02;
  if (transact_quiet (h, MOC_SUB_CMD_DUMP_DATA, &sub, 1, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   无应答\n");
      return 1;
    }
  memset (tx, 0x00, sizeof tx);
  memset (tx + 9, 0x01, sizeof tx - 9);
  tx[0] = 0x08; tx[1] = 0x09; tx[4] = 0xF7; tx[5] = 0x07; tx[8] = 0x13;
  r = image_exchange (h, tx, rx);
  if (r != 0)
    {
      printf ("   块交换失败: %s\n", libusb_error_name (r));
      return 1;
    }
  printf ("   图像块首 16 字节:");
  for (int i = 0; i < 16; i++)
    printf (" %02x", rx[i]);
  printf ("\n");

  printf ("== 5) 0x31 状态码 ==\n");
  if (transact_quiet (h, 0x31, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS) == 0 &&
      fr.payload_len >= 4)
    printf ("   code=0x%02X\n", fr.payload[0]);
  else
    printf ("   无应答\n");

  printf ("== 6) D5 (取库大小) ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) == 0)
    hexdump ("   D5", fr.payload, fr.payload_len);

  printf ("== 7) VERIFY_MATCHING_RESULT (0xC1) ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_VERIFY_MATCHING_RESULT, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   无应答\n");
      return 1;
    }
  printf ("   ev=0x%02X (%s) len=%zu\n", fr.event,
          fr.event == MOC_EV_VERIFY_MATCH ? "MATCH" : "非匹配", fr.payload_len);
  hexdump ("   result", fr.payload, fr.payload_len);

  /* MOC_REMATCH=1: 采集一次之后, 换候选模板重新取 C1。
   * 若芯片允许"同一张图像 vs 不同模板"重复比对, 多指解锁就能在
   * 一次按压里遍历全库; 若 C1 恒定或超时, 这条路不通。 */
  if (getenv ("MOC_REMATCH") != NULL && nrec > 0)
    {
      for (int i = 0; i < nrec; i++)
        {
          uint8_t pl[16];

          memset (pl, 0, sizeof pl);
          pl[0] = (uint8_t) i;
          transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                          CMD_TIMEOUT_MS);
          transact_quiet (h, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, pl,
                          sizeof pl, rsp, &fr, CMD_TIMEOUT_MS);
          if (transact_quiet (h, MOC_SUB_CMD_VERIFY_MATCHING_RESULT, NULL, 0,
                              rsp, &fr, CMD_TIMEOUT_MS) == 0)
            printf ("   [rematch %d] C1 ev=0x%02X len=%zu b1[8]=0x%02X b2[8]=0x%02X\n",
                    i, fr.event, fr.payload_len,
                    fr.payload_len > 16 ? fr.payload[16] : 0,
                    fr.payload_len > 32 ? fr.payload[32] : 0);
          else
            printf ("   [rematch %d] C1 无应答\n", i);
        }
    }
  return 0;
}

/* 完整单级录入: 照 Windows 抓包的真实时序 */
static int
cmd_enroll (libusb_device_handle *h, int seconds)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t tx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t rx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t dbsetup[36];
  uint8_t sub = 0x02;
  const uint8_t guid[16] = MOC5159_WINBIO_DB_GUID;
  int r;

  /* 抓包里 Windows 在整场会话开头只读一次 SETUP, 之后每条命令都不读。
     所以这里也只在开头读一次, 后续全部用 transact_quiet。 */
  printf ("== 0) SETUP 建立会话 (整场只做一次) ==\n");
  if (wake_ready (h, 20) != 0)
    printf ("   警告: 没拿到 0x5C, 继续尝试\n");
  else
    printf ("   0x5C OK\n");

  printf ("== 1) DB_OPEN_DB (0xDB) ==\n");
  memset (dbsetup, 0, sizeof dbsetup);
  memcpy (dbsetup, guid, 16);
  if (g_no_db)
    {
      printf ("   已跳过 (--no-db, 与 Windows 抓包一致: 采集前不打开模板库)\n");
    }
  else if (transact_quiet (h, MOC_SUB_CMD_DB_OPEN_DB, dbsetup, sizeof dbsetup,
                           rsp, &fr, CMD_TIMEOUT_MS) != 0)
    {
      printf ("   无应答, 中止\n");
      return 1;
    }
  else
    printf ("   ev=0x%02X%s\n", fr.event,
            fr.event == 0x27 ? " (0x27 = DB_HEADER_DUPLICATED, 正常)" : "");

  printf ("== 2) CREATE_ENROLLMENT (0xC2) ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_CREATE_ENROLLMENT, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   无应答, 中止\n");
      return 1;
    }
  printf ("   ev=0x%02X len=%zu\n", fr.event, fr.payload_len);
  if (fr.event != MOC_EV_OK)
    {
      printf ("   会话未建立, 中止\n");
      return 1;
    }

  printf ("== 3) 轮询 0x30 等手指 (最多 %d 秒) ==\n", seconds);
  printf ("   现在请把手指按在模组上并保持 3~5 秒\n");
  if (!wait_finger_ready (h, seconds * 1000))
    {
      printf ("   没等到就绪, 发 CANCEL 收尾\n");
      moc_cancel (h);
      return 1;
    }

  printf ("== 4) ENROLL_CAPTURE_AND_REPORT (0xC4), 等应答 (Windows 约 0.35~0.55s) ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_ENROLL_CAPTURE_AND_REPORT, NULL, 0, rsp, &fr,
                      8000) != 0)
    {
      printf ("   8 秒内无应答\n");
      moc_cancel (h);
      return 1;
    }
  printf ("   ev=0x%02X len=%zu\n", fr.event, fr.payload_len);
  hexdump ("   meta", fr.payload, fr.payload_len);
  if (fr.event != MOC_EV_OK)
    {
      printf ("   采集未成功, 发 CANCEL 收尾\n");
      moc_cancel (h);
      return 1;
    }

  printf ("== 5) DUMP_DATA (0x13 02) + 2048 字节块交换 ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_DUMP_DATA, &sub, 1, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   无应答\n");
      return 1;
    }
  memset (tx, 0x00, sizeof tx);
  memset (tx + 9, 0x01, sizeof tx - 9);
  tx[0] = 0x08; tx[1] = 0x09; tx[4] = 0xF7; tx[5] = 0x07; tx[8] = 0x13;
  r = image_exchange (h, tx, rx);
  if (r != 0)
    {
      printf ("   块交换失败: %s\n", libusb_error_name (r));
      return 1;
    }
  printf ("   图像块首 16 字节:");
  for (int i = 0; i < 16; i++)
    printf (" %02x", rx[i]);
  printf ("\n");

  printf ("== 6) 状态码 (0x31)  抓包里是 0x47/0x42 ==\n");
  if (transact_quiet (h, 0x31, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS) == 0 &&
      fr.payload_len >= 4)
    printf ("   code=0x%02X\n", fr.payload[0]);
  else
    printf ("   无应答\n");

  printf ("== 7) ENROLL_APPLY_IMAGE (0xC5) ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_ENROLL_APPLY_IMAGE, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   无应答\n");
      return 1;
    }
  printf ("   ev=0x%02X (%s) len=%zu\n", fr.event,
          fr.event == MOC_EV_OK ? "OK" :
          fr.event == 0xA6 ? "A6 = 已应用, 继续采集" :
          fr.event == 0xA0 ? "A0 = 质量警告" :
          fr.event == 0xA7 ? "A7 = 录入完成" : "其它",
          fr.payload_len);
  hexdump ("   progress", fr.payload, fr.payload_len);

  printf ("\n本次单级录入跑完。要提交模板需连采 3 级以上再发 0xC3 (80 字节记录)。\n");
  return 0;
}

/* ---------- 0x13 块内容解析 ---------- */

static const char *g_savedir = "/tmp/moc";

static void
save_blob (const char *tag, const uint8_t *b, size_t n)
{
  char path[256];
  FILE *f;

  snprintf (path, sizeof path, "%s/%s.bin", g_savedir, tag);
  f = fopen (path, "wb");
  if (!f)
    return;
  fwrite (b, 1, n, f);
  fclose (f);
}

static int
save_block (const char *tag, const uint8_t *b, size_t n)
{
  char path[256];
  FILE *f;

  snprintf (path, sizeof path, "%s/%s.bin", g_savedir, tag);
  f = fopen (path, "wb");
  if (!f)
    return -1;
  fwrite (b, 1, n, f);
  fclose (f);
  return 0;
}

/*
 * 2048 字节块的真实格式 (2026-10-24 逆出, 由 hello_enroll_3412 / verify_final 交叉验证):
 *
 *   [0:4]   u32  校验和 = bytes[4:] 逐字节和 (已逐样本核对, 全部吻合)
 *   [4:8]   u32  0x0554 固定
 *   [8]     u8   0x13 (命令回显)
 *   [9]     u8   0x01 (状态 OK)
 *   [10:12] u16  槽位总数
 *   [12:14] u16  0
 *   [14..]  槽位: u16 id | u8 len | payload[len], 固定 34 字节一个
 *            id 从 0 递增; 最后一个槽 len=8 (全 0) 是终止符
 *   其余补 0 到 2048
 */
static void
describe_block (const char *tag, const uint8_t *b)
{
  uint32_t sum = 0, stored = (uint32_t) b[0] | ((uint32_t) b[1] << 8) |
                             ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
  uint32_t magic = (uint32_t) b[4] | ((uint32_t) b[5] << 8) |
                   ((uint32_t) b[6] << 16) | ((uint32_t) b[7] << 24);
  unsigned slots = (unsigned) b[10] | ((unsigned) b[11] << 8);
  unsigned nz = 0, last = 0;

  for (int i = 4; i < MOC5159_IMG_BLOCK_SIZE; i++)
    sum += b[i];
  for (int i = 0; i < MOC5159_IMG_BLOCK_SIZE; i++)
    if (b[i])
      {
        nz++;
        last = (unsigned) i;
      }

  printf ("   [%s] 校验和 0x%08X (%s, 实算 0x%08X)  魔数 0x%04X  cmd=0x%02X st=0x%02X 槽位=%u\n",
          tag, stored, stored == sum ? "OK" : "不匹配", sum, magic, b[8], b[9],
          slots);
  printf ("        非零 %u 字节, 末偏移 %u, 首 20 字节: ", nz, last);
  for (int i = 0; i < 20; i++)
    printf ("%02x ", b[i]);
  printf ("\n");

  /* 槽位表: 只打有内容的 */
  if (slots > 0 && slots <= 60)
    {
      for (unsigned s = 0; s < slots; s++)
        {
          const uint8_t *r = b + 14 + s * 34;
          unsigned id = (unsigned) r[0] | ((unsigned) r[1] << 8);
          unsigned len = r[2];
          /* 实测: len 只覆盖前几个字节, len 之后还有 9 字节有效数据,
             所以这里固定打 18 字节 (id+len+15 字节载荷)。 */
          printf ("        槽 %2u id=0x%02X len=%u :", s, id, len);
          for (unsigned k = 0; k < 18; k++)
            printf (" %02x", r[3 + k]);
          printf ("\n");
        }
    }

  save_block (tag, b, MOC5159_IMG_BLOCK_SIZE);
}

/* 一次 0x13 <sub> + 2048 块交换 */
static int
grab_page (libusb_device_handle *h, uint8_t sub, const char *tag)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  uint8_t tx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t rx[MOC5159_IMG_BLOCK_SIZE];
  Moc5159Frame fr;
  int r;

  if (transact_quiet (h, MOC_SUB_CMD_DUMP_DATA, &sub, 1, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   [%s] 0x13 %02X 无应答\n", tag, sub);
      return -1;
    }
  printf ("   [%s] 0x13 %02X -> ev=0x%02X len=%zu\n", tag, sub, fr.event,
          fr.payload_len);

  memset (tx, 0x00, sizeof tx);
  memset (tx + 9, 0x01, sizeof tx - 9);
  tx[0] = 0x08; tx[1] = 0x09; tx[4] = 0xF7; tx[5] = 0x07; tx[8] = 0x13;
  r = image_exchange (h, tx, rx);
  if (r != 0)
    {
      printf ("   [%s] 块交换失败: %s\n", tag, libusb_error_name (r));
      return -1;
    }
  describe_block (tag, rx);
  return 0;
}

/*
 * imgpage — 方案 A: 枚举 DUMP_DATA(0x13) 的 sub 参数, 看能不能拿到图像/模板数据。
 *
 * 已经确认的真实时序 (Windows 抓包):
 *   ... -> C0(采集) -> 0x13 02 + 块(空, 槽位=0) -> 0x31 -> D5 -> C1
 *       -> 0x13 02 + 块(有数据! 槽位=2/3/16/20)   <-- 数据在这里
 *
 * 所以这里先用 02 走完 Windows 的完整流程, 再逐个 sub 抓一轮做对比。
 */
static int
cmd_imgpage (libusb_device_handle *h, int seconds, const char *subs)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t list[16];
  int nlist = 0;
  uint32_t v = 0xFFFFFFFFu;
  int64_t start;
  int nrec = 0;

  if (subs != NULL)
    {
      const char *p = subs;
      while (*p != '\0' && nlist < (int) sizeof list)
        {
          while (*p == ',' || *p == ' ')
            p++;
          if (*p == '\0')
            break;
          list[nlist++] = (uint8_t) strtol (p, (char **) &p, 16);
        }
    }
  if (nlist == 0)
    {
      list[nlist++] = 0x02;
    }

  printf ("== 0) SETUP + D5/D9 加载比对集合 ==\n");
  if (wake_ready (h, 20) != 0)
    printf ("   警告: 没拿到 0x5C\n");
  else
    printf ("   0x5C OK\n");

  for (int round = 1; round <= 2; round++)
    {
      uint8_t sub;
      if (transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                          CMD_TIMEOUT_MS) == 0 && fr.payload_len >= 4)
        {
          nrec = (int) (fr.payload[0] | (fr.payload[1] << 8));
          if (nrec > 32)
            nrec = 32;
        }
      for (int i = 0; i < nrec; i++)
        {
          sub = (uint8_t) i;
          transact_quiet (h, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &sub, 1,
                          rsp, &fr, CMD_TIMEOUT_MS);
        }
    }
  printf ("   库内模板 %d 条, 已加载\n", nrec);

  printf ("== 1) 轮询 0x30 (最多 %d 秒) ==\n", seconds);
  start = now_ms ();
  while (now_ms () - start < (int64_t) seconds * 1000)
    {
      if (transact_quiet (h, 0x30, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS) != 0 ||
          fr.payload_len < 4)
        {
          msleep (20);
          continue;
        }
      v = (uint32_t) fr.payload[0] | ((uint32_t) fr.payload[1] << 8) |
          ((uint32_t) fr.payload[2] << 16) | ((uint32_t) fr.payload[3] << 24);
      if (v == 0)
        break;
      msleep (20);
    }

  printf ("== 2) 采集 (0xC0), 请把手指按上去并保持 ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT, NULL, 0,
                      rsp, &fr,
                      seconds * 1000 > g_c0_timeout_ms ? seconds * 1000
                                                       : g_c0_timeout_ms) != 0)
    {
      printf ("   超时, 结束\n");
      return 1;
    }
  printf ("   ev=0x%02X len=%zu meta=", fr.event, fr.payload_len);
  for (size_t i = 0; i < fr.payload_len; i++)
    printf ("%02x ", fr.payload[i]);
  printf ("\n");
  if (fr.event != MOC_EV_OK)
    {
      printf ("   采集失败, 结束\n");
      return 1;
    }

  printf ("== 3) 采集后 (C1 之前) 读一块 ==\n");
  grab_page (h, 0x02, "pre_c1_02");

  printf ("== 4) 0x31 状态码 ==\n");
  if (transact_quiet (h, 0x31, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS) == 0 &&
      fr.payload_len >= 4)
    printf ("   code=0x%02X\n", fr.payload[0]);

  transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS);

  printf ("== 5) VERIFY_MATCHING_RESULT (0xC1) ==\n");
  if (transact_quiet (h, MOC_SUB_CMD_VERIFY_MATCHING_RESULT, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS) != 0)
    {
      printf ("   无应答\n");
      return 1;
    }
  printf ("   ev=0x%02X len=%zu\n", fr.event, fr.payload_len);

  printf ("== 6) C1 之后逐个 sub 抓块 (真实数据在这里) ==\n");
  for (int i = 0; i < nlist; i++)
    {
      char tag[32];
      snprintf (tag, sizeof tag, "post_%02x", list[i]);
      if (grab_page (h, list[i], tag) != 0)
        {
          printf ("   sub=%02X 失败, 停止后续枚举\n", list[i]);
          break;
        }
    }

  printf ("\n块已保存到 %s/ (2048 字节/块)\n", g_savedir);
  return 0;
}

/*
 * small — 走「2 字节 u16 LE 小命令」通道 (厂商驱动里的 SmallOut(code u16))。
 *
 * 逆向记录 (docs/zh-CN/04-protocol-notes.md §3) 记录了这一组 FPD_CMD:
 *   0x10 GET_WHOLE_IMAGE      0x1E GET_WHOLE_IMAGE_NO_DETECT
 *   0x11 DETECT_FINGER        0x23 TLS_GET_IMAGE
 *   0x24 SET_IMAGE_NUMBER     0x1A CHECK_DEV_READY
 * 它们不属于 128 字节帧体系, 所以这里直接裸写 2 字节, 然后尽量多地读 IN。
 *
 * 用法: small <codehex> [读超时ms] [最多读几轮]
 */
static int
cmd_small (libusb_device_handle *h, const char *codehex, int timeout_ms,
           int rounds)
{
  uint8_t cmd[2];
  uint8_t buf[2048];
  unsigned code = (unsigned) strtol (codehex, NULL, 16);
  int transferred = 0;
  int r;

  cmd[0] = (uint8_t) (code & 0xFF);
  cmd[1] = (uint8_t) ((code >> 8) & 0xFF);

  printf ("== small 0x%04X -> OUT %02x %02x ==\n", code, cmd[0], cmd[1]);
  drain_in (h, 30);

  r = libusb_bulk_transfer (h, MOC5159_EP_OUT, cmd, 2, &transferred, 1000);
  printf ("   OUT: %s, %d 字节\n", libusb_error_name (r), transferred);
  if (r != 0)
    return 1;

  for (int i = 0; i < (rounds > 0 ? rounds : 1); i++)
    {
      memset (buf, 0, sizeof buf);
      transferred = 0;
      r = libusb_bulk_transfer (h, MOC5159_EP_IN, buf, sizeof buf,
                                &transferred, timeout_ms);
      printf ("   读 #%d: %s, %d 字节", i + 1, libusb_error_name (r),
              transferred);
      if (transferred > 0)
        {
          printf (" ->");
          for (int k = 0; k < (transferred < 24 ? transferred : 24); k++)
            printf (" %02x", buf[k]);
          if (transferred > 24)
            printf (" ...");
          {
            unsigned nz = 0;
            for (int k = 0; k < transferred; k++)
              if (buf[k])
                nz++;
            printf ("  (非零 %u/%d)", nz, transferred);
          }
        }
      printf ("\n");
      if (r != 0 || transferred == 0)
        break;
    }
  return 0;
}

/*
 * multicap — 一次会话内连续采集 N 次, 每次都把
 *   采集后(pre) 块 / C1 的 120 字节 / C1 之后(post) 块
 * 落盘, 用来判断「post 块里那批 34 字节记录」在同一根手指上是否可复现。
 *
 * 用法: multicap <次数> [标签]
 */
static int
cmd_multicap (libusb_device_handle *h, int count, const char *tag)
{
  uint8_t rsp[MOC5159_FRAME_SIZE];
  Moc5159Frame fr;
  uint8_t tx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t rx[MOC5159_IMG_BLOCK_SIZE];
  uint8_t sub;
  int nrec = 0;

  printf ("== SETUP + D5/D9 ==\n");
  if (wake_ready (h, 20) != 0)
    printf ("   警告: 没拿到 0x5C\n");
  for (int round = 1; round <= 2; round++)
    {
      if (transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                          CMD_TIMEOUT_MS) == 0 && fr.payload_len >= 4)
        {
          nrec = (int) (fr.payload[0] | (fr.payload[1] << 8));
          if (nrec > 32)
            nrec = 32;
        }
      for (int i = 0; i < nrec; i++)
        {
          sub = (uint8_t) i;
          transact_quiet (h, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &sub, 1,
                          rsp, &fr, CMD_TIMEOUT_MS);
        }
    }
  printf ("   模板 %d 条已加载\n", nrec);

  for (int i = 1; i <= count; i++)
    {
      char t[64];
      int ok = 0;

      printf ("\n===== 第 %d/%d 次: 请按住传感器 3~5 秒 =====\n", i, count);
      fflush (stdout);

      for (int attempt = 1; attempt <= 2 && !ok; attempt++)
        {
          if (transact_quiet (h, MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT, NULL, 0,
                              rsp, &fr, 30000) == 0 && fr.event == MOC_EV_OK)
            {
              ok = 1;
              break;
            }
          printf ("   没采到 / 失败, 发 CANCEL 并清 halt 后重试\n");
          moc_cancel (h);
          libusb_clear_halt (h, MOC5159_EP_OUT);
          libusb_clear_halt (h, MOC5159_EP_IN);
          msleep (1500);
        }
      if (!ok)
        {
          printf ("   两次都没采到, 跳过本次\n");
          continue;
        }

      printf ("   C0 ev=0x%02X meta=", fr.event);
      for (size_t k = 0; k < fr.payload_len; k++)
        printf ("%02x ", fr.payload[k]);
      printf ("\n");

      /* pre 块 */
      memset (tx, 0x00, sizeof tx);
      memset (tx + 9, 0x01, sizeof tx - 9);
      tx[0] = 0x08; tx[1] = 0x09; tx[4] = 0xF7; tx[5] = 0x07; tx[8] = 0x13;
      sub = 0x02;
      if (transact_quiet (h, MOC_SUB_CMD_DUMP_DATA, &sub, 1, rsp, &fr,
                          CMD_TIMEOUT_MS) == 0 &&
          image_exchange (h, tx, rx) == 0)
        {
          snprintf (t, sizeof t, "%s_%d_pre", tag, i);
          describe_block (t, rx);
        }

      transact_quiet (h, 0x31, NULL, 0, rsp, &fr, CMD_TIMEOUT_MS);
      transact_quiet (h, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0, rsp, &fr,
                      CMD_TIMEOUT_MS);

      /* C1 */
      if (transact_quiet (h, MOC_SUB_CMD_VERIFY_MATCHING_RESULT, NULL, 0,
                          rsp, &fr, CMD_TIMEOUT_MS) == 0)
        {
          printf ("   C1 ev=0x%02X len=%zu  [0:8]=", fr.event, fr.payload_len);
          for (size_t k = 0; k < 8 && k < fr.payload_len; k++)
            printf ("%02x ", fr.payload[k]);
          printf ("  [108:120]=");
          for (size_t k = 108; k < fr.payload_len; k++)
            printf ("%02x ", fr.payload[k]);
          printf ("\n");
          snprintf (t, sizeof t, "%s_%d_c1", tag, i);
          save_blob (t, fr.payload, fr.payload_len);
        }

      /* post 块 —— 真实数据在这一次 */
      memset (tx, 0x00, sizeof tx);
      memset (tx + 9, 0x01, sizeof tx - 9);
      tx[0] = 0x08; tx[1] = 0x09; tx[4] = 0xF7; tx[5] = 0x07; tx[8] = 0x13;
      if (transact_quiet (h, MOC_SUB_CMD_DUMP_DATA, &sub, 1, rsp, &fr,
                          CMD_TIMEOUT_MS) == 0 &&
          image_exchange (h, tx, rx) == 0)
        {
          snprintf (t, sizeof t, "%s_%d_post", tag, i);
          describe_block (t, rx);
        }

      msleep (800);
    }

  printf ("\n全部落盘在 %s/\n", g_savedir);
  return 0;
}

/* ---------- main ---------- */
static void
usage (const char *argv0)
{
  fprintf (stderr,
           "用法: %s [-v] [--zlp] [--cancel] [--no-cancel] [--clear-halt] [--force] [-t秒数] [-T单条超时ms] <子命令> [参数]\n"
           "  info            描述符 + SETUP + 固件版本\n"
           "  zlp             对比补/不补 ZLP\n"
           "  db              DB_OPEN_DB + DB_GET_SIZE\n"
           "  list            列出设备内模板\n"
           "  dump            DUMP_DATA 图像块往返 (危险, 需 --force)\n"
           "  cap [-t秒]      验证采集全流程 (等 0xC0 应答, 默认 8 秒)\n"
           "  mon [-t秒]      只监听 bulk IN\n"
           "  poll [-t秒]     高频轮询 0x30, 看手指在位状态跳变\n"
           "  enroll [-t秒]   完整单级录入: DB_OPEN -> 0xC2 -> 轮询 -> 0xC4 -> 图像 -> 0xC5\n"
           "  imgpage [subs] 采集后枚举 0x13 的 sub (默认 02), 打印并保存 2048 块到 /tmp/moc\n"
           "  small <code> [ms] [轮] 2 字节小命令通道 (10=GET_WHOLE_IMAGE 11=DETECT_FINGER 24=SET_IMAGE_NUMBER)\n"
           "  multicap <n> [标签] 连续采 n 次, 每次存 pre/c1/post 三个 blob 到 /tmp/moc\n"
           "  c0probe         判别 0xC0 是「等手指」还是「被丢弃」\n"
           "  ident [-t秒]    完整识别: 轮询 -> 0xC0 -> 0x13 块交换 -> 0x31 -> D5 -> 0xC1 (不需要 C2)\n"
           "  cap3 [-t秒] [cmdhex]  常驻 IN 读线程 + 发采集命令 (默认 0xC4)\n"
           "  sweep <cmdhex>  同一条命令按不同 payload 各试一次\n"
           "  regs <起始hex> [个数]  用 0x22 批量读 32 位寄存器\n"
           "  p2 [-t秒]       建录入会话后持续轮询 0x30, 标定手指在位语义\n"
           "  phase [-t秒]    建会话后按 8 秒一段分三阶段轮询 0x30 (A 空手/B 按住/C 抬起)\n"
           "  ctl <rt> <req> <val> <idx> [data-hex]   任意控制请求\n"
           "  raw <cmdhex> [payload-hex]   发任意命令号 (可带 payload)\n"
           "    默认不补 ZLP; 需要对比时加 --zlp\n",
           argv0);
}

static int
subcommand_valid (const char *sub)
{
  static const char *known[] = { "info", "zlp", "db", "list", "dump",
                                 "mon", "cap", "cap3", "poll", "p2", "phase", "enroll",
                                 "ident", "c0probe", "sweep", "regs", "ctl",
                                 "raw", "del", "imgpage", "small", "multicap",
                                 "seq", NULL };

  for (int k = 0; known[k] != NULL; k++)
    if (strcmp (sub, known[k]) == 0)
      return 1;
  return 0;
}

int
main (int argc, char **argv)
{
  libusb_context *ctx = NULL;
  libusb_device_handle *h = NULL;
  const char *pos[8];
  int npos = 0;
  const char *sub;
  int r, with_zlp = 0, i, seconds = 8;

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "-v") == 0)
        g_trace = 1;
      else if (strcmp (argv[i], "-h") == 0 || strcmp (argv[i], "--help") == 0)
        {
          usage (argv[0]);
          return 0;
        }
      else if (strcmp (argv[i], "--zlp") == 0)
        with_zlp = 1;
      else if (strcmp (argv[i], "--no-zlp") == 0)
        with_zlp = 0;
      else if (strcmp (argv[i], "--cancel") == 0)
        g_cancel_first = 1;
      else if (strcmp (argv[i], "--clear-halt") == 0)
        g_clear_halt = 1;
      else if (strcmp (argv[i], "--sync-img") == 0)
        g_sync_img = 1;
      else if (strcmp (argv[i], "--force") == 0)
        g_force = 1;
      else if (strcmp (argv[i], "--no-cancel") == 0)
        g_no_cancel = 1;
      else if (strcmp (argv[i], "--no-db") == 0)
        g_no_db = 1;
      else if (strncmp (argv[i], "-t", 2) == 0 && strlen (argv[i]) > 2)
        seconds = atoi (argv[i] + 2);
      else if (strncmp (argv[i], "-T", 2) == 0 && strlen (argv[i]) > 2)
        g_timeout_ms = atoi (argv[i] + 2);
      else if (argv[i][0] != '-' && npos < (int) (sizeof pos / sizeof pos[0]))
        pos[npos++] = argv[i];
    }

  sub = npos > 0 ? pos[0] : "info";
  g_zlp = with_zlp;

  /* 用法错误不必先连设备, 否则设备不在时连帮助都看不到 */
  if (!subcommand_valid (sub))
    {
      usage (argv[0]);
      return 1;
    }

  r = libusb_init (&ctx);
  g_ctx = ctx;
  if (r != 0)
    {
      fprintf (stderr, "libusb_init 失败: %s\n", libusb_error_name (r));
      return 1;
    }

  h = libusb_open_device_with_vid_pid (ctx, MOC5159_VID, MOC5159_PID);
  if (h == NULL)
    {
      fprintf (stderr, "找不到 %04x:%04x (需要 root 权限? 或设备已被别的进程占用)\n",
               MOC5159_VID, MOC5159_PID);
      libusb_exit (ctx);
      return 1;
    }

  if (libusb_kernel_driver_active (h, MOC5159_INTERFACE) == 1)
    libusb_detach_kernel_driver (h, MOC5159_INTERFACE);

  r = libusb_claim_interface (h, MOC5159_INTERFACE);
  if (r != 0)
    {
      fprintf (stderr, "claim interface 失败: %s\n", libusb_error_name (r));
      libusb_close (h);
      libusb_exit (ctx);
      return 1;
    }

  if (g_clear_halt)
    {
      /* 端点被固件/主机控制器置成 halt 时 bulk OUT 会一直 NAK 到超时,
         现象和"命令被忽略"完全一样。clear_halt 不涉及 USB reset, 安全。 */
      int r1 = libusb_clear_halt (h, MOC5159_EP_OUT);
      int r2 = libusb_clear_halt (h, MOC5159_EP_IN);
      printf ("clear_halt: OUT=%s IN=%s\n",
              libusb_error_name (r1), libusb_error_name (r2));
    }

  if (strcmp (sub, "info") == 0)
    r = cmd_info (h);
  else if (strcmp (sub, "zlp") == 0)
    r = cmd_zlp (h);
  else if (strcmp (sub, "db") == 0)
    r = cmd_db (h);
  else if (strcmp (sub, "list") == 0)
    r = cmd_list (h);
  else if (strcmp (sub, "dump") == 0)
    r = cmd_dump (h);
  else if (strcmp (sub, "mon") == 0)
    r = monitor_in (h, seconds, "纯监听") < 0 ? 1 : 0;
  else if (strcmp (sub, "cap") == 0)
    r = cmd_cap (h, seconds);
  else if (strcmp (sub, "poll") == 0)
    r = cmd_poll (h, seconds);
  else if (strcmp (sub, "regs") == 0)
    r = cmd_regs (h, npos >= 2 ? (uint32_t) strtol (pos[1], NULL, 16) : 0,
                  npos >= 3 ? (uint32_t) strtol (pos[2], NULL, 10) : 32);
  else if (strcmp (sub, "sweep") == 0)
    r = cmd_sweep (h, npos >= 2 ? pos[1] : "20");
  else if (strcmp (sub, "cap3") == 0)
    r = cmd_cap3 (h, seconds, npos >= 2 ? (uint8_t) strtol (pos[1], NULL, 16) : 0xC4);
  else if (strcmp (sub, "p2") == 0)
    r = cmd_p2 (h, seconds);
  else if (strcmp (sub, "phase") == 0)
    r = cmd_phase (h, seconds);
  else if (strcmp (sub, "c0probe") == 0)
    r = cmd_c0probe (h);
  else if (strcmp (sub, "ident") == 0)
    r = cmd_ident (h, seconds, npos >= 2 ? atoi (pos[1]) : -1);
  else if (strcmp (sub, "enroll") == 0)
    r = cmd_enroll (h, seconds);
  else if (strcmp (sub, "imgpage") == 0)
    r = cmd_imgpage (h, seconds, npos >= 2 ? pos[1] : NULL);
  else if (strcmp (sub, "multicap") == 0)
    r = cmd_multicap (h, npos >= 2 ? atoi (pos[1]) : 4,
                      npos >= 3 ? pos[2] : "cap");
  else if (strcmp (sub, "small") == 0)
    r = cmd_small (h, npos >= 2 ? pos[1] : "10",
                   npos >= 3 ? atoi (pos[2]) : 1500,
                   npos >= 4 ? atoi (pos[3]) : 1);
  else if (strcmp (sub, "ctl") == 0)
    r = cmd_ctl (h, (char **) &pos[1], npos - 1);
  else if (strcmp (sub, "raw") == 0)
    r = cmd_raw (h, npos >= 2 ? pos[1] : "d5", npos >= 3 ? pos[2] : NULL);
  else if (strcmp (sub, "seq") == 0)
    r = cmd_seq (h, &pos[1], npos > 1 ? npos - 1 : 0);
  else if (strcmp (sub, "del") == 0)
    r = cmd_del (h, npos >= 2 ? pos[1] : "0");
  else
    {
      usage (argv[0]);   /* 上面已校验过, 逻辑上不会到这里 */
      r = 1;
    }

  libusb_release_interface (h, MOC5159_INTERFACE);
  libusb_close (h);
  libusb_exit (ctx);
  return r;
}
