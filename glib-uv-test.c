/* Unit tests for GLib-Uv
 * Copyright (C) 2013 Mikhail Zabaluev
 *
 * Based on unit tests for GMainLoop
 * Copyright (C) 2011 Red Hat, Inc
 * Author: Matthias Clasen
 *
 * This work is provided "as is"; redistribution and modification
 * in whole or in part, in any medium, physical or electronic is
 * permitted without restriction.
 *
 * This work is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * In no event shall the authors or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 */

#define G_LOG_DOMAIN "GLib-Uv-Test"

#include <glib-uv.h>

static gboolean prepare (GSource *source, gint *time)
{
  return FALSE;
}
static gboolean check (GSource *source)
{
  return FALSE;
}
static gboolean dispatch (GSource *source, GSourceFunc cb, gpointer date)
{
  return FALSE;
}

GSourceFuncs funcs = {
  prepare,
  check,
  dispatch,
  NULL
};

static void
test_uvcontext_basic (void)
{
  uv_loop_t *loop;
  GMainContext *ctx;

  loop = uv_loop_new ();
  ctx = guv_main_context_new (loop);

  g_assert (!g_main_context_pending (ctx));
  g_assert (!g_main_context_iteration (ctx, FALSE));

  g_main_context_unref (ctx);

  uv_run (loop, UV_RUN_DEFAULT);
  uv_loop_delete (loop);
}

static gint a;
static gint b;
static gint c;

static gboolean
count_calls (gpointer data)
{
  gint *i = data;

  (*i)++;

  return TRUE;
}

static gboolean
quit_loop (gpointer data)
{
  uv_stop ((uv_loop_t *) data);
  return FALSE;
}

static void
test_timeouts (void)
{
  uv_loop_t *loop;
  GMainContext *ctx;
  GSource *source;

  a = b = c = 0;

  loop = uv_loop_new ();
  ctx = guv_main_context_new (loop);

  source = g_timeout_source_new (100);
  g_source_set_callback (source, count_calls, &a, NULL);
  g_source_attach (source, ctx);
  g_source_unref (source);

  source = g_timeout_source_new (250);
  g_source_set_callback (source, count_calls, &b, NULL);
  g_source_attach (source, ctx);
  g_source_unref (source);

  source = g_timeout_source_new (330);
  g_source_set_callback (source, count_calls, &c, NULL);
  g_source_attach (source, ctx);
  g_source_unref (source);

  source = g_timeout_source_new (1050);
  g_source_set_callback (source, quit_loop, loop, NULL);
  g_source_attach (source, ctx);
  g_source_unref (source);

  uv_run (loop, UV_RUN_DEFAULT);

  /* We may be delayed for an arbitrary amount of time - for example,
   * it's possible for all timeouts to fire exactly once.
   */
  g_assert_cmpint (a, >, 0);
  g_assert_cmpint (a, >=, b);
  g_assert_cmpint (b, >=, c);

  g_assert_cmpint (a, <=, 10);
  g_assert_cmpint (b, <=, 4);
  g_assert_cmpint (c, <=, 3);

  g_main_context_unref (ctx);
  /* Run the loop again to complete closing of the handles */
  uv_run (loop, UV_RUN_DEFAULT);
  uv_loop_delete (loop);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/basic", test_uvcontext_basic);
  g_test_add_func ("/timeouts", test_timeouts);

  return g_test_run ();
}
