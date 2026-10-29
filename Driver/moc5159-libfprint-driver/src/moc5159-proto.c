/*
 * moc5159-proto.c — MOC-5159 定长帧编解码实现
 */
#include <string.h>

#include "moc5159-proto.h"

uint16_t
moc5159_checksum (const uint8_t frame[MOC5159_CHECKSUM_OFF])
{
  uint32_t sum = 0;
  int i;

  for (i = 0; i < MOC5159_CHECKSUM_OFF; i++)
    sum += frame[i];

  return (uint16_t) (sum & 0xFFFF);
}

int
moc5159_frame_encode (uint8_t        cmd,
                      const uint8_t *payload,
                      size_t         paylen,
                      uint8_t        out[MOC5159_FRAME_SIZE])
{
  uint16_t cks;

  if (paylen > MOC5159_PAYLOAD_MAX)
    paylen = MOC5159_PAYLOAD_MAX;

  memset (out, 0, MOC5159_FRAME_SIZE);
  out[0] = (uint8_t) (paylen & 0xFF);
  out[1] = (uint8_t) (paylen >> 8);
  out[2] = cmd;
  /* [3][4][5] 保持 0 (发送侧) */

  if (paylen > 0 && payload != NULL)
    memcpy (out + MOC5159_PAYLOAD_OFF, payload, paylen);

  cks = moc5159_checksum (out);
  out[MOC5159_CHECKSUM_OFF]     = (uint8_t) (cks & 0xFF);
  out[MOC5159_CHECKSUM_OFF + 1] = (uint8_t) (cks >> 8);

  return 0;
}

int
moc5159_frame_parse (const uint8_t *in,
                     size_t         inlen,
                     Moc5159Frame  *frame)
{
  uint16_t stored;
  size_t   len;

  if (inlen < MOC5159_FRAME_SIZE)
    return -2;

  stored = (uint16_t) (in[MOC5159_CHECKSUM_OFF] |
                       (in[MOC5159_CHECKSUM_OFF + 1] << 8));
  if (moc5159_checksum (in) != stored)
    return -1;

  len = (size_t) in[0] | ((size_t) in[1] << 8);
  if (len > MOC5159_PAYLOAD_MAX)
    len = MOC5159_PAYLOAD_MAX;

  frame->cmd         = in[2];
  frame->byte3       = in[3];
  frame->event       = in[4];
  frame->byte5       = in[5];
  frame->payload     = in + MOC5159_PAYLOAD_OFF;
  frame->payload_len = len;

  return 0;
}
