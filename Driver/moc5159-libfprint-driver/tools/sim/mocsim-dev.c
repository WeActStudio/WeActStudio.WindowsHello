/*
 * mocsim-dev.c — protocol state machine of the MOC-5159 module
 *
 * Models what the firmware does: a 128-byte command/response frame protocol
 * with a small template database in "flash". Every command is answered with
 * one response frame, and (except for 0x13, which is followed by an image
 * block exchange) one trailing 0xFF frame, exactly as captured on real
 * hardware. Leaving that tail frame in the IN pipe is what makes the driver's
 * "drain before each command" requirement observable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mocsim.h"

#if defined(_MSC_VER)
#  define ARRAY_LEN(a) (sizeof (a) / sizeof ((a)[0]))
#else
#  define ARRAY_LEN(a) (sizeof (a) / sizeof ((a)[0]))
#endif

/* ---- protocol constants (mirror of src/moc5159.h) -------------------- */

#define CMD_CANCEL              0x0C
#define CMD_GET_FW_VERSION      0x10
#define CMD_DUMP_DATA           0x13
#define CMD_CAPTURE_STATUS      0x30
#define CMD_PROCESS_STATUS      0x31
#define CMD_VERIFY_CAPTURE      0xC0
#define CMD_VERIFY_MATCHING     0xC1
#define CMD_CREATE_ENROLLMENT   0xC2
#define CMD_COMMIT_ENROLLMENT   0xC3
#define CMD_ENROLL_CAPTURE      0xC4
#define CMD_ENROLL_APPLY        0xC5
#define CMD_DB_GET_SIZE         0xD5
#define CMD_DB_CURRENT_CONTENT  0xD4
#define CMD_DB_WIPE             0xD7
#define CMD_DB_UPDATE_TPL       0xD8
#define CMD_DB_DUMP_RECORD      0xD9
#define CMD_DB_CREATE           0xDA
#define CMD_DB_OPEN             0xDB
#define CMD_DB_CLOSE            0xDC
#define CMD_DB_DEL_RECORD       0xDF

#define EV_OK                   0x01
#define EV_CANCEL               0x0D
#define EV_UNSUPPORTED          0x26
#define EV_DB_DUPLICATED        0x27
#define EV_DB_EMPTY             0x29
#define EV_UNKNOWN_CMD          0x90
#define EV_IMG_NOT_QUALIFIED    0xA0
#define EV_NEED_LIFT            0xA2
#define EV_ENROLL_NEED_MORE     0xA6
#define EV_ENROLL_DONE          0xA7
#define EV_VERIFY_MATCH         0xAF

#define RECORD_SIZE             80
#define RECORD_CNT_OFF          76
#define MAX_TEMPLATES           10
#define MAX_ENROLL_STAGES       5

static const uint8_t template_guid[16] = {
  0xa1, 0xe0, 0x2e, 0xf3, 0x46, 0xa8, 0x12, 0x66,
  0xf2, 0xd5, 0xbc, 0x0f, 0xe9, 0x03, 0x00, 0x00
};

/* ---- IN queue -------------------------------------------------------- */

typedef struct {
  uint8_t *buf;
  int      len;
} InItem;

struct MocSimDev {
  /* on-chip database */
  uint8_t  rec[MAX_TEMPLATES][RECORD_SIZE];
  uint32_t tid[MAX_TEMPLATES];
  int      used;
  uint32_t counter;          /* global monotonic counter in record[76:80] */

  /* pending IN frames */
  InItem q[64];
  int    qhead, qtail, qcount;

  /* scenario */
  int      verify_matched;
  int      enroll_c5_count;
  int      enroll_stage;
  int      stale_injected;
  int      quality_failed;   /* set once the injected bad capture happened */
  unsigned faults;

  /* accounting */
  int      nframe;
  int      ncmd[256];
  int      out_block;        /* image blocks seen */
  int      in_block;         /* image blocks returned */
  uint8_t  last_img_tx[2048];
  int      have_img_tx;
};

/* ---- little helpers -------------------------------------------------- */

static uint32_t rd32 (const uint8_t *p)
{
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
         ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void wr32 (uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t) (v & 0xFF);
  p[1] = (uint8_t) ((v >> 8) & 0xFF);
  p[2] = (uint8_t) ((v >> 16) & 0xFF);
  p[3] = (uint8_t) ((v >> 24) & 0xFF);
}

static uint16_t frame_checksum (const uint8_t *f)
{
  uint32_t sum = 0;
  int i;

  for (i = 0; i < MOCSIM_CHECKSUM_OFF; i++)
    sum += f[i];

  return (uint16_t) (sum & 0xFFFF);
}

/* Push one item onto the IN queue. Takes ownership of `buf`. */
static void
q_push (MocSimDev *d, uint8_t *buf, int len)
{
  if (d->qcount >= (int) ARRAY_LEN (d->q))
    {
      fprintf (stderr, "[mocsim] IN queue overflow, dropping %d bytes\n", len);
      free (buf);
      return;
    }

  d->q[d->qtail].buf = buf;
  d->q[d->qtail].len = len;
  d->qtail = (d->qtail + 1) % (int) ARRAY_LEN (d->q);
  d->qcount++;
}

/* Build and queue one 128-byte response frame. */
static void
push_frame (MocSimDev *d, uint8_t cmd, uint8_t ev,
            const uint8_t *payload, int paylen)
{
  uint8_t *f = calloc (1, MOCSIM_FRAME_SIZE);
  uint16_t cks;

  if (paylen < 0)
    paylen = 0;
  if (paylen > MOCSIM_CHECKSUM_OFF - MOCSIM_PAYLOAD_OFF)
    paylen = MOCSIM_CHECKSUM_OFF - MOCSIM_PAYLOAD_OFF;

  f[0] = (uint8_t) (paylen & 0xFF);
  f[1] = (uint8_t) (paylen >> 8);
  f[2] = cmd;
  f[4] = ev;

  if (paylen > 0 && payload != NULL)
    memcpy (f + MOCSIM_PAYLOAD_OFF, payload, (size_t) paylen);

  cks = frame_checksum (f);
  if (d->faults & MOCSIM_FAULT_BAD_CHECKSUM)
    cks ^= 0x5A5A;
  f[MOCSIM_CHECKSUM_OFF]     = (uint8_t) (cks & 0xFF);
  f[MOCSIM_CHECKSUM_OFF + 1] = (uint8_t) (cks >> 8);

  q_push (d, f, MOCSIM_FRAME_SIZE);
}

/* Every command except 0x13 is followed by a 0xFF tail frame on real HW. */
static void
push_tail (MocSimDev *d)
{
  if (d->faults & MOCSIM_FAULT_NO_TAIL_FRAME)
    return;

  push_frame (d, 0xFF, 0x00, NULL, 0);
}

/* ---- database -------------------------------------------------------- */

static void
db_add (MocSimDev *d, uint32_t tid)
{
  int i = d->used;

  if (i >= MAX_TEMPLATES)
    return;

  memset (d->rec[i], 0, RECORD_SIZE);
  wr32 (d->rec[i] + 0, tid);
  wr32 (d->rec[i] + 4, 0x1c);
  wr32 (d->rec[i] + 8, 0x501);
  wr32 (d->rec[i] + 12, 0x05000000);
  wr32 (d->rec[i] + 16, 0x15);
  memcpy (d->rec[i] + 20, template_guid, sizeof template_guid);
  wr32 (d->rec[i] + RECORD_CNT_OFF, ++d->counter);
  d->tid[i] = tid;
  d->used++;
}

static void
db_del (MocSimDev *d, int index)
{
  int i;

  if (index < 0 || index >= d->used)
    return;

  for (i = index; i + 1 < d->used; i++)
    {
      memcpy (d->rec[i], d->rec[i + 1], RECORD_SIZE);
      d->tid[i] = d->tid[i + 1];
    }

  d->used--;
  memset (d->rec[d->used], 0, RECORD_SIZE);
  d->tid[d->used] = 0;
}


/* ---- C1 verdict payload --------------------------------------------- */

/*
 * The driver decides match/no-match purely from the relation between two
 * 16-byte blocks at payload[8:24] and payload[24:40]:
 *
 *   match    : b1[8] == b2[8]  and  b1[9:16] != b2[9:16]
 *   no match : b1[8] != b2[8]  and  b1[9:16] == b2[9:16]
 */
static void
build_c1_payload (int matched, uint8_t out[120])
{
  memset (out, 0, 120);
  wr32 (out + 0, 0x00000123);      /* tick, unused by the driver */
  wr32 (out + 4, 0x00000001);      /* status, unused by the driver */

  if (matched)
    {
      out[8 + 8] = 0x49;
      out[24 + 8] = 0x49;
      /* differ inside [9:16] */
      out[8 + 9] = 0x11; out[8 + 10] = 0x22; out[8 + 11] = 0x33;
      out[8 + 12] = 0x44; out[8 + 13] = 0x55; out[8 + 14] = 0x66;
      out[8 + 15] = 0x77;
      out[24 + 9] = 0x19; out[24 + 10] = 0x2a; out[24 + 11] = 0x3b;
      out[24 + 12] = 0x4c; out[24 + 13] = 0x5d; out[24 + 14] = 0x6e;
      out[24 + 15] = 0x7f;
    }
  else
    {
      out[8 + 8] = 0x49;
      out[24 + 8] = 0x89;
      /* identical inside [9:16] */
      out[8 + 9] = 0x11; out[8 + 10] = 0x22; out[8 + 11] = 0x33;
      out[8 + 12] = 0x44; out[8 + 13] = 0x55; out[8 + 14] = 0x66;
      out[8 + 15] = 0x77;
      memcpy (out + 24 + 9, out + 8 + 9, 7);
    }

  /* fixed trailer seen in every capture */
  {
    static const uint8_t tail[12] = {
      0x57, 0x8d, 0x07, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00
    };
    memcpy (out + 108, tail, sizeof tail);
  }
}

/* ---- public API ------------------------------------------------------ */

MocSimDev *
mocsim_new (void)
{
  MocSimDev *d = calloc (1, sizeof *d);

  if (d == NULL)
    return NULL;

  /* A fresh module with one enrolled finger is the most useful default:
   * list/verify/delete all have something to work with. */
  d->verify_matched = 1;
  db_add (d, 3);

  return d;
}

void
mocsim_free (MocSimDev *d)
{
  int i;

  if (d == NULL)
    return;

  for (i = 0; i < (int) ARRAY_LEN (d->q); i++)
    free (d->q[i].buf);

  free (d);
}

void
mocsim_set_verify_result (MocSimDev *d, int matched)
{
  d->verify_matched = matched ? 1 : 0;
}

void
mocsim_set_template_count (MocSimDev *d, int n)
{
  d->used = 0;
  d->counter = 0;

  for (int i = 0; i < n && i < MAX_TEMPLATES; i++)
    db_add (d, (uint32_t) (i + 1));
}

void
mocsim_set_faults (MocSimDev *d, unsigned mask)
{
  d->faults = mask;
}

int
mocsim_cmd_count (MocSimDev *d, uint8_t cmd)
{
  return d->ncmd[cmd];
}

int
mocsim_frame_count (MocSimDev *d)
{
  return d->nframe;
}

uint32_t
mocsim_template_id (MocSimDev *d, int index)
{
  return (index >= 0 && index < d->used) ? d->tid[index] : 0;
}

int
mocsim_template_used (MocSimDev *d)
{
  return d->used;
}

void
mocsim_dump_db (MocSimDev *d, void *FILEp)
{
  FILE *fp = FILEp ? (FILE *) FILEp : stderr;

  fprintf (fp, "[mocsim] db: %d template(s), counter=%u\n",
           d->used, d->counter);

  for (int i = 0; i < d->used; i++)
    fprintf (fp, "[mocsim]   #%d tid=%u cnt=%u\n",
             i, d->tid[i], rd32 (d->rec[i] + RECORD_CNT_OFF));
}

void
mocsim_reset (MocSimDev *d)
{
  for (int i = 0; i < (int) ARRAY_LEN (d->q); i++)
    {
      free (d->q[i].buf);
      d->q[i].buf = NULL;
      d->q[i].len = 0;
    }

  d->qhead = d->qtail = d->qcount = 0;
  d->enroll_c5_count = 0;
  d->enroll_stage = 0;
  d->stale_injected = 0;
  d->quality_failed = 0;
}

int
mocsim_in (MocSimDev *d, uint8_t *buf, int maxlen)
{
  InItem *it;
  int n;

  if (d->faults & MOCSIM_FAULT_DEVICE_GONE)
    return -1;

  if (d->qcount == 0)
    return -1;                    /* caller maps this to LIBUSB timeout */

  it = &d->q[d->qhead];
  n = it->len < maxlen ? it->len : maxlen;
  memcpy (buf, it->buf, (size_t) n);

  /* A short host buffer consumes the whole item: libusb would return the
   * truncated length and the remainder would be lost, which never happens
   * because the driver always uses the exact expected size. */
  free (it->buf);
  it->buf = NULL;
  it->len = 0;
  d->qhead = (d->qhead + 1) % (int) ARRAY_LEN (d->q);
  d->qcount--;

  return n;
}

int
mocsim_control (MocSimDev *d,
                uint8_t bmRequestType, uint8_t bRequest,
                uint16_t wValue, uint16_t wIndex,
                uint8_t *data, uint16_t wLength)
{
  (void) d;
  (void) wValue;
  (void) wIndex;

  /* Session SETUP: vendor IN, request 0xC0, one byte, value 0x5C. */
  if ((bmRequestType & 0xC0) == 0xC0 && bRequest == 0xC0)
    {
      if (wLength >= 1 && data != NULL)
        {
          data[0] = 0x5C;
          return 1;
        }
      return 0;
    }

  return -1;
}

/* Image block exchange: host sends 2048 bytes, device echoes a block. */
int
mocsim_out_block (MocSimDev *d, const uint8_t *buf, int len)
{
  uint8_t *echo;

  if (d->faults & MOCSIM_FAULT_DEVICE_GONE)
    return -1;

  d->out_block++;
  if (len > 0)
    {
      int n = len < (int) sizeof d->last_img_tx ? len : (int) sizeof d->last_img_tx;
      memcpy (d->last_img_tx, buf, (size_t) n);
      d->have_img_tx = 1;
    }

  /* The driver only needs the transfer to complete; image content is
   * irrelevant because matching happens on the chip. */
  echo = calloc (1, MOCSIM_IMG_BLOCK);
  echo[0] = 0x08;
  echo[1] = 0x09;
  echo[4] = 0xF7;
  echo[5] = 0x07;
  echo[8] = 0x13;
  memset (echo + 9, 0x01, MOCSIM_IMG_BLOCK - 9);
  q_push (d, echo, MOCSIM_IMG_BLOCK);
  d->in_block++;

  return 0;
}

int
mocsim_out_frame (MocSimDev *d, const uint8_t *frame, int len)
{
  uint8_t cmd;
  uint8_t payload[128];
  int paylen;
  uint16_t stored, calc;

  if (len < MOCSIM_FRAME_SIZE)
    return -1;

  if (d->faults & MOCSIM_FAULT_DEVICE_GONE)
    return -1;

  /* A leftover frame is what a device does after a command the host never
   * read: it blocks the next OUT until the host drains it. */
  if ((d->faults & MOCSIM_FAULT_STALE_IN) && !d->stale_injected)
    {
      uint8_t *junk = calloc (1, MOCSIM_FRAME_SIZE);
      junk[0] = 0;
      junk[2] = 0xDC;
      junk[4] = EV_OK;
      stored = frame_checksum (junk);
      junk[MOCSIM_CHECKSUM_OFF]     = (uint8_t) (stored & 0xFF);
      junk[MOCSIM_CHECKSUM_OFF + 1] = (uint8_t) (stored >> 8);
      q_push (d, junk, MOCSIM_FRAME_SIZE);
      d->stale_injected = 1;
    }

  d->nframe++;

  cmd    = frame[2];
  paylen = (int) frame[0] | ((int) frame[1] << 8);
  if (paylen > MOCSIM_CHECKSUM_OFF - MOCSIM_PAYLOAD_OFF)
    paylen = MOCSIM_CHECKSUM_OFF - MOCSIM_PAYLOAD_OFF;

  memset (payload, 0, sizeof payload);
  if (paylen > 0)
    memcpy (payload, frame + MOCSIM_PAYLOAD_OFF, (size_t) paylen);

  /* The real firmware rejects frames with a bad checksum. */
  stored = (uint16_t) (frame[MOCSIM_CHECKSUM_OFF] |
                       (frame[MOCSIM_CHECKSUM_OFF + 1] << 8));
  calc = frame_checksum (frame);
  if (stored != calc)
    {
      fprintf (stderr, "[mocsim] bad checksum on cmd 0x%02X "
                       "(stored 0x%04X, calc 0x%04X)\n",
               cmd, stored, calc);
      push_frame (d, cmd, 0x9C, NULL, 0);
      push_tail (d);
      return 0;
    }

  d->ncmd[cmd]++;

  switch (cmd)
    {
    case CMD_CANCEL:
      /* Cancelling the enrollment session does not touch the database. */
      d->enroll_c5_count = 0;
      d->enroll_stage = 0;
      push_frame (d, cmd, EV_CANCEL, NULL, 0);
      break;

    case CMD_GET_FW_VERSION:
      {
        uint8_t fw[4] = { 0x04, 0x00, 0x69, 0x00 };   /* 4.0.105 */
        push_frame (d, cmd, EV_OK, fw, sizeof fw);
      }
      break;

    case CMD_DB_OPEN:
      /* 0x27 = header already there; 0x29 = empty header (needs 0xDA). */
      push_frame (d, cmd, d->used > 0 ? EV_DB_DUPLICATED : EV_DB_EMPTY,
                  NULL, 0);
      break;

    case CMD_DB_CREATE:
      push_frame (d, cmd, EV_OK, NULL, 0);
      break;

    case CMD_DB_CLOSE:
      push_frame (d, cmd, EV_OK, NULL, 0);
      break;

    case CMD_DB_GET_SIZE:
      {
        /* [0:4] free slots, [8:12] used templates — the driver must use
         * [8:12], and using [0:4] here would make it dump past the end. */
        uint8_t sz[12];
        memset (sz, 0, sizeof sz);
        wr32 (sz + 0, (uint32_t) (MAX_TEMPLATES - d->used));
        wr32 (sz + 8, (uint32_t) d->used);
        push_frame (d, cmd, EV_OK, sz, sizeof sz);
      }
      break;

    case CMD_DB_DUMP_RECORD:
      {
        int idx = paylen >= 1 ? payload[0] : 0;

        if (idx < d->used)
          {
            uint8_t out[4 + RECORD_SIZE];
            memset (out, 0, sizeof out);
            /* 4-byte header then the 80-byte record, as captured */
            memcpy (out + 4, d->rec[idx], RECORD_SIZE);
            push_frame (d, cmd, EV_OK, out, sizeof out);
          }
        else
          {
            /* Out-of-range index: the firmware answers 0x26 with zeros. */
            uint8_t out[4 + RECORD_SIZE];
            memset (out, 0, sizeof out);
            push_frame (d, cmd, EV_UNSUPPORTED, out, sizeof out);
          }
      }
      break;

    case CMD_DB_UPDATE_TPL:
      /* Template adaptation write-back; the driver ignores the result. */
      push_frame (d, cmd, EV_OK, NULL, 0);
      break;

    case CMD_DB_CURRENT_CONTENT:
      /* Not supported on this firmware — the driver falls back to "matched". */
      push_frame (d, cmd, EV_UNSUPPORTED, NULL, 0);
      break;

    case CMD_DB_WIPE:
      d->used = 0;
      memset (d->rec, 0, sizeof d->rec);
      memset (d->tid, 0, sizeof d->tid);
      push_frame (d, cmd, EV_OK, NULL, 0);
      break;

    case CMD_DB_DEL_RECORD:
      {
        uint32_t want;
        int found = -1;

        if (paylen >= RECORD_SIZE)
          {
            want = rd32 (payload);
            for (int i = 0; i < d->used; i++)
              if (d->tid[i] == want)
                {
                  found = i;
                  break;
                }
          }

        if (found >= 0)
          {
            uint8_t echo[RECORD_SIZE];
            memcpy (echo, d->rec[found], RECORD_SIZE);
            db_del (d, found);
            push_frame (d, cmd, EV_OK, echo, RECORD_SIZE);
          }
        else
          {
            push_frame (d, cmd, EV_UNSUPPORTED, NULL, 0);
          }
      }
      break;

    case CMD_CAPTURE_STATUS:
      /* Always 0x00 on this model; unusable for finger detection. */
      {
        uint8_t st[1] = { 0x00 };
        push_frame (d, cmd, EV_OK, st, sizeof st);
      }
      break;

    case CMD_PROCESS_STATUS:
      {
        uint8_t st[4] = { 0x47, 0x00, 0x00, 0x00 };
        push_frame (d, cmd, EV_OK, st, sizeof st);
      }
      break;

    case CMD_VERIFY_CAPTURE:
      {
        /* 20-byte metadata: quality in [2:4], "verify" tag in [12:16]. */
        uint8_t meta[20];
        memset (meta, 0, sizeof meta);
        meta[2] = 0x50;
        meta[3] = 0x02;
        meta[12] = 0x65; meta[13] = 0xF7;
        meta[16] = 0x21;
        push_frame (d, cmd, EV_OK, meta, sizeof meta);
      }
      break;

    case CMD_ENROLL_CAPTURE:
      {
        uint8_t meta[20];
        memset (meta, 0, sizeof meta);
        meta[2] = 0x50;
        meta[3] = 0x02;
        meta[12] = 0xFF; meta[13] = 0xFF;
        meta[16] = 0x21;

        /* Fail the very first capture only, so the driver's retry path is
         * exercised and the enrollment still succeeds afterwards. */
        if ((d->faults & MOCSIM_FAULT_ENROLL_QUALITY) && !d->quality_failed)
          {
            d->quality_failed = 1;
            push_frame (d, cmd, EV_IMG_NOT_QUALIFIED, meta, sizeof meta);
          }
        else
          {
            push_frame (d, cmd, EV_OK, meta, sizeof meta);
          }
      }
      break;

    case CMD_CREATE_ENROLLMENT:
      d->enroll_c5_count = 0;
      d->enroll_stage = 0;
      push_frame (d, cmd, EV_OK, NULL, 0);
      break;

    case CMD_ENROLL_APPLY:
      /* 0xA6 = this sample accepted, keep going; 0xA7 = all stages done. */
      d->enroll_c5_count++;
      if (d->enroll_c5_count >= MAX_ENROLL_STAGES)
        {
          d->enroll_stage = MAX_ENROLL_STAGES - 1;
          push_frame (d, cmd, EV_ENROLL_DONE, NULL, 0);
        }
      else
        {
          d->enroll_stage = d->enroll_c5_count;
          push_frame (d, cmd, EV_ENROLL_NEED_MORE, NULL, 0);
        }
      break;

    case CMD_COMMIT_ENROLLMENT:
      {
        uint32_t tid;

        if (paylen >= RECORD_SIZE)
          {
            tid = rd32 (payload);
            /* Same tid as an existing record = extra sample for it. */
            db_add (d, tid);
            if (d->used >= 2 && d->tid[d->used - 2] == tid)
              fprintf (stderr, "[mocsim] C3 appended a sample to tid=%u "
                               "(now %d records, firmware matches #0 only)\n",
                       tid, d->used);

            /* The driver requires at least 4 payload bytes from C3 and reads
             * the template id from them, so answer with the id we stored. */
            {
              uint8_t idbuf[4];

              wr32 (idbuf, tid);
              push_frame (d, cmd, EV_OK, idbuf, sizeof idbuf);
            }
          }
        else
          {
            push_frame (d, cmd, 0x91, NULL, 0);   /* length error */
          }
      }
      break;

    case CMD_VERIFY_MATCHING:
      {
        uint8_t c1[120];

        if (d->faults & MOCSIM_FAULT_C1_TIMEOUT)
          {
            /* No answer at all: the driver retries then fails. */
            break;
          }

        build_c1_payload (d->verify_matched, c1);
        push_frame (d, cmd, d->verify_matched ? EV_VERIFY_MATCH : EV_OK,
                    c1, sizeof c1);
      }
      break;

    case CMD_DUMP_DATA:
      /* Prepare the image block exchange; the 2048-byte OUT follows. */
      push_frame (d, cmd, EV_OK, NULL, 0);
      break;

    default:
      fprintf (stderr, "[mocsim] unknown command 0x%02X (%d byte payload)\n",
               cmd, paylen);
      push_frame (d, cmd, EV_UNKNOWN_CMD, NULL, 0);
      break;
    }

  /* 0x13 is immediately followed by the 2048-byte image block, so it does
   * not carry a tail frame; everything else does. */
  if (cmd != CMD_DUMP_DATA)
    push_tail (d);

  return 0;
}
