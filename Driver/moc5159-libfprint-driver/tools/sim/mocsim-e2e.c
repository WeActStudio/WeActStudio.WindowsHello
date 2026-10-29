/*
 * mocsim-e2e.c — end-to-end test of the driver against the simulated module
 *
 * Exercises the whole libfprint path: context → device discovery → open →
 * list → enroll → verify → identify → delete. Nothing here knows about USB;
 * with LD_PRELOAD=libmocsim.so the transfers land in the simulator, which is
 * exactly the code path a real module would take.
 *
 *   ./build.sh --with-e2e
 *   LD_PRELOAD=./build/libmocsim.so ./build/mocsim-e2e
 *   LD_PRELOAD=./build/libmocsim.so MOCSIM_MATCH=0 ./build/mocsim-e2e
 *
 * Exit code 0 means every step behaved as expected for the configured scenario.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <glib.h>
#include <fprint.h>

static int g_pass;
static int g_fail;

static void
ok (const char *fmt, ...)
{
  va_list ap;

  g_pass++;
  va_start (ap, fmt);
  printf ("  ok   ");
  vprintf (fmt, ap);
  printf ("\n");
  va_end (ap);
}

static void
bad (const char *fmt, ...)
{
  va_list ap;

  g_fail++;
  va_start (ap, fmt);
  printf ("  FAIL ");
  vprintf (fmt, ap);
  printf ("\n");
  va_end (ap);
}

static void
on_progress (FpDevice *device, int completed, FpPrint *print,
             gpointer user_data, GError *error)
{
  (void) device;
  (void) print;
  (void) user_data;

  if (error != NULL)
    printf ("       stage %d: %s\n", completed, error->message);
  else
    printf ("       stage %d ok\n", completed);
}

int
main (void)
{
  const char *want;
  gboolean expect_match;
  g_autoptr(GError) error = NULL;
  FpContext *ctx;
  GPtrArray *devices;
  FpDevice *dev = NULL;
  GPtrArray *prints;
  FpPrint *print = NULL;
  FpPrint *enrolled = NULL;
  gboolean match = FALSE;

  setvbuf (stdout, NULL, _IONBF, 0);

  want = getenv ("MOCSIM_MATCH");
  expect_match = (want == NULL || *want != '0');

  printf ("== mocsim end-to-end (expect %s) ==\n",
          expect_match ? "MATCH" : "NO MATCH");

  ctx = fp_context_new ();
  fp_context_enumerate (ctx);
  devices = fp_context_get_devices (ctx);

  if (devices == NULL || devices->len == 0)
    {
      bad ("no device found — is libmocsim.so preloaded?");
      return 1;
    }

  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *d = g_ptr_array_index (devices, i);

      printf ("  device: %s\n", fp_device_get_name (d));
      if (g_strcmp0 (fp_device_get_driver (d), "moc5159") == 0)
        dev = d;
    }

  if (dev == NULL)
    {
      bad ("moc5159 driver not among the discovered devices");
      return 1;
    }

  ok ("driver discovered through libfprint");

  if (!fp_device_open_sync (dev, NULL, &error))
    {
      bad ("open failed: %s", error->message);
      return 1;
    }

  ok ("device open (SETUP + DB_OPEN)");

  prints = fp_device_list_prints_sync (dev, NULL, &error);
  if (prints == NULL)
    {
      bad ("list failed: %s", error->message);
      fp_device_close_sync (dev, NULL, NULL);
      return 1;
    }

  printf ("  info %u stored print(s) on the chip\n", prints->len);

  if (prints->len > 0)
    print = g_object_ref (g_ptr_array_index (prints, 0));

  /* ---- enroll when the chip database is empty ---------------------- */
  if (print == NULL)
    {
      FpPrint *tmpl = fp_print_new (dev);

      fp_print_set_finger (tmpl, FP_FINGER_RIGHT_INDEX);
      fp_print_set_username (tmpl, g_get_user_name ());

      /* fp_device_enroll_sync takes ownership of the template print. */
      enrolled = fp_device_enroll_sync (dev, tmpl, NULL, on_progress, NULL, &error);

      if (enrolled == NULL)
        {
          bad ("enroll failed: %s", error->message);
          fp_device_close_sync (dev, NULL, NULL);
          return 1;
        }

      ok ("enroll completed (5 stages)");

      print = g_object_ref (enrolled);

      g_ptr_array_unref (prints);
      prints = fp_device_list_prints_sync (dev, NULL, &error);
      if (prints == NULL || prints->len != 1)
        bad ("expected exactly 1 template after enroll, got %u",
             prints ? prints->len : 0);
      else
        ok ("list shows the new template");
    }

  /* ---- verify ------------------------------------------------------ */
  if (!fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error))
    {
      bad ("verify failed: %s", error->message);
    }
  else if (match == expect_match)
    {
      ok ("verify returned %s as configured", match ? "MATCH" : "NO MATCH");
    }
  else
    {
      bad ("verify returned %s, expected %s", match ? "MATCH" : "NO MATCH",
           expect_match ? "MATCH" : "NO MATCH");
    }

  /* ---- identify ---------------------------------------------------- */
  {
    GPtrArray *gallery;
    FpPrint *found = NULL;

    /* identify compares against the caller's gallery, which is normally the
     * prints fprintd stored for the user. */
    gallery = g_ptr_array_new_with_free_func (g_object_unref);
    g_ptr_array_add (gallery, g_object_ref (print));

    if (!fp_device_identify_sync (dev, gallery, NULL, NULL, NULL, &found, NULL, &error))
      {
        bad ("identify failed: %s", error->message);
      }
    else if (expect_match)
      {
        if (found != NULL)
          ok ("identify matched a gallery entry");
        else
          bad ("identify reported no match while MOCSIM_MATCH=1");
      }
    else
      {
        /* The driver reports the first gallery entry when the chip says
         * "matched" but cannot name the template; with no match it must
         * return nothing. */
        if (found == NULL)
          ok ("identify reported no match");
        else
          bad ("identify returned a print while MOCSIM_MATCH=0");
      }

    g_ptr_array_unref (gallery);
  }

  /* ---- delete ------------------------------------------------------ */
  {
    guint before = prints->len;

    if (!fp_device_delete_print_sync (dev, print, NULL, &error))
      {
        bad ("delete failed: %s", error->message);
      }
    else
      {
        g_ptr_array_unref (prints);
        prints = fp_device_list_prints_sync (dev, NULL, &error);

        if (prints != NULL && prints->len + 1 == before)
          ok ("delete removed one template (%u → %u)", before, prints->len);
        else
          bad ("expected %u template(s) after delete, got %u",
               before - 1, prints ? prints->len : 0);
      }
  }

  fp_device_close_sync (dev, NULL, NULL);

  printf ("\n%d passed, %d failed\n", g_pass, g_fail);

  if (prints != NULL)
    g_ptr_array_unref (prints);
  g_clear_object (&print);
  g_clear_object (&enrolled);

  /* Deliberately not unreffing the FpContext: tearing down a GUsbContext
   * whose transfers are answered by an LD_PRELOAD shim blocks in the event
   * thread join. The process is finished either way, so exit directly. */
  fflush (NULL);
  _exit (g_fail == 0 ? 0 : 1);
}
