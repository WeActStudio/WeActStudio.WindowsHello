/*
 * mocsim-selftest.c — protocol-level self-test of the simulator
 *
 * Talks to whatever libusb is present, which under LD_PRELOAD=libmocsim.so is
 * the simulated module. It replays the driver's own sequence
 * (SETUP → D5 → D9 → C0 → 0x13 + block → 0x31 → C1) and checks each answer,
 * so the simulator can be validated without libfprint, fprintd or hardware.
 *
 *   ./build.sh && LD_PRELOAD=./libmocsim.so ./mocsim-selftest
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libusb-1.0/libusb.h>

#define EP_OUT 0x01
#define EP_IN  0x81

#define CMD_GET_FW_VERSION    0x10
#define CMD_DUMP_DATA         0x13
#define CMD_CAPTURE_STATUS    0x30
#define CMD_PROCESS_STATUS    0x31
#define CMD_VERIFY_CAPTURE    0xC0
#define CMD_VERIFY_MATCHING   0xC1
#define CMD_DB_GET_SIZE       0xD5
#define CMD_DB_WIPE           0xD7
#define CMD_DB_DUMP_RECORD    0xD9
#define CMD_DB_OPEN           0xDB

static int g_fail;
static int g_pass;
static int g_frame;

static void
ok (const char *what)
{
  g_pass++;
  printf ("  ok   %s\n", what);
}

static void
fail (const char *what, const char *why)
{
  g_fail++;
  printf ("  FAIL %s — %s\n", what, why);
}

static uint16_t
checksum (const uint8_t *f, int upto)
{
  uint32_t sum = 0;

  for (int i = 0; i < upto; i++)
    sum += f[i];

  return (uint16_t) (sum & 0xFFFF);
}

static void
encode (uint8_t cmd, const uint8_t *payload, int paylen, uint8_t out[128])
{
  uint16_t cks;

  memset (out, 0, 128);
  out[0] = (uint8_t) (paylen & 0xFF);
  out[1] = (uint8_t) (paylen >> 8);
  out[2] = cmd;

  if (paylen > 0 && payload != NULL)
    memcpy (out + 6, payload, (size_t) paylen);

  cks = checksum (out, 126);
  out[126] = (uint8_t) (cks & 0xFF);
  out[127] = (uint8_t) (cks >> 8);
}

/* Drain leftovers exactly like the driver: read until the IN pipe is empty. */
static void
drain (libusb_device_handle *h)
{
  uint8_t buf[128];
  int got;
  int n = 0;

  while (libusb_bulk_transfer (h, EP_IN, buf, sizeof buf, &got, 60) == 0)
    if (++n > 8)
      break;
}

static int
cmd (libusb_device_handle *h, uint8_t code,
     const uint8_t *payload, int paylen,
     uint8_t *rsp, int *rsp_len)
{
  uint8_t frame[128];
  int got = 0;
  int rc;

  encode (code, payload, paylen, frame);
  drain (h);

  rc = libusb_bulk_transfer (h, EP_OUT, frame, 128, &got, 2000);
  if (rc != 0)
    {
      printf ("       OUT 0x%02X rc=%d\n", code, rc);
      return rc;
    }

  rc = libusb_bulk_transfer (h, EP_IN, rsp, 128, rsp_len, 2000);
  g_frame++;

  return rc;
}

int
main (void)
{
  libusb_context *ctx = NULL;
  libusb_device **list = NULL;
  libusb_device_handle *h = NULL;
  struct libusb_device_descriptor desc;
  uint8_t rsp[128];
  int rsp_len = 0;
  ssize_t n;
  int rc;

  printf ("== mocsim self-test ==\n");

  if (libusb_init (&ctx) != 0)
    {
      printf ("libusb_init failed\n");
      return 1;
    }

  n = libusb_get_device_list (ctx, &list);
  if (n < 1)
    {
      printf ("FAIL no device enumerated (is libmocsim.so preloaded?)\n");
      return 1;
    }

  while (n-- > 0)
    {
      if (libusb_get_device_descriptor (list[n], &desc) != 0)
        continue;
      if (desc.idVendor == 0x28e9 && desc.idProduct == 0x5159)
        break;
    }

  if (desc.idVendor != 0x28e9)
    {
      printf ("FAIL device 28e9:5159 not found\n");
      return 1;
    }

  ok ("enumerated 28e9:5159");

  if (libusb_open ((libusb_device *) list[0], &h) != 0)
    {
      printf ("FAIL open\n");
      return 1;
    }

  libusb_claim_interface (h, 0);

  {
    unsigned char setup = 0;

    rc = libusb_control_transfer (h, 0xC0, 0xC0, 0, 0, &setup, 1, 2000);
    if (rc == 1 && setup == 0x5C)
      ok ("SETUP answered 0x5C");
    else
      fail ("SETUP", "wrong reply");
  }

  /* DB_OPEN */
  {
    uint8_t guid[36];

    memset (guid, 0, sizeof guid);
    rc = cmd (h, CMD_DB_OPEN, guid, 36, rsp, &rsp_len);
    if (rc == 0 && rsp_len == 128 && (rsp[4] == 0x27 || rsp[4] == 0x01 || rsp[4] == 0x29))
      ok ("DB_OPEN (0xDB)");
    else
      fail ("DB_OPEN", "no usable answer");
  }

  /* FW version */
  rc = cmd (h, CMD_GET_FW_VERSION, NULL, 0, rsp, &rsp_len);
  if (rc == 0 && rsp[4] == 0x01 && (rsp[0] | (rsp[1] << 8)) >= 4)
    printf ("  ok   GET_FW_VERSION = %02X %02X %02X %02X\n",
            rsp[6], rsp[7], rsp[8], rsp[9]), g_pass++;
  else
    fail ("GET_FW_VERSION", "bad payload");

  /* DB size: [8:12] must hold the used count */
  rc = cmd (h, CMD_DB_GET_SIZE, NULL, 0, rsp, &rsp_len);
  {
    int used = (rsp_len == 128) ? (rsp[6 + 8] | (rsp[6 + 9] << 8)) : -1;

    if (rc == 0 && used >= 0)
      printf ("  ok   DB_GET_SIZE: %d template(s)\n", used), g_pass++;
    else
      fail ("DB_GET_SIZE", "bad answer");
  }

  /* Dump record 0 and check the 80-byte record makes sense */
  {
    uint8_t idx = 0;

    rc = cmd (h, CMD_DB_DUMP_RECORD, &idx, 1, rsp, &rsp_len);
    if (rc == 0 && rsp[4] == 0x01 && (rsp[0] | (rsp[1] << 8)) == 84)
      {
        unsigned tid = rsp[6 + 4] | (rsp[6 + 5] << 8) |
                       (rsp[6 + 6] << 16) | (rsp[6 + 7] << 24);

        printf ("  ok   DB_DUMP_RECORD #0 → tid=%u\n", tid), g_pass++;
      }
    else
      fail ("DB_DUMP_RECORD", "expected 84-byte payload with ev=0x01");
  }

  /* Capture status is always 0x00 on this model */
  rc = cmd (h, CMD_CAPTURE_STATUS, NULL, 0, rsp, &rsp_len);
  if (rc == 0 && rsp[4] == 0x01 && rsp[6] == 0x00)
    ok ("0x30 = 0x00");
  else
    fail ("0x30", "unexpected");

  /* Verify capture: 20-byte metadata, [12:16] = 65 F7 00 00 */
  rc = cmd (h, CMD_VERIFY_CAPTURE, NULL, 0, rsp, &rsp_len);
  if (rc == 0 && rsp[4] == 0x01 && (rsp[0] | (rsp[1] << 8)) >= 20 &&
      rsp[6 + 12] == 0x65 && rsp[6 + 13] == 0xF7)
    ok ("0xC0 capture metadata");
  else
    fail ("0xC0", "expected 20-byte payload tagged 65 F7");

  /* Image block exchange */
  {
    uint8_t block[2048];
    int got = 0;

    rc = cmd (h, CMD_DUMP_DATA, (const uint8_t *) "\x02", 1, rsp, &rsp_len);
    if (rc != 0 || rsp[4] != 0x01)
      fail ("0x13", "no answer");

    memset (block, 0, sizeof block);
    block[0] = 0x08; block[1] = 0x09; block[8] = 0x13;
    memset (block + 9, 0x01, sizeof block - 9);

    rc = libusb_bulk_transfer (h, EP_OUT, block, 2048, &got, 2000);
    if (rc != 0)
      fail ("image OUT", "write failed");

    rc = libusb_bulk_transfer (h, EP_IN, block, 2048, &got, 2000);
    if (rc == 0 && got == 2048)
      ok ("image block round trip (2048 B)");
    else
      fail ("image block", "short read");
  }

  rc = cmd (h, CMD_PROCESS_STATUS, NULL, 0, rsp, &rsp_len);
  if (rc == 0 && (rsp[0] | (rsp[1] << 8)) >= 4)
    ok ("0x31 process status");
  else
    fail ("0x31", "bad answer");

  /* Matching verdict */
  {
    const char *want = getenv ("MOCSIM_MATCH");
    int expect_match = (want == NULL || *want != '0');

    rc = cmd (h, CMD_VERIFY_MATCHING, NULL, 0, rsp, &rsp_len);
    if (rc != 0 || (rsp[0] | (rsp[1] << 8)) < 40)
      {
        fail ("0xC1", "too short");
      }
    else
      {
        const uint8_t *b1 = rsp + 6 + 8;
        const uint8_t *b2 = rsp + 6 + 24;
        int diff = 0;

        for (int i = 9; i < 16; i++)
          diff += abs ((int) b1[i] - (int) b2[i]);

        if ((b1[8] == b2[8] && diff > 0 && expect_match) ||
            (b1[8] != b2[8] && diff == 0 && !expect_match))
          printf ("  ok   0xC1 verdict = %s\n", expect_match ? "match" : "no match"), g_pass++;
        else
          fail ("0xC1", "verdict does not match MOCSIM_MATCH");
      }
  }

  /* Wipe, then the database must be empty */
  rc = cmd (h, CMD_DB_WIPE, NULL, 0, rsp, &rsp_len);
  rc = cmd (h, CMD_DB_GET_SIZE, NULL, 0, rsp, &rsp_len);
  if (rc == 0 && (rsp[6 + 8] | (rsp[6 + 9] << 8)) == 0)
    ok ("DB_WIPE empties the database");
  else
    fail ("DB_WIPE", "database not empty");

  printf ("\n%d passed, %d failed\n", g_pass, g_fail);

  libusb_close (h);
  libusb_free_device_list (list, 1);
  libusb_exit (ctx);

  return g_fail == 0 ? 0 : 1;
}
