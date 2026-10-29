/*
 * moc5159.h — Petaic MOC-5159 (GD32W515 + GSL6150) 常量定义
 *
 * 设备: 28e9:5159 "GDMicroelectronics Touch Fingerprint Sensor"
 *       模组 MOC-5159, Windows Hello 认证模组, match-on-chip (模板存芯片内、比对也在芯片内)
 *
 * 协议来源: 对 Windows 驱动 PetaicTouchFP.dll 4.0.105.5 的静态逆向 +
 *           USBPcap 实测, 详见 ../docs/zh-CN/04-protocol-notes.md
 *
 * 本文件不依赖 libfprint, 独立工具 (tools/moc5159-probe.c) 与驱动共用。
 */
#ifndef MOC5159_H
#define MOC5159_H

#include <stddef.h>
#include <stdint.h>

/* ---------------- USB 身份 / 端点 ---------------- */
#define MOC5159_VID             0x28E9
#define MOC5159_PID             0x5159
#define MOC5159_INTERFACE       0
#define MOC5159_EP_OUT          0x01   /* bulk OUT, wMaxPacketSize = 64 */
#define MOC5159_EP_IN           0x81   /* bulk IN,  wMaxPacketSize = 64 */

/* ---------------- 定长帧 ---------------- */
#define MOC5159_FRAME_SIZE      128    /* 命令/响应帧固定 128 字节 */
#define MOC5159_PAYLOAD_OFF     6
#define MOC5159_PAYLOAD_MAX     120
#define MOC5159_CHECKSUM_OFF    126
#define MOC5159_IMG_BLOCK_SIZE  2048   /* DUMP_DATA 图像块 */

/* ---------------- SETUP 握手 (厂商控制请求) ---------------- */
#define MOC5159_SETUP_REQUEST   0xC0   /* bmRequestType=0xC0, bRequest=0xC0 */
#define MOC5159_SETUP_READY     0x5C

/* FPD 小命令通道 (控制请求, bRequest = 命令号) */
#define MOC5159_FPD_SET_CONTACT_MODE   0x15
#define MOC5159_FPD_CHECK_DEV_READY    0x1A
#define MOC5159_FPD_GET_FW_VERSION     0x1B
#define MOC5159_FPD_COMPLETE_CAPTURE   0x1D
#define MOC5159_FPD_SET_IMAGE_NUMBER   0x24

/* ---------------- MOC_SUB_CMD (frame[2]) ---------------- */
#define MOC_SUB_CMD_CANCEL                      0x0C
#define MOC_SUB_CMD_MOC_PROCESS_END             0x0F
#define MOC_SUB_CMD_GET_FW_VERSION              0x10
#define MOC_SUB_CMD_GET_SENSOR_INFO             0x11
#define MOC_SUB_CMD_DUMP_DATA                   0x13
/* 0x30: 采集会话状态字 (4 字节小端)。抓包里 00000000 = 无会话/空闲,
   00000001 = 录入会话等待手指; 在本机型上该字不可靠, 驱动不依赖它判断手指。 */
#define MOC_SUB_CMD_GET_CAPTURE_STATUS          0x30
/* 0x31: 图像处理完成后的 4 字节状态码 (抓包里 0x47 / 0x42)。 */
#define MOC_SUB_CMD_GET_PROCESS_STATUS          0x31
#define MOC_SUB_CMD_VERIFY_CAPTURE_AND_REPORT   0xC0
#define MOC_SUB_CMD_VERIFY_MATCHING_RESULT      0xC1
#define MOC_SUB_CMD_CREATE_ENROLLMENT           0xC2
#define MOC_SUB_CMD_COMMIT_ENROLLMENT           0xC3
#define MOC_SUB_CMD_ENROLL_CAPTURE_AND_REPORT   0xC4
#define MOC_SUB_CMD_ENROLL_APPLY_IMAGE          0xC5
#define MOC_SUB_CMD_DB_GET_SIZE                 0xD5
#define MOC_SUB_CMD_DB_GET_CURRENT_CONTENT      0xD4
#define MOC_SUB_CMD_DB_REMOVE_ALL_DB            0xD7
#define MOC_SUB_CMD_DB_DUMP_RECORD_BY_INDEX     0xD9
#define MOC_SUB_CMD_DB_OPEN_DB                  0xDB
/* 0xDA: DB_OPEN 回 0x29(DB_HEADER_EMPTY) 时 Windows 紧接着发的建库命令,
   payload = DB GUID(16) + u32 8 + 库内容 GUID(16) = 36 字节。
   不建库时芯片处于"无库头"降级状态: 多条模板只有一条能被比对命中。 */
#define MOC_SUB_CMD_DB_CREATE_DB                0xDA
#define MOC_SUB_CMD_DB_UPDATE_TPL               0xD8
#define MOC_SUB_CMD_DB_DEL_RECORD               0xDF

/* ---------------- 响应帧 frame[4]: 事件 / 状态码 ---------------- */
#define MOC_EV_NONE                 0x00
#define MOC_EV_OK                   0x01
#define MOC_EV_CANCEL               0x0D
#define MOC_EV_UNEXPECTED_PARAMETER 0x0E
#define MOC_EV_DB_OPEN_FAIL         0x22
#define MOC_EV_DB_HEADER_DUPLICATED 0x27
#define MOC_EV_DB_HEADER_EMPTY      0x29   /* 实测: 0xD7 清库后 DB_OPEN 回这个, 库为空但可用 */
#define MOC_EV_PKG_SUB_CMD_UNKNOWN  0x90
#define MOC_EV_PKG_DATA_LEN_ERROR   0x91
#define MOC_EV_PKG_CHECKSUM_FAIL    0x9C
#define MOC_EV_ALGO_IMAGE_NOT_QUALIFIED 0xA0
#define MOC_EV_ALGO_NEED_LIFT           0xA2   /* 实测: 重复取图后固件回这个 */
#define MOC_EV_ALGO_ENROLL_RUN_EXCEEDED 0xA5
#define MOC_EV_ALGO_ENROLL_RUN_NEED_MORE 0xA6
#define MOC_EV_ALGO_ENROLL_RUN_DONE     0xA7
#define MOC_EV_ALGO_ENROLL_RUN_FAIL     0xA8
#define MOC_EV_ALGO_ENROLL_FINALIZE_FAIL 0xA9
#define MOC_EV_VERIFY_MATCH             0xAF
#define MOC_EV_DEVICE_USB_TIMEOUT  0xD0
#define MOC_EV_DEVICE_USB_CRASH    0xDC

/* ---------------- 模板记录 (DB_DUMP_RECORD_BY_INDEX, 80 字节) -------------
 * [0:4]   槽位/模板号        (删除时作 key)
 * [4:8]   记录长度 (0x1C)
 * [8:12]  格式号   (0x501)
 * [16:20] 0x15
 * [20:36] 身份 GUID
 * [76:80] 模板计数器
 */
#define MOC5159_RECORD_SIZE     80
#define MOC5159_RECORD_ID_OFF   0
#define MOC5159_RECORD_GUID_OFF 20
#define MOC5159_RECORD_CNT_OFF  76

/* Windows Hello 生物识别数据库 GUID (小端字节序)
 * 52ABE9CD-ADC7-4052-B028-504FFE22188B */
#define MOC5159_WINBIO_DB_GUID \
    { 0xcd, 0xe9, 0xab, 0x52, 0xc7, 0xad, 0x52, 0x40, \
      0xb0, 0x28, 0x50, 0x4f, 0xfe, 0x22, 0x18, 0x8b }

/* 建库(0xDA) payload 里第二个 GUID = 库内容 GUID (抓包 delete_01 #158 原样) */
#define MOC5159_DB_CONTENT_GUID \
    { 0xb9, 0x7c, 0x69, 0xa2, 0x00, 0xb2, 0xf5, 0x4b, \
      0x9f, 0x40, 0x3a, 0x8f, 0x69, 0x9b, 0x4b, 0xd6 }

/* 模板记录 [20:36] 的身份 GUID。
 * **所有记录必须用同一个值** —— Windows 库里 5 条记录
 * (hello_enroll_3412 / verify_truth.pcapng) 该字段逐字节相同:
 * a1e02ef3-46a8-1266-f2d5-bc0fe9030000。
 * 之前驱动每条记录随机生成 GUID, 结果多条模板只有一条能命中。 */
#define MOC5159_TEMPLATE_GUID \
    { 0xa1, 0xe0, 0x2e, 0xf3, 0x46, 0xa8, 0x12, 0x66, \
      0xf2, 0xd5, 0xbc, 0x0f, 0xe9, 0x03, 0x00, 0x00 }

/* 帧解析结果 */
typedef struct
{
  uint8_t        cmd;
  uint8_t        byte3;     /* Windows 抓包里恒为 0x05, 语义未定 */
  uint8_t        event;
  uint8_t        byte5;     /* Windows 抓包里恒为 0x01 */
  const uint8_t *payload;   /* 指向原始 128 字节帧内部, 不拷贝 */
  size_t         payload_len;
} Moc5159Frame;

#endif /* MOC5159_H */
