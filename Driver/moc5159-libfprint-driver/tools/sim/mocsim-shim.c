/*
 * mocsim-shim.c — LD_PRELOAD library that fakes libusb for the MOC-5159 module
 *
 * Build:  cc -shared -fPIC -O2 -o libmocsim.so mocsim-shim.c mocsim-dev.c \
 *            $(pkg-config --cflags libusb-1.0) -ldl
 *
 * Use:    LD_PRELOAD=/path/libmocsim.so fprintd-verify
 *
 * The shim exports the subset of the libusb 1.0 API that libgusb (and hence
 * libfprint) uses, enumerates exactly one device (28e9:5159) with the same
 * descriptors as the real module, and routes every bulk/control transfer into
 * the protocol state machine in mocsim-dev.c.
 *
 * Environment:
 *   MOCSIM_MATCH=0|1      outcome of the next 0xC1 matching command (1)
 *   MOCSIM_TEMPLATES=N    templates present in the on-chip database (1)
 *   MOCSIM_FAULT=a,b,c    see the MOCSIM_FAULT_* bits:
 *                         notail, stale, c1timeout, quality, badcrc, gone
 *   MOCSIM_LOG=1          log every command to stderr
 *   MOCSIM_DB=1           dump the database on exit
 *   MOCSIM_SPEED=N        artificial delay per transfer, in milliseconds (0)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb-1.0/libusb.h>

#include "mocsim.h"

#define VENDOR_ID   0x28e9
#define PRODUCT_ID  0x5159
#define IFACE       0

/* ---- singleton state ------------------------------------------------- */

static MocSimDev *g_dev;
static int        g_ctx_token;
static int        g_dev_token;
static int        g_hub_token;      /* fake root hub: libfprint walks up to it */
static int        g_handle_token;
static int        g_speed_ms;
static int        g_log;

/* libgusb runs an event thread *and* pumps events from the main context, so
 * libusb entry points are reached from two threads at once. Real libusb
 * serialises internally; without the same protection here both threads can
 * pop the same pending transfer and deliver it twice, which completes one
 * GTask twice and aborts the process. The lock also has to be recursive:
 * pump() services a transfer while holding it, and servicing reaches sim()
 * which may lazily initialise the device and take the lock again. */
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static const char *g_env_fault;

static int
env_flag (const char *name, int dflt)
{
  const char *v = getenv (name);

  if (v == NULL || *v == '\0')
    return dflt;

  return (*v != '0');
}

static unsigned
parse_faults (const char *s)
{
  unsigned mask = 0;

  if (s == NULL)
    return 0;

  if (strstr (s, "notail"))    mask |= MOCSIM_FAULT_NO_TAIL_FRAME;
  if (strstr (s, "stale"))     mask |= MOCSIM_FAULT_STALE_IN;
  if (strstr (s, "c1timeout")) mask |= MOCSIM_FAULT_C1_TIMEOUT;
  if (strstr (s, "quality"))   mask |= MOCSIM_FAULT_ENROLL_QUALITY;
  if (strstr (s, "badcrc"))    mask |= MOCSIM_FAULT_BAD_CHECKSUM;
  if (strstr (s, "gone"))      mask |= MOCSIM_FAULT_DEVICE_GONE;

  return mask;
}

static MocSimDev *
sim (void)
{
  if (g_dev != NULL)
    return g_dev;

  pthread_mutex_lock (&g_lock);
  if (g_dev == NULL)
    {
      const char *v;

      g_dev = mocsim_new ();
      g_log = env_flag ("MOCSIM_LOG", 0);

      v = getenv ("MOCSIM_TEMPLATES");
      if (v != NULL && *v != '\0')
        mocsim_set_template_count (g_dev, atoi (v));

      v = getenv ("MOCSIM_MATCH");
      if (v != NULL && *v != '\0')
        mocsim_set_verify_result (g_dev, atoi (v) != 0);

      v = getenv ("MOCSIM_SPEED");
      if (v != NULL && *v != '\0')
        g_speed_ms = atoi (v);

      g_env_fault = getenv ("MOCSIM_FAULT");
      mocsim_set_faults (g_dev, parse_faults (g_env_fault));

      if (g_log)
        fprintf (stderr, "[mocsim] device up: %d template(s), match=%d, faults=%s\n",
                 mocsim_template_used (g_dev),
                 getenv ("MOCSIM_MATCH") ? atoi (getenv ("MOCSIM_MATCH")) : 1,
                 g_env_fault ? g_env_fault : "none");
    }
  pthread_mutex_unlock (&g_lock);

  return g_dev;
}

__attribute__ ((destructor))
static void
mocsim_teardown (void)
{
  if (g_dev != NULL && env_flag ("MOCSIM_DB", 0))
    mocsim_dump_db (g_dev, stderr);

  mocsim_free (g_dev);
  g_dev = NULL;
}

/* ---- descriptors ----------------------------------------------------- */

static void
fill_device_descriptor (const libusb_device *dev,
                        struct libusb_device_descriptor *d)
{
  memset (d, 0, sizeof *d);

  /* Fake root hub. libfprint's wakeup configuration walks the parent chain
   * and assumes it ends at a non-NULL parent (the root hub); returning NULL
   * straight away makes it compute a string length of -1 and abort. */
  if ((const void *) dev == (const void *) &g_hub_token)
    {
      d->bLength            = 18;
      d->bDescriptorType    = LIBUSB_DT_DEVICE;
      d->bcdUSB             = 0x0200;
      d->bDeviceClass       = LIBUSB_CLASS_HUB;
      d->bMaxPacketSize0    = 64;
      d->idVendor           = 0x1d6b;   /* Linux Foundation */
      d->idProduct          = 0x0002;   /* 2.0 root hub */
      d->bcdDevice          = 0x0608;
      d->bNumConfigurations = 1;
      return;
    }

  d->bLength            = 18;
  d->bDescriptorType    = LIBUSB_DT_DEVICE;
  d->bcdUSB             = 0x0200;
  d->bDeviceClass       = 0;
  d->bDeviceSubClass    = 0;
  d->bDeviceProtocol    = 0;
  d->bMaxPacketSize0    = 64;
  d->idVendor           = VENDOR_ID;
  d->idProduct          = PRODUCT_ID;
  d->bcdDevice          = 0x0100;
  d->iManufacturer      = 1;
  d->iProduct           = 2;
  d->iSerialNumber      = 0;
  d->bNumConfigurations = 1;
}

/* The config/interface/endpoint trio must be a stable tree; libgusb keeps
 * pointers into it for the lifetime of the GUsbDevice. */
static struct libusb_config_descriptor *
make_config_descriptor (void)
{
  struct libusb_config_descriptor   *cfg;
  struct libusb_interface           *iface;
  struct libusb_interface_descriptor *alt;
  struct libusb_endpoint_descriptor *eps;

  cfg   = calloc (1, sizeof *cfg);
  iface = calloc (1, sizeof *iface);
  alt   = calloc (1, sizeof *alt);
  eps   = calloc (2, sizeof *eps);
  if (cfg == NULL || iface == NULL || alt == NULL || eps == NULL)
    {
      free (cfg); free (iface); free (alt); free (eps);
      return NULL;
    }

  cfg->bLength             = 9;
  cfg->bDescriptorType     = LIBUSB_DT_CONFIG;
  cfg->wTotalLength        = 9 + 9 + 7 + 7;
  cfg->bNumInterfaces      = 1;
  cfg->bConfigurationValue = 1;
  cfg->iConfiguration      = 0;
  cfg->bmAttributes        = 0x80;
  cfg->MaxPower            = 0x32;
  cfg->interface           = iface;
  cfg->extra               = NULL;
  cfg->extra_length        = 0;

  iface->altsetting   = alt;
  iface->num_altsetting = 1;

  alt->bLength            = 9;
  alt->bDescriptorType    = LIBUSB_DT_INTERFACE;
  alt->bInterfaceNumber   = IFACE;
  alt->bAlternateSetting  = 0;
  alt->bNumEndpoints      = 2;
  alt->bInterfaceClass    = 0xFF;      /* vendor specific, not HID */
  alt->bInterfaceSubClass = 0;
  alt->bInterfaceProtocol = 0;
  alt->iInterface         = 0;
  alt->endpoint           = eps;
  alt->extra              = NULL;
  alt->extra_length       = 0;

  eps[0].bLength          = 7;
  eps[0].bDescriptorType  = LIBUSB_DT_ENDPOINT;
  eps[0].bEndpointAddress = 0x01;      /* OUT */
  eps[0].bmAttributes     = LIBUSB_TRANSFER_TYPE_BULK;
  eps[0].wMaxPacketSize   = 64;
  eps[0].bInterval        = 0;

  eps[1].bLength          = 7;
  eps[1].bDescriptorType  = LIBUSB_DT_ENDPOINT;
  eps[1].bEndpointAddress = 0x81;      /* IN */
  eps[1].bmAttributes     = LIBUSB_TRANSFER_TYPE_BULK;
  eps[1].wMaxPacketSize   = 64;
  eps[1].bInterval        = 0;

  return cfg;
}

/* ---- libusb: core ---------------------------------------------------- */

int
libusb_init (libusb_context **ctx)
{
  if (ctx != NULL)
    *ctx = (libusb_context *) &g_ctx_token;

  sim ();
  return 0;
}

void
libusb_exit (libusb_context *ctx)
{
  (void) ctx;
  if (g_dev != NULL)
    mocsim_reset (g_dev);
}

int
libusb_set_option (libusb_context *ctx, enum libusb_option option, ...)
{
  (void) ctx;
  (void) option;
  return 0;
}

int
libusb_has_capability (uint32_t capability)
{
  (void) capability;
  return 0;                     /* no hotplug, no capture, no HID access */
}

const char *
libusb_error_name (int code)
{
  static char buf[32];

  snprintf (buf, sizeof buf, "MOCSIM_ERR_%d", code);
  return buf;
}

const char *
libusb_strerror (int code)
{
  return libusb_error_name (code);
}

/* ---- libusb: enumeration --------------------------------------------- */

ssize_t
libusb_get_device_list (libusb_context *ctx, libusb_device ***list)
{
  libusb_device **out;

  (void) ctx;
  sim ();

  out = calloc (3, sizeof *out);
  if (out == NULL)
    return LIBUSB_ERROR_NO_MEM;

  out[0] = (libusb_device *) &g_dev_token;
  out[1] = (libusb_device *) &g_hub_token;
  out[2] = NULL;
  *list = out;

  return 2;
}

void
libusb_free_device_list (libusb_device **list, int unref_devices)
{
  (void) unref_devices;
  free (list);
}

libusb_device *
libusb_ref_device (libusb_device *dev)
{
  return dev;
}

void
libusb_unref_device (libusb_device *dev)
{
  (void) dev;
}

int
libusb_get_device_descriptor (libusb_device *dev,
                              struct libusb_device_descriptor *desc)
{
  fill_device_descriptor (dev, desc);
  return 0;
}

int
libusb_get_active_config_descriptor (libusb_device *dev,
                                     struct libusb_config_descriptor **config)
{
  if ((const void *) dev == (const void *) &g_hub_token)
    {
      struct libusb_config_descriptor *cfg = calloc (1, sizeof *cfg);

      if (cfg == NULL)
        return LIBUSB_ERROR_NO_MEM;

      cfg->bLength             = 9;
      cfg->bDescriptorType     = LIBUSB_DT_CONFIG;
      cfg->wTotalLength        = 9;
      cfg->bNumInterfaces      = 0;
      cfg->bConfigurationValue = 1;
      cfg->bmAttributes        = 0xE0;
      cfg->MaxPower            = 0;
      *config = cfg;
      return 0;
    }

  *config = make_config_descriptor ();
  return *config ? 0 : LIBUSB_ERROR_NO_MEM;
}

int
libusb_get_config_descriptor (libusb_device *dev, uint8_t index,
                              struct libusb_config_descriptor **config)
{
  (void) dev;
  if (index != 0)
    return LIBUSB_ERROR_NOT_FOUND;

  *config = make_config_descriptor ();
  return *config ? 0 : LIBUSB_ERROR_NO_MEM;
}

void
libusb_free_config_descriptor (struct libusb_config_descriptor *config)
{
  if (config == NULL)
    return;

  if (config->interface != NULL)
    {
      if (config->interface[0].altsetting != NULL)
        {
          free ((void *) config->interface[0].altsetting[0].endpoint);
          free ((void *) config->interface[0].altsetting);
        }
      free ((void *) config->interface);
    }

  free (config);
}

int
libusb_get_bos_descriptor (libusb_device_handle *handle,
                           struct libusb_bos_descriptor **bos)
{
  (void) handle;
  (void) bos;
  return LIBUSB_ERROR_NOT_FOUND;
}

void
libusb_free_bos_descriptor (struct libusb_bos_descriptor *bos)
{
  (void) bos;
}

uint8_t
libusb_get_bus_number (libusb_device *dev)
{
  (void) dev;
  return 1;
}

uint8_t
libusb_get_port_number (libusb_device *dev)
{
  /* module sits on port 2 of the root hub, so the sysfs node is 1-2 */
  return ((const void *) dev == (const void *) &g_hub_token) ? 1 : 2;
}

int
libusb_get_port_numbers (libusb_device *dev, uint8_t *port_numbers, int port_numbers_len)
{
  (void) dev;

  if (port_numbers_len >= 1)
    {
      port_numbers[0] = 1;
      return 1;
    }

  return LIBUSB_ERROR_OVERFLOW;
}

libusb_device *
libusb_get_parent (libusb_device *dev)
{
  if ((const void *) dev == (const void *) &g_hub_token)
    return NULL;

  return (libusb_device *) &g_hub_token;
}

uint8_t
libusb_get_device_address (libusb_device *dev)
{
  return ((const void *) dev == (const void *) &g_hub_token) ? 1 : 3;
}

int
libusb_get_device_speed (libusb_device *dev)
{
  (void) dev;
  return LIBUSB_SPEED_FULL;
}

/* ---- libusb: open / claim -------------------------------------------- */

int
libusb_open (libusb_device *dev, libusb_device_handle **handle)
{
  (void) dev;
  *handle = (libusb_device_handle *) &g_handle_token;
  return 0;
}

void
libusb_close (libusb_device_handle *handle)
{
  (void) handle;
}

libusb_device *
libusb_get_device (libusb_device_handle *handle)
{
  (void) handle;
  return (libusb_device *) &g_dev_token;
}

int
libusb_get_configuration (libusb_device_handle *handle, int *config)
{
  (void) handle;
  *config = 1;
  return 0;
}

int
libusb_set_configuration (libusb_device_handle *handle, int configuration)
{
  (void) handle;
  (void) configuration;
  return 0;
}

int
libusb_claim_interface (libusb_device_handle *handle, int iface)
{
  (void) handle;
  (void) iface;
  return 0;
}

int
libusb_release_interface (libusb_device_handle *handle, int iface)
{
  (void) handle;
  (void) iface;
  return 0;
}

int
libusb_set_interface_alt_setting (libusb_device_handle *handle,
                                  int iface, int alternate_setting)
{
  (void) handle;
  (void) iface;
  (void) alternate_setting;
  return 0;
}

int
libusb_kernel_driver_active (libusb_device_handle *handle, int iface)
{
  (void) handle;
  (void) iface;
  return 0;
}

int
libusb_detach_kernel_driver (libusb_device_handle *handle, int iface)
{
  (void) handle;
  (void) iface;
  return 0;
}

int
libusb_attach_kernel_driver (libusb_device_handle *handle, int iface)
{
  (void) handle;
  (void) iface;
  return 0;
}

int
libusb_reset_device (libusb_device_handle *handle)
{
  (void) handle;
  pthread_mutex_lock (&g_lock);
  mocsim_reset (sim ());
  pthread_mutex_unlock (&g_lock);
  return 0;
}

int
libusb_get_string_descriptor_ascii (libusb_device_handle *handle,
                                    uint8_t desc_index,
                                    unsigned char *data, int length)
{
  const char *s;

  (void) handle;

  switch (desc_index)
    {
    case 1:  s = "Petaic";                    break;
    case 2:  s = "Touch Fingerprint Sensor";   break;
    default: return LIBUSB_ERROR_NOT_FOUND;
    }

  if (length < (int) strlen (s))
    return LIBUSB_ERROR_OVERFLOW;

  memcpy (data, s, strlen (s));
  return (int) strlen (s);
}


/* ---- transfer plumbing ----------------------------------------------- */

static struct libusb_transfer *g_pending[64];
static int                    g_npending;

static void
delay_ms (void)
{
  if (g_speed_ms > 0)
    {
      struct timespec ts;

      ts.tv_sec = g_speed_ms / 1000;
      ts.tv_nsec = (long) (g_speed_ms % 1000) * 1000000L;
      nanosleep (&ts, NULL);
    }
}

static void
log_cmd (const uint8_t *frame, int in)
{
  if (!g_log)
    return;

  fprintf (stderr, "[mocsim] %s cmd=0x%02X ev=0x%02X len=%d\n",
           in ? "IN " : "OUT", frame[2], frame[4],
           (int) frame[0] | ((int) frame[1] << 8));
}

/* Perform one transfer against the device model. */
static void
run_transfer (struct libusb_transfer *t)
{
  t->actual_length = 0;

  switch (t->type)
    {
    case LIBUSB_TRANSFER_TYPE_BULK:
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
      if ((t->endpoint & 0x80) != 0)
        {
          int n = mocsim_in (sim (), t->buffer, t->length);

          if (n < 0)
            {
              t->status = LIBUSB_TRANSFER_TIMED_OUT;
            }
          else
            {
              t->status        = LIBUSB_TRANSFER_COMPLETED;
              t->actual_length = n;
              if (n == MOCSIM_FRAME_SIZE)
                log_cmd (t->buffer, 1);
            }
        }
      else
        {
          int rc;

          if (t->length == MOCSIM_IMG_BLOCK)
            rc = mocsim_out_block (sim (), t->buffer, t->length);
          else
            rc = mocsim_out_frame (sim (), t->buffer, t->length);

          if (rc < 0)
            {
              t->status = LIBUSB_TRANSFER_TIMED_OUT;
            }
          else
            {
              t->status        = LIBUSB_TRANSFER_COMPLETED;
              t->actual_length = t->length;
              if (t->length == MOCSIM_FRAME_SIZE)
                log_cmd (t->buffer, 0);
            }
        }
      break;

    case LIBUSB_TRANSFER_TYPE_CONTROL:
      {
        const struct libusb_control_setup *setup =
          libusb_control_transfer_get_setup (t);
        uint8_t *data = libusb_control_transfer_get_data (t);
        int n;

        n = mocsim_control (sim (), setup->bmRequestType, setup->bRequest,
                            setup->wValue, setup->wIndex, data,
                            setup->wLength);
        if (n < 0)
          {
            t->status = LIBUSB_TRANSFER_STALL;
          }
        else
          {
            t->status        = LIBUSB_TRANSFER_COMPLETED;
            t->actual_length = LIBUSB_CONTROL_SETUP_SIZE + n;
          }
      }
      break;

    default:
      fprintf (stderr, "[mocsim] unsupported transfer type %d\n", t->type);
      t->status = LIBUSB_TRANSFER_ERROR;
      break;
    }

  delay_ms ();
}

struct libusb_transfer *
libusb_alloc_transfer (int iso_packets)
{
  size_t size = sizeof (struct libusb_transfer) +
                (size_t) iso_packets * sizeof (struct libusb_iso_packet_descriptor);

  return calloc (1, size);
}

void
libusb_free_transfer (struct libusb_transfer *t)
{
  free (t);
}

int
libusb_submit_transfer (struct libusb_transfer *t)
{
  pthread_mutex_lock (&g_lock);

  if (g_npending >= (int) (sizeof g_pending / sizeof g_pending[0]))
    {
      pthread_mutex_unlock (&g_lock);
      fprintf (stderr, "[mocsim] pending queue full\n");
      return LIBUSB_ERROR_NO_MEM;
    }

  t->status = LIBUSB_TRANSFER_COMPLETED;
  g_pending[g_npending++] = t;

  pthread_mutex_unlock (&g_lock);
  return 0;
}

int
libusb_cancel_transfer (struct libusb_transfer *t)
{
  int i;

  pthread_mutex_lock (&g_lock);

  for (i = 0; i < g_npending; i++)
    {
      if (g_pending[i] == t)
        {
          /* Complete it with CANCELLED on the next event pump. */
          t->status = LIBUSB_TRANSFER_CANCELLED;
          pthread_mutex_unlock (&g_lock);
          return 0;
        }
    }

  pthread_mutex_unlock (&g_lock);
  return LIBUSB_ERROR_NOT_FOUND;
}

/*
 * Event pump.
 *
 * A transfer is moved from the pending queue to the ready queue when it is
 * serviced, and its callback runs on the *next* pump call. Calling the
 * libgusb callback from inside the same dispatch that completed the transfer
 * makes libgusb retire the GTask while libfprint is still inside its own
 * completion path, which intermittently trips "task already returned" and
 * crashes. Separating the two steps mirrors real libusb, where completion and
 * callback are never in the same stack frame as the submit.
 */
static struct libusb_transfer *g_ready[64];
static int                    g_nready;

static int
pump (void)
{
  struct libusb_transfer *t;
  int i;

  pthread_mutex_lock (&g_lock);

  /* 1) deliver one previously completed transfer (callback runs unlocked) */
  if (g_nready > 0)
    {
      t = g_ready[0];
      for (i = 1; i < g_nready; i++)
        g_ready[i - 1] = g_ready[i];
      g_nready--;
      pthread_mutex_unlock (&g_lock);

      if (t->callback != NULL)
        t->callback (t);

      return 1;
    }

  /* 2) otherwise service one pending transfer while holding the lock */
  if (g_npending == 0)
    {
      pthread_mutex_unlock (&g_lock);
      return 0;
    }

  t = g_pending[0];
  for (i = 1; i < g_npending; i++)
    g_pending[i - 1] = g_pending[i];
  g_npending--;

  if (t->status != LIBUSB_TRANSFER_CANCELLED)
    run_transfer (t);
  else
    t->actual_length = 0;

  g_ready[g_nready++] = t;
  pthread_mutex_unlock (&g_lock);

  return 1;
}

int
libusb_handle_events_timeout_completed (libusb_context *ctx,
                                        struct timeval *tv,
                                        int *completed)
{
  int n;

  (void) ctx;
  (void) tv;

  n = pump ();
  if (completed != NULL && n > 0)
    (*completed)++;

  return 0;
}

int
libusb_handle_events_timeout (libusb_context *ctx, struct timeval *tv)
{
  return libusb_handle_events_timeout_completed (ctx, tv, NULL);
}

int
libusb_handle_events (libusb_context *ctx)
{
  return libusb_handle_events_timeout_completed (ctx, NULL, NULL);
}

int
libusb_handle_events_completed (libusb_context *ctx, int *completed)
{
  return libusb_handle_events_timeout_completed (ctx, NULL, completed);
}

int
libusb_handle_events_locked (libusb_context *ctx, struct timeval *tv)
{
  (void) ctx;
  (void) tv;
  return pump () >= 0 ? 0 : LIBUSB_ERROR_OTHER;
}

/* ---- libusb: synchronous API (used by moc5159-probe) ------------------ */

int
libusb_bulk_transfer (libusb_device_handle *handle, unsigned char endpoint,
                      unsigned char *data, int length,
                      int *transferred, unsigned int timeout)
{
  int n;

  (void) handle;
  (void) timeout;

  if (transferred != NULL)
    *transferred = 0;

  pthread_mutex_lock (&g_lock);

  if ((endpoint & 0x80) != 0)
    {
      n = mocsim_in (sim (), data, length);
      if (n < 0)
        {
          pthread_mutex_unlock (&g_lock);
          return LIBUSB_ERROR_TIMEOUT;
        }
      if (transferred != NULL)
        *transferred = n;
      if (n == MOCSIM_FRAME_SIZE)
        log_cmd (data, 1);
      pthread_mutex_unlock (&g_lock);
      return 0;
    }

  if (length == MOCSIM_IMG_BLOCK)
    n = mocsim_out_block (sim (), data, length);
  else
    n = mocsim_out_frame (sim (), data, length);

  if (n < 0)
    {
      pthread_mutex_unlock (&g_lock);
      return LIBUSB_ERROR_TIMEOUT;
    }

  if (transferred != NULL)
    *transferred = length;
  if (length == MOCSIM_FRAME_SIZE)
    log_cmd (data, 0);

  pthread_mutex_unlock (&g_lock);
  return 0;
}

int
libusb_control_transfer (libusb_device_handle *handle, uint8_t request_type,
                         uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                         unsigned char *data, uint16_t wLength,
                         unsigned int timeout)
{
  int n;

  (void) handle;
  (void) timeout;

  pthread_mutex_lock (&g_lock);
  n = mocsim_control (sim (), request_type, bRequest, wValue, wIndex,
                      data, wLength);
  pthread_mutex_unlock (&g_lock);
  if (n < 0)
    return LIBUSB_ERROR_PIPE;

  return n;
}

int
libusb_interrupt_transfer (libusb_device_handle *handle, unsigned char endpoint,
                           unsigned char *data, int length,
                           int *transferred, unsigned int timeout)
{
  return libusb_bulk_transfer (handle, endpoint, data, length, transferred,
                               timeout);
}
