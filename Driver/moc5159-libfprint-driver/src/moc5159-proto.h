/*
 * moc5159-proto.h — MOC-5159 定长帧编解码
 *
 *   [0:2]  len    u16 LE  payload 长度 (<= 120)
 *   [2]    cmd    u8      MOC_SUB_CMD; 设备响应回显同一命令号
 *   [3]    ?      u8      发送 0; 接收侧疑似返回码
 *   [4]    event  u8      事件/状态码 (接收侧)
 *   [5]    ?      u8      发送 0
 *   [6:6+len]     payload
 *   [126:128]     cks     u16 LE = sum(frame[0..125]) & 0xFFFF
 *
 * 独立于 libfprint, 可单独编译进诊断工具。
 */
#ifndef MOC5159_PROTO_H
#define MOC5159_PROTO_H

#include "moc5159.h"

/* 组帧: out 必须是 128 字节; paylen > 120 会被截断。返回 0。 */
int moc5159_frame_encode (uint8_t        cmd,
                          const uint8_t *payload,
                          size_t         paylen,
                          uint8_t        out[MOC5159_FRAME_SIZE]);

/* 解析: 校验和错误返回 -1, 长度不足返回 -2, 成功返回 0。
 * frame->payload 指向 in 内部, 生命周期随 in。 */
int moc5159_frame_parse (const uint8_t *in,
                         size_t         inlen,
                         Moc5159Frame  *frame);

/* 前 126 字节累加和 (16 位截断) */
uint16_t moc5159_checksum (const uint8_t frame[MOC5159_CHECKSUM_OFF]);

#endif /* MOC5159_PROTO_H */
