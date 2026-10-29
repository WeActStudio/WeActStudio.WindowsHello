/*
 * moc5159.c — libfprint 驱动: Petaic MOC-5159 (GD32W515 + GSL6150)
 *
 * 设备: 28e9:5159 "GDMicroelectronics Touch Fingerprint Sensor"
 *       模组 MOC-5159, Windows Hello 认证模组, match-on-chip (模板与比对都在芯片内)
 *
 * 结构参考 libfprint 的 match-on-chip 驱动 (drivers/egismoc, drivers/goodixmoc):
 * 一个 FpDevice 子类 + 「每条命令一个 FpiSsm」+「每个动作一个任务 FpiSsm」。
 *
 * 传输层三条硬性要求 (实测, 见 ../docs/zh-CN/04-protocol-notes.md):
 *   1. 每条命令前读一次 SETUP 字节 (控制 IN 0xC0/0xC0, 1 字节, 期望 0x5C),
 *      既做就绪检查, 也把空闲约 2 秒就休眠的模组唤醒;
 *   2. 发命令前必须清空 IN 侧残留帧 —— 上一轮的帧没被读走时, 固件不接受新的
 *      bulk OUT, 表现为 OUT 一直 NAK 直到超时 (最容易误判成 "设备坏了" 的坑);
 *   3. bulk IN 的 URB 长度必须正好 128 —— 回帧是两个 64 字节满包, 没有短包收尾,
 *      给更大的缓冲永远等不到 "短包结束"。
 *
 * 本文件以 LGPL-2.1+ 发布, 与 libfprint 保持一致。
 */

#define FP_COMPONENT "moc5159"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "moc5159.h"
#include "moc5159-proto.h"

/* ---------------- 超时 (ms) ---------------- */
#define MOC_TIMEOUT_CTRL      2000
#define MOC_TIMEOUT_CMD       2000
#define MOC_TIMEOUT_DRAIN     60      /* 清残留: 短超时, 超时本身就是期望结果 */
#define MOC_TIMEOUT_CAPTURE   2500    /* 单次等应答; 配合 MOC_MAX_RECV_TRIES (48) 约 2 分钟 */
#define MOC_TIMEOUT_IMAGE     5000
#define MOC_MAX_DRAIN_TRIES   4
#define MOC_MAX_RECV_TRIES    12      /* 允许若干 0xFF 尾帧 */
#define MOC_MAX_WAIT_TRIES    48      /* 阻塞采集 (C0/C4): 48 × 2.5s ≈ 2 分钟等手指 */

#define MOC_MAX_ENROLL_STAGES 5
#define MOC_MAX_CAPTURE_TRIES 5

typedef struct _FpiDeviceMoc5159        FpiDeviceMoc5159;
typedef struct _FpiDeviceMoc5159Class   FpiDeviceMoc5159Class;

struct _FpiDeviceMoc5159
{
  FpDevice      parent;

  FpiSsm       *task_ssm;
  FpiSsm       *cmd_ssm;
  FpiSsm       *img_ssm;
  GCancellable *interrupt_cancellable;

  GPtrArray    *records;       /* 设备内模板记录: 每项 GByteArray(80) */
  guint         list_index;
  gboolean      session_up;    /* SETUP 已做过: Windows 整场只做一次 */
};

struct _FpiDeviceMoc5159Class
{
  FpDeviceClass parent_class;
};

GType fpi_device_moc5159_get_type (void);

G_DEFINE_TYPE (FpiDeviceMoc5159, fpi_device_moc5159, FP_TYPE_DEVICE);

#define FPI_DEVICE_MOC5159(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), fpi_device_moc5159_get_type (), FpiDeviceMoc5159))

static const FpIdEntry moc5159_id_table[] = {
  { .vid = MOC5159_VID, .pid = MOC5159_PID, .driver_data = 0 },
  { .vid = 0,           .pid = 0,           .driver_data = 0 }
};

/* 所有动作共用的收尾回调 */
static void moc_action_done (FpiSsm *ssm, FpDevice *device, GError *error);

/* =====================================================================
 * 命令通道: WAKE -> DRAIN -> SEND -> RECV
 * ===================================================================== */

typedef struct
{
  guint8 cmd;
  guint8 event;
  gsize  len;
  guint8 payload[MOC5159_PAYLOAD_MAX];
} MocRsp;

typedef void (*MocCmdCallback) (FpDevice *device,
                                MocRsp   *rsp,
                                GError   *error,
                                gpointer  user);

typedef struct
{
  MocCmdCallback cb;
  gpointer       user;
  guint8         want_cmd;
  guint          tries;
  guint          send_tries;
  guint          timeout;
  guint8         frame[MOC5159_FRAME_SIZE];
  MocRsp         rsp;
  gboolean       bare;          /* 不做 WAKE/DRAIN, 直接收发 (贴近抓包) */
} MocCmd;

#define MOC_MAX_SEND_TRIES 3

enum {
  CMD_WAKE,
  CMD_DRAIN,
  CMD_SEND,
  CMD_RECV,
  CMD_STATES,
};

static void
moc_cmd_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  MocCmd *cmd = fpi_ssm_get_data (ssm);
  g_autoptr(GError) local_error = error;

  g_assert (self->cmd_ssm == ssm);
  self->cmd_ssm = NULL;

  if (cmd && cmd->cb)
    cmd->cb (device, &cmd->rsp,
             error ? g_steal_pointer (&local_error) : NULL,
             cmd->user);
}

/* SETUP 只读: 失败不致命 (设备可能只是没醒, 后面还会重试) */
static void
moc_wake_cb (FpiUsbTransfer *transfer, FpDevice *device,
             gpointer user_data, GError *error)
{
  if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (!error)
    FPI_DEVICE_MOC5159 (device)->session_up = TRUE;

  fpi_ssm_next_state (transfer->ssm);
}

static void moc_drain_transfer (FpiSsm *ssm, FpDevice *device);

static void
moc_drain_cb (FpiUsbTransfer *transfer, FpDevice *device,
              gpointer user_data, GError *error)
{
  MocCmd *cmd = fpi_ssm_get_data (transfer->ssm);

  if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* 超时 = IN 侧已经干净, 可以发命令了 */
  if (error)
    {
      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  cmd->tries++;
  if (cmd->tries < MOC_MAX_DRAIN_TRIES)
    {
      moc_drain_transfer (transfer->ssm, device);
      return;
    }

  fp_dbg ("IN 侧残留帧过多 (%u)", cmd->tries);
  fpi_ssm_next_state (transfer->ssm);
}

static void
moc_drain_transfer (FpiSsm *ssm, FpDevice *device)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (device);

  fpi_usb_transfer_fill_bulk (transfer, MOC5159_EP_IN, MOC5159_FRAME_SIZE);
  transfer->ssm = ssm;
  transfer->short_is_error = FALSE;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           MOC_TIMEOUT_DRAIN,
                           fpi_device_get_cancellable (device),
                           moc_drain_cb, NULL);
}

static void moc_recv_transfer (FpiSsm *ssm, FpDevice *device);

static void
moc_recv_cb (FpiUsbTransfer *transfer, FpDevice *device,
             gpointer user_data, GError *error)
{
  MocCmd *cmd = fpi_ssm_get_data (transfer->ssm);
  Moc5159Frame frame;

  if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (!error && transfer->actual_length == MOC5159_FRAME_SIZE)
    {
      if (moc5159_frame_parse (transfer->buffer, MOC5159_FRAME_SIZE, &frame) == 0)
        {
          /* 0xFF 是设备在每条命令之后追加的传输尾帧, 不是应答 */
          if (frame.cmd == 0xFF && cmd->want_cmd != 0xFF)
            {
              fp_dbg ("忽略 0xFF 尾帧 (ev=0x%02X)", frame.event);
            }
          else if (cmd->want_cmd == 0 || frame.cmd == cmd->want_cmd)
            {
              cmd->rsp.cmd   = frame.cmd;
              cmd->rsp.event = frame.event;
              cmd->rsp.len   = MIN (frame.payload_len, sizeof (cmd->rsp.payload));
              memcpy (cmd->rsp.payload, frame.payload, cmd->rsp.len);
              fpi_ssm_mark_completed (transfer->ssm);
              return;
            }
          else
            {
              fp_dbg ("丢弃不属于本次事务的帧 cmd=0x%02X (期望 0x%02X)",
                      frame.cmd, cmd->want_cmd);
            }
        }
      else
        {
          fp_dbg ("丢弃校验和错误的帧");
        }
    }

  /* 阻塞采集 (C0/C4, timeout 用 MOC_TIMEOUT_CAPTURE) 允许等手指等更久 */
  guint max_tries = (cmd->timeout >= MOC_TIMEOUT_CAPTURE) ? MOC_MAX_WAIT_TRIES
                                                          : MOC_MAX_RECV_TRIES;

  cmd->tries++;
  if (cmd->tries < max_tries)
    {
      moc_recv_transfer (transfer->ssm, device);
      return;
    }

  fpi_ssm_mark_failed (transfer->ssm,
                       fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                 "设备未返回命令 0x%02X 的应答",
                                                 cmd->want_cmd));
}

static void
moc_recv_transfer (FpiSsm *ssm, FpDevice *device)
{
  MocCmd *cmd = fpi_ssm_get_data (ssm);
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (device);

  fpi_usb_transfer_fill_bulk (transfer, MOC5159_EP_IN, MOC5159_FRAME_SIZE);
  transfer->ssm = ssm;
  transfer->short_is_error = FALSE;
  fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                           cmd->timeout,
                           fpi_device_get_cancellable (device),
                           moc_recv_cb, NULL);
}

/*
 * 发送失败时重试。
 * 典型场景: 设备刚枚举完 (或刚上电) 的那几百毫秒里, OUT 会全部 NAK,
 * 表现为 LIBUSB 超时；等一会儿重来就好。已实测: 冷插拔后第一条命令必失败。
 */
static void
moc_send_cb (FpiUsbTransfer *transfer, FpDevice *device,
             gpointer user_data, GError *error)
{
  MocCmd *cmd = fpi_ssm_get_data (transfer->ssm);

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_failed (transfer->ssm, error);
          return;
        }

      cmd->send_tries++;
      if (cmd->send_tries < MOC_MAX_SEND_TRIES)
        {
          fp_dbg ("发送命令 0x%02X 失败 (%s), 第 %u 次重试",
                  cmd->want_cmd, error->message, cmd->send_tries);
          fpi_ssm_jump_to_state_delayed (transfer->ssm, CMD_WAKE, 60);
          return;
        }

      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
moc_cmd_run_state (FpiSsm *ssm, FpDevice *device)
{
  MocCmd *cmd = fpi_ssm_get_data (ssm);
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  g_autoptr(FpiUsbTransfer) transfer = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CMD_WAKE:
      /* Windows 整场只在会话开始时发一次 SETUP; 之后每条命令都不发。
         bare 命令 (0x13) 连会话检查都不做, 直接发。 */
      if (cmd->bare || self->session_up)
        {
          fpi_ssm_jump_to_state (ssm, CMD_DRAIN);
          break;
        }

      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_control (transfer,
                                     G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     MOC5159_SETUP_REQUEST, 0, 0, 1);
      transfer->ssm = ssm;
      transfer->short_is_error = FALSE;
      fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                               MOC_TIMEOUT_CTRL,
                               fpi_device_get_cancellable (device),
                               moc_wake_cb, NULL);
      break;

    case CMD_DRAIN:
      cmd->tries = 0;

      if (cmd->bare)
        {
          fpi_ssm_jump_to_state (ssm, CMD_SEND);
          break;
        }

      moc_drain_transfer (ssm, device);
      break;

    case CMD_SEND:
      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_bulk_full (transfer, MOC5159_EP_OUT,
                                       cmd->frame, MOC5159_FRAME_SIZE, NULL);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                               MOC_TIMEOUT_CMD,
                               fpi_device_get_cancellable (device),
                               moc_send_cb, NULL);
      break;

    case CMD_RECV:
      cmd->tries = 0;
      moc_recv_transfer (ssm, device);
      break;

    default:
      g_assert_not_reached ();
    }
}

/*
 * 发起一条 MOC 命令。payload 会复制进内部帧缓冲。
 * 完成时 cb 在 cmd SSM 的 done 回调里同步触发。
 */
static void
moc_transact (FpDevice       *device,
              guint8          cmd_code,
              const guint8   *payload,
              gsize           paylen,
              guint           timeout_ms,
              MocCmdCallback  cb,
              gpointer        user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  MocCmd *cmd;

  g_assert (self->cmd_ssm == NULL);

  cmd = g_new0 (MocCmd, 1);
  cmd->cb         = cb;
  cmd->user       = user;
  cmd->want_cmd   = cmd_code;
  cmd->timeout    = timeout_ms;
  moc5159_frame_encode (cmd_code, payload, paylen, cmd->frame);

  self->cmd_ssm = fpi_ssm_new (device, moc_cmd_run_state, CMD_STATES);
  fpi_ssm_set_data (self->cmd_ssm, cmd, g_free);
  fpi_ssm_start (self->cmd_ssm, moc_cmd_ssm_done);
}

/*
 * bare 版本: 不发 SETUP、不做 DRAIN, 帧直接发出去。
 * 抓包显示 0x13 是在 0xC0 应答之后 1ms 内直接发出的, 中间没有任何控制传输;
 * 而我们在采到图之后立刻插一条控制请求, 设备会当场重启 (USB 重新枚举)。
 */
static void
moc_transact_bare (FpDevice       *device,
                   guint8          cmd_code,
                   const guint8   *payload,
                   gsize           paylen,
                   guint           timeout_ms,
                   MocCmdCallback  cb,
                   gpointer        user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  MocCmd *cmd;

  g_assert (self->cmd_ssm == NULL);

  cmd = g_new0 (MocCmd, 1);
  cmd->cb       = cb;
  cmd->user     = user;
  cmd->want_cmd = cmd_code;
  cmd->timeout  = timeout_ms;
  cmd->bare     = TRUE;
  moc5159_frame_encode (cmd_code, payload, paylen, cmd->frame);

  self->cmd_ssm = fpi_ssm_new (device, moc_cmd_run_state, CMD_STATES);
  fpi_ssm_set_data (self->cmd_ssm, cmd, g_free);
  fpi_ssm_start (self->cmd_ssm, moc_cmd_ssm_done);
}

/* =====================================================================
 * 图像块交换: 2048 字节 OUT -> 2048 字节 IN
 * ===================================================================== */

typedef void (*MocImgCallback) (FpDevice *device, GError *error, gpointer user);

typedef struct
{
  MocImgCallback cb;
  gpointer       user;
  guint8         tx[MOC5159_IMG_BLOCK_SIZE];
  guint8         rx[MOC5159_IMG_BLOCK_SIZE];
} MocImg;

enum {
  IMG_SEND,
  IMG_RECV,
  IMG_STATES,
};

static void
moc_img_ssm_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  MocImg *img = fpi_ssm_get_data (ssm);
  g_autoptr(GError) local_error = error;

  g_assert (self->img_ssm == ssm);
  self->img_ssm = NULL;

  if (img && !error)
    fp_dbg ("图像块: TX %02X %02X %02X %02X %02X %02X %02X %02X %02X | "
            "RX %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            img->tx[0], img->tx[1], img->tx[2], img->tx[3], img->tx[4],
            img->tx[5], img->tx[6], img->tx[7], img->tx[8],
            img->rx[0], img->rx[1], img->rx[2], img->rx[3], img->rx[4],
            img->rx[5], img->rx[6], img->rx[7], img->rx[8]);

  if (img && img->cb)
    img->cb (device, error ? g_steal_pointer (&local_error) : NULL, img->user);
}

static void
moc_img_run_state (FpiSsm *ssm, FpDevice *device)
{
  MocImg *img = fpi_ssm_get_data (ssm);
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (device);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case IMG_SEND:
      fpi_usb_transfer_fill_bulk_full (transfer, MOC5159_EP_OUT,
                                       img->tx, MOC5159_IMG_BLOCK_SIZE, NULL);
      transfer->short_is_error = TRUE;
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                               MOC_TIMEOUT_IMAGE,
                               fpi_device_get_cancellable (device),
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case IMG_RECV:
      fpi_usb_transfer_fill_bulk_full (transfer, MOC5159_EP_IN,
                                       img->rx, MOC5159_IMG_BLOCK_SIZE, NULL);
      transfer->short_is_error = FALSE;
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (g_steal_pointer (&transfer),
                               MOC_TIMEOUT_IMAGE,
                               fpi_device_get_cancellable (device),
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    default:
      g_assert_not_reached ();
    }
}

/*
 * DUMP_DATA 事务里的 2048 字节块 (Windows 抓包 31 次逐字节恒定):
 * 头部 9 字节 + 0x01 填充。应答里是图像数据, 本驱动不需要图像内容
 * (比对在芯片内完成), 但必须读走以释放设备缓冲。
 */
static void
moc_img_fill_block (guint8 block[MOC5159_IMG_BLOCK_SIZE])
{
  /* 抓包 #88 逐字节: 08 09 00 00 F7 07 00 00 13 + 0x01 填充 (2039 字节) */
  memset (block, 0x00, MOC5159_IMG_BLOCK_SIZE);
  memset (block + 9, 0x01, MOC5159_IMG_BLOCK_SIZE - 9);
  block[0] = 0x08;
  block[1] = 0x09;
  block[4] = 0xF7;
  block[5] = 0x07;
  block[8] = 0x13;
}

static void
moc_image_exchange (FpDevice *device, MocImgCallback cb, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  MocImg *img;

  g_assert (self->img_ssm == NULL);

  img = g_new0 (MocImg, 1);
  img->cb = cb;
  img->user = user;
  moc_img_fill_block (img->tx);

  self->img_ssm = fpi_ssm_new (device, moc_img_run_state, IMG_STATES);
  fpi_ssm_set_data (self->img_ssm, img, g_free);
  fpi_ssm_start (self->img_ssm, moc_img_ssm_done);
}

/* =====================================================================
 * FpPrint <-> 设备模板号
 * ===================================================================== */

static guint32
moc_le32 (const guint8 *p)
{
  return (guint32) p[0] | ((guint32) p[1] << 8) |
         ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}

static void
moc_put_le32 (guint8 *p, guint32 v)
{
  p[0] = (guint8) (v & 0xFF);
  p[1] = (guint8) ((v >> 8) & 0xFF);
  p[2] = (guint8) ((v >> 16) & 0xFF);
  p[3] = (guint8) ((v >> 24) & 0xFF);
}

static void
moc_print_set_id (FpPrint *print, guint32 template_id)
{
  guint8 id[4];
  g_autofree gchar *desc = NULL;
  GVariant *id_var, *data;

  moc_put_le32 (id, template_id);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);

  id_var = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, id, sizeof (id), 1);
  data = g_variant_new ("(@ay)", id_var);

  g_object_set (print, "fpi-data", data, NULL);
  desc = g_strdup_printf ("MOC-5159#%u", template_id);
  g_object_set (print, "description", desc, NULL);
}

static gboolean
moc_print_get_id (FpPrint *print, guint32 *template_id)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) id_var = NULL;
  const guint8 *id;
  gsize len;

  g_object_get (print, "fpi-data", &data, NULL);
  if (data == NULL || !g_variant_check_format_string (data, "(@ay)", FALSE))
    return FALSE;

  g_variant_get (data, "(@ay)", &id_var);
  id = g_variant_get_fixed_array (id_var, &len, 1);
  if (len < 4)
    return FALSE;

  *template_id = moc_le32 (id);
  return TRUE;
}

/* 通用「下一步」回调 */
static void
moc_next_state_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  fpi_ssm_next_state (self->task_ssm);
}

/* =====================================================================
 * open: 挂载模板库 + 读固件版本
 * ===================================================================== */

enum {
  OPEN_DB,
  OPEN_CREATE,
  OPEN_FW,
  OPEN_STATES,
};

static void
moc_open_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  g_assert (self->task_ssm == ssm);
  self->task_ssm = NULL;

  fpi_device_open_complete (device, error);
}

static void
moc_open_db_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  /* 0x01 = 新建成功, 0x27 = 库已存在, 0x29 = 库为空(清库后首次打开), 都算正常 */
  if (rsp->event != MOC_EV_OK && rsp->event != MOC_EV_DB_HEADER_DUPLICATED &&
      rsp->event != MOC_EV_DB_HEADER_EMPTY)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "DB_OPEN 失败: ev=0x%02X",
                                                     rsp->event));
      return;
    }

  fp_dbg ("模板库已挂载 (ev=0x%02X)", rsp->event);

  /* 0x29 = 库头为空 (0xD7 清库后 / 从未建过库)。
   * Windows 这时会立刻发 0xDA 建库 (delete_01.pcapng #155~#158)。
   * 不建库芯片处于降级状态: 多条模板只有一条能被比对命中
   * —— 这正是"只有某个手指能解锁"的根因。 */
  if (rsp->event == MOC_EV_DB_HEADER_EMPTY)
    {
      fp_info ("库头为空, 补发 0xDA 建库");
      fpi_ssm_jump_to_state (self->task_ssm, OPEN_CREATE);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
moc_open_create_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  /* 0x01 = 建立成功, 0x27 = 已存在, 都正常 */
  if (rsp->event != MOC_EV_OK && rsp->event != MOC_EV_DB_HEADER_DUPLICATED)
    fp_warn ("DB_CREATE 非预期应答 ev=0x%02X (继续)", rsp->event);
  else
    fp_info ("建库完成 (ev=0x%02X)", rsp->event);

  fpi_ssm_next_state (self->task_ssm);
}

static void
moc_open_fw_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  if (!error && rsp->len >= 4)
    fp_info ("固件版本 %02x %02x %02x %02x",
             rsp->payload[0], rsp->payload[1],
             rsp->payload[2], rsp->payload[3]);

  fpi_ssm_next_state (self->task_ssm);
}

static void
moc_open_run_state (FpiSsm *ssm, FpDevice *device)
{
  static const guint8 db_guid[16] = MOC5159_WINBIO_DB_GUID;
  guint8 payload[36];

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_DB:
      /* 新会话: SETUP 需要重做一次 (设备重新枚举后旧会话已失效) */
      FPI_DEVICE_MOC5159 (device)->session_up = FALSE;
      memset (payload, 0, sizeof payload);
      memcpy (payload, db_guid, sizeof db_guid);
      moc_transact (device, MOC_SUB_CMD_DB_OPEN_DB, payload, sizeof payload,
                    MOC_TIMEOUT_CMD, moc_open_db_cb, NULL);
      break;

    case OPEN_CREATE:
      /* 0xDA 建库: DB GUID(16) + u32 8 + 库内容 GUID(16) */
      {
        static const guint8 db_guid2[16] = MOC5159_WINBIO_DB_GUID;
        static const guint8 content_guid[16] = MOC5159_DB_CONTENT_GUID;
        guint8 create[36];

        memset (create, 0, sizeof create);
        memcpy (create, db_guid2, 16);
        moc_put_le32 (create + 16, 8);
        memcpy (create + 20, content_guid, 16);
        moc_transact (device, MOC_SUB_CMD_DB_CREATE_DB, create, sizeof create,
                      MOC_TIMEOUT_CMD, moc_open_create_cb, NULL);
      }
      break;

    case OPEN_FW:
      moc_transact (device, MOC_SUB_CMD_GET_FW_VERSION, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_open_fw_cb, NULL);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
moc_probe (FpDevice *device)
{
  GUsbDevice *usb_dev;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *serial = NULL;

  usb_dev = fpi_device_get_usb_device (device);

  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL, g_steal_pointer (&error));
      return;
    }

  if (g_usb_device_get_serial_number_index (usb_dev) > 0)
    serial = g_usb_device_get_string_descriptor (
      usb_dev, g_usb_device_get_serial_number_index (usb_dev), NULL);

  fpi_device_set_nr_enroll_stages (device, MOC_MAX_ENROLL_STAGES);

  g_usb_device_close (usb_dev, NULL);
  fpi_device_probe_complete (device, serial, NULL, NULL);
}

static void
moc_open (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  g_autoptr(GError) error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device),
                                     MOC5159_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (device, g_steal_pointer (&error));
      return;
    }

  self->interrupt_cancellable = g_cancellable_new ();

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, moc_open_run_state, OPEN_STATES);
  fpi_ssm_start (self->task_ssm, moc_open_done);
}

static void
moc_close (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  g_autoptr(GError) error = NULL;

  g_clear_object (&self->interrupt_cancellable);
  g_clear_pointer (&self->records, g_ptr_array_unref);

  g_usb_device_release_interface (fpi_device_get_usb_device (device),
                                  MOC5159_INTERFACE, 0, &error);
  fpi_device_close_complete (device, g_steal_pointer (&error));
}

static void
moc_cancel (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  fp_dbg ("取消当前动作");

  if (self->interrupt_cancellable)
    g_cancellable_cancel (self->interrupt_cancellable);

  g_cancellable_cancel (fpi_device_get_cancellable (device));
}

static void
moc_suspend (FpDevice *device)
{
  moc_cancel (device);
  fpi_device_suspend_complete (device, NULL);
}

/* 唤醒后需要重新握手 + 重新挂载模板库: 下一次动作开头会发 CANCEL/DB_OPEN */
static void
moc_resume (FpDevice *device)
{
  fpi_device_resume_complete (device, NULL);
}

/* =====================================================================
 * list
 * ===================================================================== */

enum {
  LIST_SIZE,
  LIST_RECORD,
  LIST_DONE,
  LIST_STATES,
};

static void
moc_list_run_state (FpiSsm *ssm, FpDevice *device);

static void
moc_list_record_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  guint index = GPOINTER_TO_UINT (user);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (rsp->event == MOC_EV_OK && rsp->len >= 4 + MOC5159_RECORD_SIZE)
    {
      GByteArray *rec = g_byte_array_new ();

      g_byte_array_append (rec, rsp->payload + 4, MOC5159_RECORD_SIZE);
      g_ptr_array_index (self->records, index) = rec;
    }
  else
    {
      fp_dbg ("读取模板记录 %u 失败 (ev=0x%02X len=%zu)",
              index, rsp->event, rsp->len);
    }

  self->list_index++;
  fpi_ssm_jump_to_state (self->task_ssm, LIST_RECORD);
}

static void
moc_list_size_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  guint32 used;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (rsp->event != MOC_EV_OK || rsp->len < 12)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "DB_GET_SIZE 失败"));
      return;
    }

  used = moc_le32 (rsp->payload + 8);
  fp_dbg ("设备内模板数: %u", used);

  g_clear_pointer (&self->records, g_ptr_array_unref);
  self->records = g_ptr_array_new_with_free_func ((GDestroyNotify) g_byte_array_unref);
  for (guint32 i = 0; i < used; i++)
    g_ptr_array_add (self->records, NULL);

  self->list_index = 0;
  fpi_ssm_next_state (self->task_ssm);
}

static void
moc_list_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case LIST_SIZE:
      moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_list_size_cb, NULL);
      break;

    case LIST_RECORD:
      if (self->list_index >= self->records->len)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      {
        guint8 idx = (guint8) self->list_index;
        moc_transact (device, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &idx, 1,
                      MOC_TIMEOUT_CMD, moc_list_record_cb,
                      GUINT_TO_POINTER (self->list_index));
      }
      break;

    case LIST_DONE:
      {
        g_autoptr(GPtrArray) prints =
          g_ptr_array_new_with_free_func (g_object_unref);

        for (guint i = 0; i < self->records->len; i++)
          {
            GByteArray *rec = g_ptr_array_index (self->records, i);
            FpPrint *print;

            if (rec == NULL || rec->len < MOC5159_RECORD_SIZE)
              continue;

            print = fp_print_new (device);
            moc_print_set_id (print, moc_le32 (rec->data + MOC5159_RECORD_ID_OFF));
            g_ptr_array_add (prints, g_object_ref_sink (print));
          }

        fpi_device_list_complete (device, g_steal_pointer (&prints), NULL);
        fpi_ssm_mark_completed (ssm);
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
moc_list (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, moc_list_run_state, LIST_STATES);
  fpi_ssm_start (self->task_ssm, moc_action_done);
}

/* =====================================================================
 * enroll
 * ===================================================================== */

enum {
  ENROLL_DB_SIZE,       /* D5: 已用槽位数 */
  ENROLL_DB_LAST,       /* D9×N: 遍历库里全部记录, 取身份 GUID / 计数器 */
  ENROLL_CREATE,        /* 0xC2: 建录入会话 */
  ENROLL_CAPTURE,       /* 0xC4: 阻塞采集, 回 20 字节元数据 */
  ENROLL_DUMP,          /* 0x13 02 */
  ENROLL_IMAGE,         /* 2048 字节块交换 */
  ENROLL_PROCESS,       /* 0x31: 4 字节处理状态码 */
  ENROLL_APPLY,         /* 0xC5: A6 继续 / A0 质量差 / A7 完成 */
  ENROLL_DECIDE,        /* 按 0xC5 的 event 决定继续采集还是提交 */
  ENROLL_FLUSH,         /* C5 之后的 0x13+块收尾 (Windows 在 A6/A0/A7 后都做) */
  ENROLL_FINAL_D5,      /* A7 收尾后: D5 */
  ENROLL_FINAL_C1,      /* A7 之后: 0xC1 自验证, 命中回 ev=0xAF */
  ENROLL_FLUSH2,        /* C1 自验证之后的 0x13+块收尾 */
  ENROLL_PRE_COMMIT,    /* D5 + D9×3 (提交前 Windows 的固定序列) */
  ENROLL_COMMIT,        /* 0xC3: 提交 80 字节记录 */
  ENROLL_ID_SIZE,       /* D5: 芯片分配后的模板总数 */
  ENROLL_ID_REC,        /* D9: 读回最后一条记录, 拿芯片分配的真实模板号 */
  ENROLL_END,           /* 0x0C: 结束录入会话 (必须在读回之后: CANCEL 后通道挂数秒) */
  ENROLL_FINISH,
  ENROLL_STATES,
};

typedef struct
{
  FpPrint *print;
  guint    stage;
  guint    tries;
  guint32  used;
  guint32  slot;          /* 本次登记要写入的槽位 = 登记开始时的已用数 */
  guint32  counter;
  guint32  template_id;
  guint32  max_tid;       /* 遍历中见过的最大模板号 */
  guint32  base_tid;      /* 库内 index0 记录的模板号 (0 = 库空) */
  guint32  record_tid;    /* 本次 C3 提交用的模板号 (= 遍历到的 max_tid + 1) */
  guint8   guid[16];
  guint8   record[MOC5159_RECORD_SIZE];
  guint8   last_event;
  gboolean flush;          /* 本次 0x13+块是 C5/C1 之后的收尾读 (不走 0x31) */
  guint    flush_target;   /* flush 读完之后要跳到的状态 */
  guint    precommit_step; /* ENROLL_PRE_COMMIT 的 D5+D9×N 步进 */
  guint    db_step;        /* ENROLL_DB_LAST 的 D9 遍历步进 */
} EnrollCtx;

static void
moc_enroll_run_state (FpiSsm *ssm, FpDevice *device);

static void
moc_enroll_capture_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("采集上报 ev=0x%02X len=%zu", rsp->event, rsp->len);

  if (rsp->event != MOC_EV_OK && rsp->event != MOC_EV_NONE)
    {
      ctx->tries++;
      if (ctx->tries > MOC_MAX_CAPTURE_TRIES)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "采集失败: ev=0x%02X",
                                                         rsp->event));
          return;
        }

      fpi_device_enroll_progress (device, ctx->stage, NULL,
                                  fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                                            "请抬起手指后重新按压"));
      fpi_ssm_jump_to_state (ssm, ENROLL_CAPTURE);
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
moc_enroll_image_cb (FpDevice *device, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /*
   * 每一级是两次 0x13+块交换 (3412 抓包逐帧核实):
   *   C4 -> 0x13+块 -> 0x31 -> C5 -> 0x13+块(收尾) -> 下一次 C4
   * 收尾读完成后跳 ctx->flush_target (中级=ENROLL_CAPTURE, 最终=后续状态)。
   */
  if (ctx->flush)
    {
      ctx->flush = FALSE;
      fpi_ssm_jump_to_state (ssm, ctx->flush_target);
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
moc_enroll_dump_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  ctx->last_event = rsp->event;

  /* 没有待取图像时固件会回非 OK; 此时要重新按压采集, 而不是直接失败 */
  if (rsp->event != MOC_EV_OK)
    {
      ctx->tries++;
      if (ctx->tries > MOC_MAX_CAPTURE_TRIES)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "取图像失败: ev=0x%02X",
                                                         rsp->event));
          return;
        }
      fpi_ssm_jump_to_state (ssm, ENROLL_CAPTURE);
      return;
    }

  fpi_ssm_next_state (ssm);      /* -> ENROLL_IMAGE: 无论状态都要把图像读走 */
}

static void
moc_enroll_final_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /*
   * 登记完成后芯片会对新模板做一次自比对, 命中回 ev=0xAF (抓包 #2747)。
   * 这里只做记录: 即使不是 0xAF 也继续提交, 由后续 verify 判定。
   */
  fp_info ("登记自验证: ev=0x%02X len=%zu%s", rsp->event, rsp->len,
           rsp->event == MOC_EV_VERIFY_MATCH ? " (命中)" : "");

  /* C1 之后 Windows 还做一次 0x13+块收尾, 再走 D5 + D9×3 + C3 */
  ctx->flush_target = ENROLL_PRE_COMMIT;
  fpi_ssm_jump_to_state (ssm, ENROLL_FLUSH2);
}

static void
moc_enroll_process_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (rsp->len >= 4)
    fp_dbg ("处理状态码 0x%02X", rsp->payload[0]);

  fpi_ssm_next_state (ssm);
}

static void
moc_enroll_apply_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("APPLY_IMAGE ev=0x%02X len=%zu", rsp->event, rsp->len);
  ctx->last_event = rsp->event;

  /* C5 之后必须再做一次 0x13+块收尾 (ENROLL_DECIDE 统一路由到 ENROLL_FLUSH) */
  fpi_ssm_next_state (ssm);
}

static void
moc_enroll_last_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  /* pcap 实测 (hello_enroll_3412 D9 应答): record[0:4]=模板号, record[76:80]=计数器。
   * 遍历全库: 第 1 条 (index0) 的模板号 = 本库的"基础模板号" base_tid。
   *
   * 【多指解锁的关键】Windows 库 5 条记录的 index0~3 模板号**全是 3**,
   * 只有我们早先用 Linux 写的那条第 5 条才是 4。芯片做比对时从 index0 起
   * 把**同模板号的连续记录**当成一个模板组 (同一指纹的多个特征变体),
   * 遇到模板号跳变就停止 —— 所以 index0=tid3 / index1=tid2 时,
   * 只有 index0 那条(经 D8 累积成组)参与比对, 第二个手指永远不了。
   * 因此新手指必须**复用 index0 的模板号**, 作为同一组的追加记录。 */
  if (!error && rsp->event == MOC_EV_OK && rsp->len >= 4 + MOC5159_RECORD_SIZE)
    {
      const guint8 *rec = rsp->payload + 4;
      guint32 tid = moc_le32 (rec + MOC5159_RECORD_ID_OFF);
      guint32 cnt = moc_le32 (rec + MOC5159_RECORD_CNT_OFF);

      if (ctx->base_tid == 0)
        ctx->base_tid = tid;
      ctx->max_tid = MAX (ctx->max_tid, tid);
      ctx->counter = MAX (ctx->counter, cnt + 1);
    }

  /* Windows 在 C2 之前把库里记录逐条 D9 读一遍 (hello_enroll_3412: used=3 发 D9 0/1/2),
   * 不是只读最后一条。 */
  ctx->db_step++;
  fpi_ssm_jump_to_state (ssm, ENROLL_DB_LAST);
}

static void
moc_enroll_size_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (!error && rsp->event == MOC_EV_OK && rsp->len >= 12)
    ctx->used = moc_le32 (rsp->payload + 8);

  if (ctx->used == 0)
    fpi_ssm_jump_to_state (ssm, ENROLL_CREATE);
  else
    fpi_ssm_next_state (ssm);
}

static void
moc_enroll_create_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (rsp->event != MOC_EV_OK)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "CREATE_ENROLLMENT 失败: ev=0x%02X",
                                                     rsp->event));
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
moc_enroll_commit_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (rsp->event != MOC_EV_OK || rsp->len < 4)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "提交模板失败: ev=0x%02X",
                                                     rsp->event));
      return;
    }

  ctx->template_id = moc_le32 (rsp->payload);
  fp_info ("录入完成, 模板号 0x%08X", ctx->template_id);
  /* C3 之后 Windows 发 0x0C 结束录入会话 */
  fpi_ssm_next_state (ssm);
}

/* 读回芯片分配的真实模板号。
 * C3 应答不携带芯片分配的模板号 (实测回 0), 芯片是自动递增分配的
 * (实测 6 条库: 3,3,3,3,4,5), 所以提交后必须用 D5+D9 读回最后一条记录。
 * 注意必须在 0x0C (CANCEL) 之前做 —— CANCEL 之后通道挂住数秒。 */
static void
moc_enroll_id_size_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* D5: [8:12] = 已用模板数 ([0:4] 是空槽数, 用错会读到越界 index) */
  ctx->used = (rsp->len >= 12) ? moc_le32 (rsp->payload + 8) : 0;
  fpi_ssm_next_state (ssm);
}

static void
moc_enroll_id_rec_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* D9 应答: payload[0:4] 头 + 80 字节记录 = **84 字节** (不是 88)。
   * record[0:4] = 模板号, record[76:80] = 记录计数器 (Windows 全库全局递增, 唯一)。
   * 只信 ev=0x01 且长度足够的应答 —— 越界 index 会回 ev=0x26 + 全 0,
   * 不校验就会把模板号误记成 0 (三个指纹全是 id=0 的根因)。
   *
   * 【已修 bug】原来写 `8 + RECORD_SIZE`(=88), 实际应答只有 84 字节, 条件永远不成立,
   * 于是每次都走 "读回失败" 分支, template_id 保留上一次的陈旧值 (实测留下 0x5A),
   * fprintd 里的 print id 就成了垃圾值, identify 命中时报不出具体手指。
   * 用计数器当 print id: 全局单调递增, 天然唯一, 删指纹不会删错记录。 */
  if (!error && rsp->event == MOC_EV_OK && rsp->len >= 4 + MOC5159_RECORD_SIZE)
    {
      /* 必须用 **模板号 (record[0:4])**, 不能用计数器:
       * fprintd 会用 fpi-data 里的 id 去和 list_prints 报上来的 id 比对,
       * 对不上的存储指纹会被**直接删除**（日志 "unknown to device"）。
       * list_prints 那边用的是 record[0:4], 这里必须保持一致;
       * 用计数器的话, 一旦芯片做过 D8 自适应（cnt 前进而 tid 不变）就会错位。 */
      ctx->template_id = moc_le32 (rsp->payload + 4 + MOC5159_RECORD_ID_OFF);
      fp_info ("芯片分配模板号 0x%08X (记录计数器 %u)",
               ctx->template_id,
               moc_le32 (rsp->payload + 4 + MOC5159_RECORD_CNT_OFF));
    }
  else
    {
      fp_info ("读回模板号失败 (ev=0x%02X len=%zu), 保留上次值 0x%08X",
               error ? 0 : rsp->event, error ? 0 : rsp->len, ctx->template_id);
    }
  fpi_ssm_next_state (ssm);
}

/* ENROLL_PRE_COMMIT: 步进执行 D5 + D9×3 (index 0..2) */
static void
moc_enroll_precommit_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  EnrollCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* step 0 = D5, step 1..n = D9(index 0..n-1)。
   * 遍历数必须等于库里实际条数 (Windows 库 3 条就只发 D9 0/1/2),
   * 硬编码 3 会在库不满时发越界 index, 芯片回 0x26 污染会话。 */
  ctx->precommit_step++;
  {
    guint n = MIN (ctx->slot, 3);

    if (ctx->precommit_step <= n)
      fpi_ssm_jump_to_state (ssm, ENROLL_PRE_COMMIT);
    else
      fpi_ssm_next_state (ssm);
  }
}

static void
moc_enroll_run_state (FpiSsm *ssm, FpDevice *device)
{
  EnrollCtx *ctx = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ENROLL_DB_SIZE:
      moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_enroll_size_cb, ctx);
      break;

    case ENROLL_DB_LAST:
      /* D9 遍历库里全部记录 (Windows: D5 + D9×N), 读完进 ENROLL_CREATE */
      if (ctx->db_step >= ctx->used)
        {
          fpi_ssm_jump_to_state (ssm, ENROLL_CREATE);
          break;
        }
      {
        guint8 idx = (guint8) ctx->db_step;

        moc_transact (device, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &idx, 1,
                      MOC_TIMEOUT_CMD, moc_enroll_last_cb, ctx);
      }
      break;

    case ENROLL_CREATE:
      /* record 布局 = Windows D9 应答 payload[4:84] 逐字节:
       *   [0:4]=模板号  [4:8]=0x1C  [8:12]=0x0501
       *   [12:16]=00 00 00 05  [16:20]=15 00 00 00
       *   [20:36]=GUID  [36:76]=0  [76:80]=计数器
       * 陷阱: 曾把 [12:20] 写成 0 + 0x1505 (整体右移一字节)，已按 pcap 修正。 */
      ctx->slot = ctx->used;
      memset (ctx->record, 0, sizeof ctx->record);
      /* 模板号规则按 Windows 实测: 新模板号 = max_tid + 1。
       * 抓包 hello_enroll_3412 #2784 的 C3 payload[0:4]=3, 而此前库里的 3 条
       * 记录全是 tid=3 (同一指纹的第 1~3 个样本) —— 那次登记是给已有模板 3
       * **追加样本**, 不是新建模板。
       * 【曾经的错误】第十九轮为了让新手指"并进同一比对组"改成全库共享 tid,
       * 结果第二次 C3 带重复 tid 被芯片当成追加样本, 模板数据没变,
       * 新手指反而彻底无法匹配 —— 已回退。 */
      {
        guint32 new_tid = ctx->max_tid + 1;

        if (new_tid == 0)
          new_tid = 1;
        ctx->record_tid = new_tid;
        moc_put_le32 (ctx->record + 0, new_tid);
      }
      moc_put_le32 (ctx->record + 4, 0x1c);
      moc_put_le32 (ctx->record + 8, 0x501);
      moc_put_le32 (ctx->record + 12, 0x05000000);
      moc_put_le32 (ctx->record + 16, 0x15);
      {
        /* 身份 GUID 必须是**全库统一**的固定值 (Windows 库 5 条记录同值),
         * 每条记录随机生成会让芯片只认其中一条 —— 多指解锁失效的根因。 */
        static const guint8 tpl_guid[16] = MOC5159_TEMPLATE_GUID;

        memcpy (ctx->record + MOC5159_RECORD_GUID_OFF, tpl_guid, 16);
      }
      moc_put_le32 (ctx->record + MOC5159_RECORD_CNT_OFF,
                    ctx->counter ? ctx->counter : 1);
      fp_info ("构造新记录: 模板号=%u 计数器=%u (新模板号 = max_tid+1)",
               ctx->record_tid,
               ctx->counter ? ctx->counter : 1);

      moc_transact (device, MOC_SUB_CMD_CREATE_ENROLLMENT, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_enroll_create_cb, ctx);
      break;

    case ENROLL_CAPTURE:
      ctx->flush = FALSE;
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      moc_transact (device, MOC_SUB_CMD_ENROLL_CAPTURE_AND_REPORT, NULL, 0,
                    MOC_TIMEOUT_CAPTURE, moc_enroll_capture_cb, ctx);
      break;

    case ENROLL_DUMP:
      {
        guint8 sub = 0x02;
        moc_transact_bare (device, MOC_SUB_CMD_DUMP_DATA, &sub, 1,
                           MOC_TIMEOUT_CMD, moc_enroll_dump_cb, ctx);
      }
      break;

    case ENROLL_IMAGE:
      moc_image_exchange (device, moc_enroll_image_cb, ctx);
      break;

    case ENROLL_PROCESS:
      moc_transact (device, MOC_SUB_CMD_GET_PROCESS_STATUS, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_enroll_process_cb, ctx);
      break;

    case ENROLL_APPLY:
      moc_transact (device, MOC_SUB_CMD_ENROLL_APPLY_IMAGE, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_enroll_apply_cb, ctx);
      break;

    case ENROLL_DECIDE:
      switch (ctx->last_event)
        {
        /*
         * 3412 抓包逐帧核实: C4 -> 0x13+块 -> 0x31 -> C5 之后, Windows
         * **无论 A6(通过) / A0(质量不足) / A7(完成)** 都会再做一次
         * 0x13+2048块收尾, 然后才回 C4。跳过收尾会让芯片卡在
         * "采集未取走" 状态, 后续 C4 连续快速回 A0。
         * C5 的事件: 0x01/0xA6 = 本级通过, 0xA0 = 质量不足, 0xA7 = 全部完成。
         */
        case MOC_EV_OK:
        case MOC_EV_ALGO_ENROLL_RUN_NEED_MORE:
          ctx->stage = MIN (ctx->stage + 1, (guint) MOC_MAX_ENROLL_STAGES - 1);
          ctx->tries = 0;
          fp_info ("请继续按压 (%u/%u)", ctx->stage + 1, MOC_MAX_ENROLL_STAGES);
          fpi_device_enroll_progress (device, ctx->stage, NULL, NULL);
          ctx->flush_target = ENROLL_CAPTURE;
          fpi_ssm_jump_to_state (ssm, ENROLL_FLUSH);
          break;

        case MOC_EV_ALGO_NEED_LIFT:
          ctx->tries++;
          if (ctx->tries > MOC_MAX_CAPTURE_TRIES)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
              break;
            }
          fpi_device_enroll_progress (device, ctx->stage, NULL,
                                      fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                                                "请抬起手指, 再重新按下"));
          ctx->flush_target = ENROLL_CAPTURE;
          fpi_ssm_jump_to_state (ssm, ENROLL_FLUSH);
          break;

        case MOC_EV_ALGO_IMAGE_NOT_QUALIFIED:
          ctx->tries++;
          if (ctx->tries > MOC_MAX_CAPTURE_TRIES)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
              break;
            }
          fpi_device_enroll_progress (device, ctx->stage, NULL,
                                      fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                                                "图像质量不足, 请调整手指位置"));
          ctx->flush_target = ENROLL_CAPTURE;
          fpi_ssm_jump_to_state (ssm, ENROLL_FLUSH);
          break;

        case MOC_EV_ALGO_ENROLL_RUN_DONE:
          fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);
          ctx->flush_target = ENROLL_FINAL_D5;
          fpi_ssm_jump_to_state (ssm, ENROLL_FLUSH);
          break;

        default:
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "未知采集状态 0x%02X",
                                                         ctx->last_event));
          break;
        }
      break;

    case ENROLL_FLUSH:
      /* C5 之后的收尾读: 0x13 + 2048 块, 读完跳 flush_target */
      ctx->flush = TRUE;
      fpi_ssm_jump_to_state (ssm, ENROLL_DUMP);
      break;

    case ENROLL_FINAL_D5:
      moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_next_state_cb, ctx);
      break;

    case ENROLL_FINAL_C1:
      moc_transact (device, MOC_SUB_CMD_VERIFY_MATCHING_RESULT, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_enroll_final_cb, ctx);
      break;

    case ENROLL_FLUSH2:
      /* C1 自验证之后的收尾读, 读完跳 flush_target (= ENROLL_PRE_COMMIT) */
      ctx->flush = TRUE;
      fpi_ssm_jump_to_state (ssm, ENROLL_DUMP);
      break;

    case ENROLL_PRE_COMMIT:
      /* Windows 提交前固定序列: D5 -> D9(0) -> D9(1) -> D9(2) */
      if (ctx->precommit_step == 0)
        moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                      MOC_TIMEOUT_CMD, moc_enroll_precommit_cb, ctx);
      else
        {
          guint8 sub = (guint8) (ctx->precommit_step - 1);

          moc_transact (device, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &sub, 1,
                        MOC_TIMEOUT_CMD, moc_enroll_precommit_cb, ctx);
        }
      break;

    case ENROLL_COMMIT:
      /* C3 = 完整 80 字节 record (pcap #2784 帧 byte6 起逐字节核对:
       * 03 00 00 00 1c 00 00 00 01 05 ... 05 15 ... GUID ... f8)。
       * 旧记忆"C3 只发 1 字节 arg=3"是误读 —— 那是 record[0]=模板号 3。
       * 之前试 1 字节提交: 芯片写入的记录全是未初始化垃圾 (模板号 0xFFFFFFxx)。 */
      fp_info ("提交模板 (C3) 80 字节 record, 模板号=%u",
               moc_le32 (ctx->record));
      moc_transact (device, MOC_SUB_CMD_COMMIT_ENROLLMENT, ctx->record,
                    sizeof ctx->record, MOC_TIMEOUT_CMD,
                    moc_enroll_commit_cb, ctx);
      break;

    case ENROLL_END:
      moc_transact (device, MOC_SUB_CMD_CANCEL, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_next_state_cb, ctx);
      break;

    case ENROLL_ID_SIZE:
      moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_enroll_id_size_cb, ctx);
      break;

    case ENROLL_ID_REC:
      {
        guint8 idx = (guint8) (ctx->used > 0 ? ctx->used - 1 : 0);

        moc_transact (device, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &idx, 1,
                      MOC_TIMEOUT_CMD, moc_enroll_id_rec_cb, ctx);
      }
      break;

    case ENROLL_FINISH:
      moc_print_set_id (ctx->print, ctx->template_id);
      fpi_device_enroll_complete (device, g_object_ref (ctx->print), NULL);
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
moc_enroll (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  EnrollCtx *ctx = g_new0 (EnrollCtx, 1);
  static const guint8 default_guid[16] = MOC5159_WINBIO_DB_GUID;

  fpi_device_get_enroll_data (device, &ctx->print);
  ctx->counter = 1;
  memcpy (ctx->guid, default_guid, sizeof default_guid);

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, moc_enroll_run_state, ENROLL_STATES);
  fpi_ssm_set_data (self->task_ssm, ctx, g_free);
  fpi_ssm_start (self->task_ssm, moc_action_done);
}

/* =====================================================================
 * verify / identify
 * ===================================================================== */

enum {
  VERIFY_PREAMBLE,      /* D5: 读模板总数 */
  VERIFY_PREAMBLE_REC,  /* D9: 逐条下发候选集 (index 0..N-1) */
  VERIFY_POLL,          /* 0x30, 抓包 #66 在采集前恰好有一条 */
  VERIFY_CAPTURE,       /* 0xC0, 固件阻塞到有手指 */
  VERIFY_DUMP,          /* 0x13 02 */
  VERIFY_IMAGE,         /* 2048 字节块交换 */
  VERIFY_PROCESS,       /* 0x31 */
  VERIFY_DB_SIZE,       /* D5 */
  VERIFY_RESULT,        /* 0xC1 */
  VERIFY_D8,            /* 0xD8: 命中后模板适应写回 (照抄 Windows 命中流) */
  VERIFY_QUERY_RECORD,
  VERIFY_FINISH,
  VERIFY_STATES,
};

/* Windows 抓包: 会话开始时连做两轮「D5 + D9 遍历全部模板」, 之后每次验证前只发 D5。
 * 关键是 D9 必须覆盖 D5 返回的全部 N 条 —— 只加载前 3 条会让新登记的模板
 * 进不了比对候选集, 表现为「用刚登记的手指也恒不命中」。 */
#define MOC_VERIFY_PREAMBLE_ROUNDS 2

typedef struct
{
  FpPrint   *verify_print;
  GPtrArray *prints;
  gboolean   is_identify;
  gboolean   matched;
  gboolean   adapt;      /* 命中后走 D8 适应写回路径 */
  gboolean   has_id;
  guint32    matched_id;
  guint      tries;
  guint      db_count;    /* D5 返回的模板总数 */
  guint      rec_idx;     /* 当前 D9 遍历到的 index */
  guint      round;       /* 已完成的前导轮数 */
} VerifyCtx;

/* D5 应答: payload[0:4] = 模板总数 (u32 LE), 之后为 0 */
static void
moc_verify_preamble_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  VerifyCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* D5 语义 (2026-10-25 终审, hello_enroll_3412.pcapng 逐字节核对):
   *   payload[0:4] = 剩余空槽数 (10 - 已用)
   *   payload[8:12] = 已用模板数
   * Windows 在 used=3 的库上只对 index 0..2 下发 D9 —— 遍历数必须用
   * [8:12]。此前误用 [0:4] 导致越界 D9 (ev=0x26) 污染比对会话,
   * 表现为「只有最后一条有效候选 (列表第一个指纹) 能解锁」。 */
  if (rsp->len >= 12)
    ctx->db_count = moc_le32 (rsp->payload + 8);
  else
    ctx->db_count = 0;

  fp_dbg ("模板总数 = %u", ctx->db_count);

  /* 注意: 只重置 rec_idx, 不能动 ctx->round —— 否则永远走不出前导轮 */
  ctx->rec_idx = 0;

  if (ctx->db_count == 0)
    fpi_ssm_next_state (ssm);          /* 空库, 跳过候选集下发 */
  else
    fpi_ssm_jump_to_state (ssm, VERIFY_PREAMBLE_REC);
}

/* D9 逐条下发候选集 */
static void
moc_verify_preamble_rec_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  VerifyCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      /* D9 越界/无应答不致命, 直接进入采集 */
      fp_dbg ("D9 index=%u 无应答, 提前结束候选集下发", ctx->rec_idx);
      fpi_ssm_jump_to_state (ssm, VERIFY_POLL);
      return;
    }

  ctx->rec_idx++;

  if (ctx->rec_idx >= ctx->db_count)
    {
      ctx->round++;
      ctx->rec_idx = 0;

      if (ctx->round >= MOC_VERIFY_PREAMBLE_ROUNDS)
        fpi_ssm_jump_to_state (ssm, VERIFY_POLL);
      else
        fpi_ssm_jump_to_state (ssm, VERIFY_PREAMBLE);
    }
  else
    {
      fpi_ssm_jump_to_state (ssm, VERIFY_PREAMBLE_REC);
    }
}

/* 调试用: 把应答 payload 打成十六进制字符串 (静态缓冲, 仅用于日志) */
static const gchar *
moc_hex_payload (const MocRsp *rsp, guint max)
{
  static gchar buf[3 * 40 + 4];
  guint n = MIN (rsp->len, max);
  guint off = 0;

  buf[0] = '\0';
  for (guint i = 0; i < n && off + 3 < sizeof buf; i++)
    off += g_snprintf (buf + off, sizeof buf - off, "%02X ", rsp->payload[i]);

  return buf;
}

static void
moc_verify_poll_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fp_dbg ("采集状态字 0x30 无应答, 直接进入采集");
    }
  else
    {
      fp_dbg ("采集状态字 0x30: ev=0x%02X len=%zu [%s]",
              rsp->event, rsp->len, moc_hex_payload (rsp, 8));
    }

  fpi_ssm_next_state (ssm);
}

static void
moc_verify_capture_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  VerifyCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("验证采集 ev=0x%02X len=%zu %s", rsp->event, rsp->len,
          moc_hex_payload (rsp, 24));

  if (rsp->event != MOC_EV_OK && rsp->event != MOC_EV_NONE)
    {
      ctx->tries++;
      if (ctx->tries > MOC_MAX_CAPTURE_TRIES)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "验证采集失败: ev=0x%02X",
                                                         rsp->event));
          return;
        }
      fpi_ssm_jump_to_state (ssm, VERIFY_CAPTURE);
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
moc_verify_image_cb (FpDevice *device, GError *error, gpointer user)
{
  VerifyCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* 命中后的第二次块交换 (适应路径) → D8; 正常路径 → 0x31 */
  if (ctx->adapt)
    fpi_ssm_jump_to_state (ssm, VERIFY_D8);
  else
    fpi_ssm_next_state (ssm);
}

static void
moc_verify_d8_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  VerifyCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (!error)
    fp_info ("模板适应写回 (D8) ev=0x%02X", rsp->event);
  else
    fp_info ("模板适应写回 (D8) 失败: %s", error->message);

  /* D8 失败不影响判定结果, 继续走完 */
  if (ctx->is_identify)
    fpi_ssm_jump_to_state (ssm, VERIFY_QUERY_RECORD);
  else
    fpi_ssm_jump_to_state (ssm, VERIFY_FINISH);
}

static void
moc_verify_dump_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("应答 cmd=0x%02X ev=0x%02X len=%zu [%s]",
          rsp->cmd, rsp->event, rsp->len, moc_hex_payload (rsp, 8));

  fpi_ssm_next_state (ssm);
}

/*
 * 命中判定 (第十轮确立, 10/10 带标签样本可分)
 *
 * 注意: ev 与成败无关! 10 个 Windows 样本(成功5/失败5)的 ev 全是 0x01,
 *       ev=0xAF 只在「登记完成 C5=0xA7」之后出现, 正常识别永不出现。
 *
 * 真正判据在 0xC1 的 120 字节里: [8:24] 与 [24:40] 是两个 16 字节块,
 * 二者的关系与成败一一对应:
 *
 *   命中  : b1[8] == b2[8]  且  b1[9:16] 与 b2[9:16] 有差异 (L1 = 18~28)
 *   未命中: b1[8] != b2[8]  且  b1[9:16] 与 b2[9:16] 完全相同 (L1 = 0)
 *
 * 样本验证: T1/T3/T4/A4/F2 成功 -> 全部 b1[8]==b2[8] 且 L1>0
 *           T2/A1/A2/A3/F1 失败 -> 全部 b1[8]!=b2[8] 且 L1==0
 */
static gboolean
moc_c1_matched (const MocRsp *rsp)
{
  const guint8 *b1, *b2;
  guint diff = 0;

  if (rsp->len < 40)
    return FALSE;

  b1 = rsp->payload + 8;
  b2 = rsp->payload + 24;

  if (b1[8] != b2[8])
    return FALSE;

  for (guint i = 9; i < 16; i++)
    diff += (guint) ABS ((gint) b1[i] - (gint) b2[i]);

  return diff > 0;
}

static void
moc_verify_result_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  VerifyCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;
  const guint8 *b1, *b2;
  guint diff = 0;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  b1 = rsp->len >= 24 ? rsp->payload + 8 : rsp->payload;
  b2 = rsp->len >= 40 ? rsp->payload + 24 : rsp->payload;
  for (guint i = 9; i < 16 && rsp->len >= 40; i++)
    diff += (guint) ABS ((gint) b1[i] - (gint) b2[i]);

  ctx->matched = moc_c1_matched (rsp);
  fp_info ("比对结果: %s (ev=0x%02X len=%zu) b1[8]=0x%02X b2[8]=0x%02X L1=%u [%s]",
           ctx->matched ? "命中" : "未命中", rsp->event, rsp->len,
           rsp->len >= 24 ? b1[8] : 0, rsp->len >= 40 ? b2[8] : 0, diff,
           moc_hex_payload (rsp, 40));

  /* 命中后照抄 Windows 命中流: 13+块交换 → D8 (DB_UPDATE_TPL)。
   * Windows 每次成功匹配都把本次采集写回芯片, 同一手指逐渐积累
   * 多条记录 (抓包里 template#3 有 4 条), 这是「越用越准」的机制。
   * 未命中直接结束。 */
  if (ctx->matched)
    {
      ctx->adapt = TRUE;
      fpi_ssm_jump_to_state (ssm, VERIFY_DUMP);
    }
  else
    fpi_ssm_jump_to_state (ssm, VERIFY_FINISH);
}

static void
moc_verify_query_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  VerifyCtx *ctx = user;
  FpiSsm *ssm = FPI_DEVICE_MOC5159 (device)->task_ssm;

  /*
   * 命中后尝试用 DB_GET_CURRENT_CONTENT (0xD4) 定位「当前记录」。
   * 这是目前唯一可能取回具体模板号的途径, 尚未在实机验证成功
   * (采集链路还没打通), 失败就退回「只报告命中」。
   */
  if (!error && rsp->event == MOC_EV_OK && rsp->len >= 4 + MOC5159_RECORD_SIZE)
    {
      ctx->matched_id = moc_le32 (rsp->payload + 4 + MOC5159_RECORD_ID_OFF);
      ctx->has_id = TRUE;
      fp_dbg ("命中模板号 0x%08X", ctx->matched_id);
    }

  fpi_ssm_next_state (ssm);
}

static void
moc_verify_run_state (FpiSsm *ssm, FpDevice *device)
{
  VerifyCtx *ctx = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case VERIFY_PREAMBLE:
      moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_verify_preamble_cb, ctx);
      break;

    case VERIFY_PREAMBLE_REC:
      {
        guint8 sub = (guint8) ctx->rec_idx;

        fp_dbg ("下发候选模板 #%u/%u (第 %u 轮)",
                ctx->rec_idx, ctx->db_count, ctx->round + 1);
        moc_transact (device, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &sub, 1,
                      MOC_TIMEOUT_CMD, moc_verify_preamble_rec_cb, ctx);
      }
      break;

    case VERIFY_POLL:
      moc_transact (device, MOC_SUB_CMD_GET_CAPTURE_STATUS, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_verify_poll_cb, ctx);
      break;

    case VERIFY_CAPTURE:
      fp_dbg ("等待手指并采集 (0xC0)");
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NEEDED);
      moc_transact (device, MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT, NULL, 0,
                    MOC_TIMEOUT_CAPTURE, moc_verify_capture_cb, ctx);
      break;

    case VERIFY_DUMP:
      {
        guint8 sub = 0x02;
        moc_transact_bare (device, MOC_SUB_CMD_DUMP_DATA, &sub, 1,
                           MOC_TIMEOUT_CMD, moc_verify_dump_cb, ctx);
      }
      break;

    case VERIFY_IMAGE:
      moc_image_exchange (device, moc_verify_image_cb, ctx);
      break;

    case VERIFY_PROCESS:
      /* 抓包里图像交换后是 0x31, 回 4 字节状态码 (0x47/0x42) */
      moc_transact (device, MOC_SUB_CMD_GET_PROCESS_STATUS, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_verify_dump_cb, ctx);
      break;

    case VERIFY_DB_SIZE:
      moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_verify_dump_cb, ctx);
      break;

    case VERIFY_RESULT:
      fpi_device_report_finger_status (device, FP_FINGER_STATUS_NONE);
      moc_transact (device, MOC_SUB_CMD_VERIFY_MATCHING_RESULT, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_verify_result_cb, ctx);
      break;

    case VERIFY_D8:
      moc_transact (device, MOC_SUB_CMD_DB_UPDATE_TPL, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_verify_d8_cb, ctx);
      break;

    case VERIFY_QUERY_RECORD:
      moc_transact (device, MOC_SUB_CMD_DB_GET_CURRENT_CONTENT, NULL, 0,
                    MOC_TIMEOUT_CMD, moc_verify_query_cb, ctx);
      break;

    case VERIFY_FINISH:
      if (ctx->is_identify)
        {
          FpPrint *match = NULL;

          if (ctx->matched)
            {
              if (ctx->has_id)
                {
                  for (guint i = 0; i < ctx->prints->len; i++)
                    {
                      FpPrint *p = g_ptr_array_index (ctx->prints, i);
                      guint32 id = 0;

                      if (moc_print_get_id (p, &id) && id == ctx->matched_id)
                        {
                          match = p;
                          break;
                        }
                    }
                }

              /* MOC 芯片不会告知命中的是哪条模板 (0xD4 在实机上
               * 取不到当前记录), 而候选集本来就只包含本用户的
               * 登记记录 —— 命中即代表 gallery 中存在匹配。
               * 拿不到具体 id 时回退报告第一条登记记录。 */
              if (match == NULL && ctx->prints != NULL && ctx->prints->len > 0)
                {
                  match = g_ptr_array_index (ctx->prints, 0);
                  fp_info ("identify 命中但无法定位具体模板, 回退报告 finger=%d",
                           fp_print_get_finger (match));
                }
            }

          fpi_device_identify_report (device, match, match, NULL);
          fpi_device_identify_complete (device, NULL);
        }
      else
        {
          fpi_device_verify_report (device,
                                    ctx->matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                    ctx->verify_print, NULL);
          fpi_device_verify_complete (device, NULL);
        }

      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
moc_verify (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  VerifyCtx *ctx = g_new0 (VerifyCtx, 1);

  fpi_device_get_verify_data (device, &ctx->verify_print);
  ctx->is_identify = FALSE;

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, moc_verify_run_state, VERIFY_STATES);
  fpi_ssm_set_data (self->task_ssm, ctx, g_free);
  fpi_ssm_start (self->task_ssm, moc_action_done);
}

static void
moc_identify (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  VerifyCtx *ctx = g_new0 (VerifyCtx, 1);

  fpi_device_get_identify_data (device, &ctx->prints);
  ctx->is_identify = TRUE;

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, moc_verify_run_state, VERIFY_STATES);
  fpi_ssm_set_data (self->task_ssm, ctx, g_free);
  fpi_ssm_start (self->task_ssm, moc_action_done);
}

/* =====================================================================
 * delete / clear_storage
 * ===================================================================== */

enum {
  DELETE_SIZE,          /* D5: 查模板总数 */
  DELETE_FIND,          /* D9: 逐条读记录, 按模板号定位 */
  DELETE_SEND,          /* 0xDF: 回显 80 字节记录 (清库时为 0xD7) */
  DELETE_FINISH,
  DELETE_STATES,
};

typedef struct
{
  gboolean is_clear;
  guint32  template_id;
  guint32  used;
  guint    index;
  guint8   record[MOC5159_RECORD_SIZE];
} DeleteCtx;

/*
 * 2026-10-25 delete_02/delete_03 抓包实锤:
 *   D9 应答 payload = 4B 保留头(00000000) + 80B 记录;
 *   0xDF payload    = 该 80B 记录原样回显 (此前发 4B 模板号被 0x26 拒绝)。
 * 记录 [0:4] = 模板号。
 */
static void
moc_delete_find_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  DeleteCtx *ctx = fpi_ssm_get_data (self->task_ssm);
  FpiSsm *ssm = self->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (rsp->event == MOC_EV_OK && rsp->len >= 4 + MOC5159_RECORD_SIZE)
    {
      memcpy (ctx->record, rsp->payload + 4, MOC5159_RECORD_SIZE);
      /* 模板号是 fprintd print id 的载体 (见登记侧的读回逻辑)。
       * 兼容老 print: 早期版本把记录计数器当作 id 存过, 所以两个字段都比一遍。 */
      if (moc_le32 (ctx->record + MOC5159_RECORD_ID_OFF) == ctx->template_id
          || moc_le32 (ctx->record + MOC5159_RECORD_CNT_OFF) == ctx->template_id)
        {
          fp_info ("删除: idx=%u 命中模板 %u (记录计数器 %u), 发送 0xDF 回显记录",
                   ctx->index, moc_le32 (ctx->record),
                   moc_le32 (ctx->record + MOC5159_RECORD_CNT_OFF));
          fpi_ssm_next_state (ssm);
          return;
        }
    }

  ctx->index++;
  if (ctx->index >= ctx->used)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                     "芯片内找不到模板 %u",
                                                     ctx->template_id));
      return;
    }
  fpi_ssm_jump_to_state (ssm, DELETE_FIND);
}

static void
moc_delete_size_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  DeleteCtx *ctx = fpi_ssm_get_data (self->task_ssm);
  FpiSsm *ssm = self->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (rsp->event != MOC_EV_OK || rsp->len < 12)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "DB_GET_SIZE 失败 (ev=0x%02X)",
                                                     rsp->event));
      return;
    }

  ctx->used = moc_le32 (rsp->payload + 8);
  fp_info ("删除: 芯片现有 %u 条模板", ctx->used);
  if (ctx->used == 0)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                     "芯片库为空, 无可删模板"));
      return;
    }

  ctx->index = 0;
  fpi_ssm_next_state (ssm);
}

static void
moc_delete_send_cb (FpDevice *device, MocRsp *rsp, GError *error, gpointer user)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  FpiSsm *ssm = self->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (rsp->event != MOC_EV_OK)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "DB_DEL_RECORD 失败 (ev=0x%02X)",
                                                     rsp->event));
      return;
    }

  fp_info ("0xDF 删除成功");
  fpi_ssm_next_state (ssm);
}

static void
moc_delete_run_state (FpiSsm *ssm, FpDevice *device)
{
  DeleteCtx *ctx = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DELETE_SIZE:
      if (ctx->is_clear)
        fpi_ssm_jump_to_state (ssm, DELETE_SEND);
      else
        moc_transact (device, MOC_SUB_CMD_DB_GET_SIZE, NULL, 0,
                      MOC_TIMEOUT_CMD, moc_delete_size_cb, ctx);
      break;

    case DELETE_FIND:
      {
        guint8 idx = (guint8) ctx->index;

        moc_transact (device, MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX, &idx, 1,
                      MOC_TIMEOUT_CMD, moc_delete_find_cb, ctx);
      }
      break;

    case DELETE_SEND:
      if (ctx->is_clear)
        {
          moc_transact (device, MOC_SUB_CMD_DB_REMOVE_ALL_DB, NULL, 0,
                        MOC_TIMEOUT_CMD, moc_next_state_cb, ctx);
        }
      else
        {
          moc_transact (device, MOC_SUB_CMD_DB_DEL_RECORD, ctx->record,
                        sizeof ctx->record,
                        MOC_TIMEOUT_CMD, moc_delete_send_cb, ctx);
        }
      break;

    case DELETE_FINISH:
      if (ctx->is_clear)
        fpi_device_clear_storage_complete (device, NULL);
      else
        fpi_device_delete_complete (device, NULL);
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
moc_delete (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  DeleteCtx *ctx = g_new0 (DeleteCtx, 1);
  FpPrint *print = NULL;

  fpi_device_get_delete_data (device, &print);
  if (!moc_print_get_id (print, &ctx->template_id))
    {
      g_free (ctx);
      fpi_device_delete_complete (device,
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, moc_delete_run_state, DELETE_STATES);
  fpi_ssm_set_data (self->task_ssm, ctx, g_free);
  fpi_ssm_start (self->task_ssm, moc_action_done);
}

static void
moc_clear_storage (FpDevice *device)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);
  DeleteCtx *ctx = g_new0 (DeleteCtx, 1);

  ctx->is_clear = TRUE;

  g_assert (self->task_ssm == NULL);
  self->task_ssm = fpi_ssm_new (device, moc_delete_run_state, DELETE_STATES);
  fpi_ssm_set_data (self->task_ssm, ctx, g_free);
  fpi_ssm_start (self->task_ssm, moc_action_done);
}

/* =====================================================================
 * 统一收尾
 * ===================================================================== */

static void
moc_action_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceMoc5159 *self = FPI_DEVICE_MOC5159 (device);

  g_assert (self->task_ssm == ssm);
  self->task_ssm = NULL;

  if (error)
    fpi_device_action_error (device, error);
}

/* =====================================================================
 * GObject
 * ===================================================================== */

static void
fpi_device_moc5159_init (FpiDeviceMoc5159 *self)
{
}

static void
fpi_device_moc5159_class_init (FpiDeviceMoc5159Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Petaic MOC-5159 (GD32W515) Touch Fingerprint Sensor";

  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = moc5159_id_table;
  dev_class->nr_enroll_stages = MOC_MAX_ENROLL_STAGES;
  dev_class->temp_hot_seconds = 0;

  dev_class->probe = moc_probe;
  dev_class->open = moc_open;
  dev_class->close = moc_close;
  dev_class->cancel = moc_cancel;
  dev_class->suspend = moc_suspend;
  dev_class->resume = moc_resume;

  dev_class->enroll = moc_enroll;
  dev_class->verify = moc_verify;
  dev_class->identify = moc_identify;
  dev_class->list = moc_list;
  dev_class->delete = moc_delete;
  dev_class->clear_storage = moc_clear_storage;

  fpi_device_class_auto_initialize_features (dev_class);
}
