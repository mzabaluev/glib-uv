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
test_mainloop_basic (void)
{
  uv_loop_t *uv_loop;
  GMainLoop *loop;
  GMainContext *ctx;

  uv_loop = uv_loop_new ();
  loop = guv_main_loop_new (NULL, uv_loop);

  g_assert (!g_main_loop_is_running (loop));

  ctx = g_main_loop_get_context (loop);
  g_assert (ctx == g_main_context_default ());

  g_assert_cmpint (g_main_depth (), ==, 0);

  g_assert (!g_main_context_pending (ctx));
  g_assert (!g_main_context_iteration (ctx, FALSE));

  g_main_loop_unref (loop);

  uv_run (uv_loop, UV_RUN_DEFAULT);

  uv_loop_delete (uv_loop);
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

static void
test_timeouts (void)
{
  uv_loop_t *uv_loop;
  GMainContext *ctx;
  GMainLoop *loop;
  GSource *source;
  gboolean loop_started;

  a = b = c = 0;

  ctx = g_main_context_new ();
  uv_loop = uv_loop_new ();
  loop = guv_main_loop_new (ctx, uv_loop);

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
  g_source_set_callback (source, (GSourceFunc)g_main_loop_quit, loop, NULL);
  g_source_attach (source, ctx);
  g_source_unref (source);

  loop_started = g_main_loop_start (loop);

  g_assert (loop_started);

  uv_run (uv_loop, UV_RUN_DEFAULT);

  /* We may be delayed for an arbitrary amount of time - for example,
   * it's possible for all timeouts to fire exactly once.
   */
  g_assert_cmpint (a, >, 0);
  g_assert_cmpint (a, >=, b);
  g_assert_cmpint (b, >=, c);

  g_assert_cmpint (a, <=, 10);
  g_assert_cmpint (b, <=, 4);
  g_assert_cmpint (c, <=, 3);

  g_main_loop_unref (loop);
  uv_loop_delete (uv_loop);
  g_main_context_unref (ctx);
}

static gboolean
quit_loop (gpointer data)
{
  GMainLoop *loop = data;

  g_main_loop_quit (loop);

  return G_SOURCE_REMOVE;
}

static gint count;

static gboolean
func (gpointer data)
{
  if (data != NULL)
    g_assert (data == g_thread_self ());

  count++;

  return FALSE;
}

static GMutex mutex;
static GCond cond;
static volatile gboolean thread_ready;

static gpointer
thread_func (gpointer data)
{
  GMainContext *ctx = data;
  uv_loop_t *uv_loop;
  GMainLoop *loop;
  GSource *source;
  gboolean loop_started;

  g_main_context_push_thread_default (ctx);

  uv_loop = uv_loop_new ();
  loop = guv_main_loop_new (ctx, uv_loop);

  g_mutex_lock (&mutex);
  thread_ready = TRUE;
  g_cond_signal (&cond);
  g_mutex_unlock (&mutex);

  source = g_timeout_source_new (500);
  g_source_set_callback (source, quit_loop, loop, NULL);
  g_source_attach (source, ctx);
  g_source_unref (source);

  loop_started = g_main_loop_start (loop);

  g_assert (loop_started);

  uv_run (uv_loop, UV_RUN_DEFAULT);

  g_main_loop_unref (loop);
  uv_loop_delete (uv_loop);

  g_main_context_pop_thread_default (ctx);

  return NULL;
}

static void
test_invoke (void)
{
  GMainContext *ctx;
  GThread *thread;

  ctx = g_main_context_new ();

  count = 0;

  /* test thread-default forcing the invocation to go
   * to another thread
   */
  thread = g_thread_new ("worker", thread_func, ctx);

  g_mutex_lock (&mutex);
  while (!thread_ready)
    g_cond_wait (&cond, &mutex);
  g_mutex_unlock (&mutex);

  g_main_context_invoke (ctx, func, thread);
  g_main_context_invoke (ctx, func, thread);

  g_thread_join (thread);
  g_assert_cmpint (count, ==, 2);

  g_main_context_unref (ctx);
}

static gboolean
quit_loop_and_unref (gpointer data)
{
  GMainLoop *loop = data;

  g_main_loop_quit (loop);
  g_main_loop_unref (loop);

  return G_SOURCE_REMOVE;
}

static void
test_unref_mainloop (void)
{
  uv_loop_t *uv_loop;
  GMainLoop *loop;
  gboolean loop_started;

  uv_loop = uv_loop_new ();
  loop = guv_main_loop_new (NULL, uv_loop);
  g_idle_add (quit_loop_and_unref, loop);

  loop_started = g_main_loop_start (loop);

  g_assert (loop_started);

  uv_run (uv_loop, UV_RUN_DEFAULT);
  uv_loop_delete (uv_loop);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/basic", test_mainloop_basic);
  g_test_add_func ("/timeouts", test_timeouts);
  g_test_add_func ("/invoke", test_invoke);
  g_test_add_func ("/unref-mainloop", test_unref_mainloop);

  return g_test_run ();
}
