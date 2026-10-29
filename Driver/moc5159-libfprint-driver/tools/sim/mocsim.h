/*
 * mocsim.h — MOC-5159 fingerprint module simulator
 *
 * A software model of the Petaic MOC-5159 (GD32W515 + GSL6150) module that
 * speaks the same private 128-byte USB protocol as the real hardware.
 *
 * Two layers:
 *   mocsim-dev.c    protocol state machine (no USB, no libusb)
 *   mocsim-shim.c   LD_PRELOAD library that fakes libusb and routes every
 *                   bulk/control transfer into the state machine
 *
 * With the shim preloaded, an unmodified libfprint + this driver run against
 * the simulator exactly as they would against real hardware, which makes
 * enroll/verify/identify/delete testable without plugging anything in.
 */
#ifndef MOCSIM_H
#define MOCSIM_H

#include <stdint.h>
#include <stddef.h>

#define MOCSIM_FRAME_SIZE   128
#define MOCSIM_IMG_BLOCK    2048
#define MOCSIM_PAYLOAD_OFF  6
#define MOCSIM_CHECKSUM_OFF 126

/* ---- device model ---------------------------------------------------- */

typedef struct MocSimDev MocSimDev;

MocSimDev *mocsim_new (void);
void       mocsim_free (MocSimDev *dev);

/* Host -> device. Returns 0, or -1 if the frame is malformed. */
int  mocsim_out_frame (MocSimDev *dev, const uint8_t *frame, int len);
int  mocsim_out_block (MocSimDev *dev, const uint8_t *buf, int len);

/* Device -> host. Returns bytes written, or -1 when nothing is queued
 * (the caller reports LIBUSB_ERROR_TIMEOUT, which is what the real device
 * does when the IN side is idle). */
int  mocsim_in (MocSimDev *dev, uint8_t *buf, int maxlen);

/* Control transfer (only the 0xC0/0xC0 SETUP read is used by the protocol). */
int  mocsim_control (MocSimDev *dev,
                     uint8_t bmRequestType, uint8_t bRequest,
                     uint16_t wValue, uint16_t wIndex,
                     uint8_t *data, uint16_t wLength);

void mocsim_reset (MocSimDev *dev);

/* ---- scenario knobs -------------------------------------------------- */

/* How the next 0xC1 (VERIFY_MATCHING) answers: 1 = match, 0 = no match. */
void mocsim_set_verify_result (MocSimDev *dev, int matched);

/* Number of templates in the on-chip database at start (default 1). */
void mocsim_set_template_count (MocSimDev *dev, int n);

/* Inject faults, see MOCSIM_FAULT_* below. */
void mocsim_set_faults (MocSimDev *dev, unsigned mask);

/* How many times a command code has been issued (diagnostics / assertions). */
int  mocsim_cmd_count (MocSimDev *dev, uint8_t cmd);

/* Total number of 128-byte OUT frames seen. */
int  mocsim_frame_count (MocSimDev *dev);

/* Read back one template id (0-based index), 0 if absent. */
uint32_t mocsim_template_id (MocSimDev *dev, int index);
int      mocsim_template_used (MocSimDev *dev);

/* Human-readable dump of the simulated database (for logs). */
void mocsim_dump_db (MocSimDev *dev, void *FILEp);

/* fault bits */
#define MOCSIM_FAULT_NO_TAIL_FRAME   (1u << 0)  /* omit the 0xFF tail frame  */
#define MOCSIM_FAULT_STALE_IN        (1u << 1)  /* leave a leftover IN frame */
#define MOCSIM_FAULT_C1_TIMEOUT      (1u << 2)  /* 0xC1 never answers        */
#define MOCSIM_FAULT_ENROLL_QUALITY  (1u << 3)  /* first C4 returns 0xA0     */
#define MOCSIM_FAULT_BAD_CHECKSUM    (1u << 4)  /* corrupt 0xD5 replies      */
#define MOCSIM_FAULT_DEVICE_GONE     (1u << 5)  /* every transfer times out  */

#endif /* MOCSIM_H */
