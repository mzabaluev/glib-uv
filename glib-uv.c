/* GLib-Uv - Integration of event loops between GLib and libuv
 *
 * Copyright (C) 2013  Mikhail Zabaluev
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#define G_LOG_DOMAIN "GLib-Uv"

#include "glib-uv.h"

#define WARN_UV_LAST_ERROR(message, loop) \
    g_warning (message ": %s", uv_strerror (uv_last_error (loop)))

typedef struct _GUvLoopBackend GUvLoopBackend;
typedef struct _GUvPollData GUvPollData;

struct _GUvLoopBackend {
  GMainContext *context;
  uv_loop_t    *loop;
  GPollFD      *fds;
  guint         fds_size;
  guint         fds_ready;
  gint          max_priority;
  GHashTable   *poll_records;           /* map<fd, GUvPollData*> */
  GThread      *thread;
  uv_timer_t   *timer;
  uv_prepare_t *prepare;
  uv_check_t   *check;
  guint         dispatch_sources  :1;
  guint         sources_ready     :1;
};

typedef enum
{
  GUV_POLL_IN_CONTEXT = 1,
  GUV_POLL_IN_LOOP    = 2,
} GUvPollGCFlags;

struct _GUvPollData {
  uv_poll_t       poll;
  gint            fd;
  gushort         events;
  gushort         gc_flags;
};

#define GUV_POLL_DATA(uvp) ((GUvPollData *) ((char *) (uvp) - G_STRUCT_OFFSET (GUvPollData, poll)))

static gpointer guv_context_create      (gpointer user_data);
static void     guv_context_set_context (gpointer backend_data,
                                         GMainContext *context);
static void     guv_context_free        (gpointer backend_data);
static gboolean guv_context_acquire     (gpointer backend_data);
static gboolean guv_context_iterate     (gpointer backend_data,
                                         gboolean block,
                                         gboolean dispatch);
static gboolean guv_context_add_fd      (gpointer backend_data,
                                         gint     fd,
                                         gushort  events,
                                         gint     priority);
static gboolean guv_context_modify_fd   (gpointer backend_data,
                                         gint     fd,
                                         gushort  events,
                                         gint     priority);
static gboolean guv_context_remove_fd   (gpointer backend_data,
                                         gint     fd);

static const GMainContextFuncs guv_main_context_funcs =
{
  guv_context_create,
  guv_context_set_context,
  guv_context_free,
  guv_context_acquire,
  NULL, /* release */
  guv_context_iterate,
  guv_context_add_fd,
  guv_context_modify_fd,
  guv_context_remove_fd,
};

GMainContext *
guv_main_context_new (void)
{
  return g_main_context_new_custom (&guv_main_context_funcs, NULL);
}

static inline guint
guv_direct_event_mask ()
{
  guint direct_mask = 0;

  if ((guint) G_IO_IN == (guint) UV_READABLE)
    direct_mask |= G_IO_IN;
  if ((guint) G_IO_OUT == (guint) UV_WRITABLE)
    direct_mask |= G_IO_OUT;

  return direct_mask;
}

static int
guv_events_glib_to_uv (guint events)
{
  int uv_events;

  uv_events = events & guv_direct_event_mask ();
  if ((guint) G_IO_IN != (guint) UV_READABLE &&
      (events & G_IO_IN) != 0)
    uv_events |= UV_READABLE;
  if ((guint) G_IO_OUT != (guint) UV_WRITABLE &&
      (events & G_IO_OUT) != 0)
    uv_events |= UV_WRITABLE;
  return uv_events;
}

static guint
guv_events_uv_to_glib (int uv_events)
{
  guint events;

  events = uv_events & guv_direct_event_mask ();
  if ((guint) G_IO_IN != (guint) UV_READABLE &&
      (uv_events & UV_READABLE) != 0)
    events |= G_IO_IN;
  if ((guint) G_IO_OUT != (guint) UV_WRITABLE &&
      (uv_events & UV_WRITABLE) != 0)
    events |= G_IO_OUT;
  return events;
}

static GUvPollData *
guv_poll_new (int fd, gushort events, GUvLoopBackend *backend)
{
  GUvPollData *pd;

  pd = g_slice_new (GUvPollData);
  pd->poll.data = backend;
  pd->fd = fd;
  pd->events = events;
  pd->gc_flags = 0;

  return pd;
}

static void
guv_poll_remove_record (GUvPollData *pd)
{
  pd->gc_flags &= ~GUV_POLL_IN_CONTEXT;
  if (pd->gc_flags == 0)
    g_slice_free (GUvPollData, pd);
}

static void
guv_poll_closed (uv_handle_t *handle)
{
  GUvPollData *pd = GUV_POLL_DATA (handle);
  pd->gc_flags &= ~GUV_POLL_IN_LOOP;
  if (pd->gc_flags == 0)
    g_slice_free (GUvPollData, pd);
}

static void
guv_context_ensure_poll_array_size (GUvLoopBackend *backend, guint required)
{
  guint new_fds_size;
  guint poll_size;

  if (backend->fds_size < required)
    {
      new_fds_size = MAX(required, backend->fds_size * 2);
      poll_size = g_hash_table_size (backend->poll_records);
      if (new_fds_size > poll_size)
        new_fds_size = poll_size;
      backend->fds_size = new_fds_size;
      backend->fds = g_renew (GPollFD, backend->fds, new_fds_size);
    }
  g_assert (backend->fds_size >= required);
}

static void
guv_poll_cb (uv_poll_t* handle, int status, int events)
{
  GUvPollData *pd = GUV_POLL_DATA (handle);
  GUvLoopBackend *backend = handle->data;
  GPollFD *pollfd;

  guv_context_ensure_poll_array_size (backend, backend->fds_ready + 1);

  pollfd = &backend->fds[backend->fds_ready++];
  pollfd->fd = pd->fd;
  pollfd->events = pd->events;

  if (status == 0)
    pollfd->revents = guv_events_uv_to_glib (events);
  else
    pollfd->revents = G_IO_ERR;
}

static void
guv_poll_start (GUvPollData *pd, uv_loop_t *loop)
{
  int status;

  status = uv_poll_init (loop, &pd->poll, pd->fd);

  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR ("uv_poll_init failed", loop);
      return;
    }

  pd->gc_flags |= GUV_POLL_IN_LOOP;

  /* FIXME: libuv can't poll for hangup */

  status = uv_poll_start (&pd->poll, guv_events_glib_to_uv (pd->events),
      guv_poll_cb);

  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR ("uv_poll_start failed", loop);
      return;
    }
}

static void
guv_poll_stop (GUvPollData *pd, uv_loop_t *loop)
{
  int status;

  status = uv_poll_stop (&pd->poll);

  if (G_UNLIKELY (status != 0))
    WARN_UV_LAST_ERROR ("uv_poll_stop failed", loop);

  uv_close ((uv_handle_t *) &pd->poll, guv_poll_closed);
}

static void
guv_poll_update (GUvPollData *pd, gushort events, uv_loop_t *loop)
{
  int status;

  status = uv_poll_start (&pd->poll, guv_events_glib_to_uv (events),
      guv_poll_cb);

  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR ("uv_poll_start failed to update the event mask", loop);
      return;
    }

  pd->events = events;
}

static void
guv_timer_cb (uv_timer_t *timer, int status)
{
}

static void
guv_prepare_cb (uv_prepare_t* handle, int status)
{
  GUvLoopBackend *backend = handle->data;
  gint timeout;
  gboolean acquired;

  g_return_if_fail (status == 0);

  /* Temporarily hold another owner reference
   * in case guv_main_context_stop is called in a prepare callback.
   */
  acquired = g_main_context_acquire (backend->context);

  g_return_if_fail (acquired);

  g_main_context_prepare (backend->context, &backend->max_priority);

  if (G_LIKELY (backend->timer != NULL))
    {
      timeout = g_main_context_get_poll_timeout (backend->context);
      if (timeout >= 0)
        uv_timer_start (&backend->timer, guv_timer_cb, timeout, 0);
    }

  backend->fds_ready = 0;

  g_main_context_release (backend->context);
}

static void
guv_check_cb (uv_check_t* handle, int status)
{
  GUvLoopBackend *backend = handle->data;
  gboolean acquired;

  g_return_if_fail (status == 0);

  /* Temporarily hold another owner reference
   * in case guv_main_context_stop is called in a check or dispatch callback.
   */
  acquired = g_main_context_acquire (backend->context);

  g_return_if_fail (acquired);

  backend->sources_ready = g_main_context_check (backend->context,
      backend->max_priority, backend->fds, backend->fds_ready);

  if (backend->sources_ready && backend->dispatch_sources)
    g_main_context_dispatch (backend->context);

  g_main_context_release (backend->context);
}

static void
guv_timer_closed (uv_handle_t *handle)
{
  g_slice_free (uv_timer_t, handle);
}

static void
guv_prepare_closed (uv_handle_t *handle)
{
  g_slice_free (uv_prepare_t, handle);
}

static void
guv_check_closed (uv_handle_t *handle)
{
  g_slice_free (uv_check_t, handle);
}

static void
guv_poll_start_walk (gpointer key, gpointer value, gpointer user_data)
{
  GUvPollData *pd = value;
  uv_loop_t *loop = user_data;

  guv_poll_start (pd, loop);
}

static void
guv_poll_stop_walk (gpointer key, gpointer value, gpointer user_data)
{
  GUvPollData *pd = value;
  uv_loop_t *loop = user_data;

  guv_poll_stop (pd, loop);
}

gboolean
guv_main_context_start (GMainContext *context, uv_loop_t *loop)
{
  GUvLoopBackend *backend;
  uv_timer_t *timer;
  uv_prepare_t *prepare;
  uv_check_t *check;
  int status;

  g_return_val_if_fail (g_main_context_get_backend_funcs (context) == &guv_main_context_funcs, FALSE);

  if (!g_main_context_acquire (context))
    return FALSE;

  backend = g_main_context_get_backend_data (context);

  if (G_UNLIKELY (backend->loop != NULL))
    {
      g_warning ("guv_main_context_start called again without an interceding guv_main_context_stop");
      goto error_cleanup;
    }

  backend->loop = loop;

  timer = g_slice_new (uv_timer_t);
  status = uv_timer_init (loop, timer);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_timer_init failed", loop);
      g_slice_free (uv_timer_t, timer);
      goto error_cleanup;
    }
  timer->data = backend;
  backend->timer = timer;

  prepare = g_slice_new (uv_prepare_t);
  status = uv_prepare_init (loop, prepare);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_prepare_init failed", loop);
      g_slice_free (uv_prepare_t, prepare);
      goto error_cleanup;
    }
  prepare->data = backend;
  backend->prepare = prepare;

  check = g_slice_new (uv_check_t);
  status = uv_check_init (loop, check);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_check_init failed", loop);
      g_slice_free (uv_check_t, check);
      goto error_cleanup;
    }
  check->data = backend;
  backend->check = check;

  status = uv_prepare_start (backend->prepare, guv_prepare_cb);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_prepare_start failed", loop);
      goto error_cleanup;
    }

  status = uv_check_start (backend->check, guv_check_cb);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_check_start failed", loop);
      goto error_cleanup;
    }

  backend->thread = g_thread_ref (g_thread_self ());

  g_hash_table_foreach (backend->poll_records, guv_poll_start_walk, loop);

  return TRUE;

error_cleanup:

  if (backend->timer != NULL)
    {
      uv_close ((uv_handle_t *) backend->timer, guv_timer_closed);
      backend->timer = NULL;
    }
  if (backend->prepare != NULL)
    {
      uv_close ((uv_handle_t *) backend->prepare, guv_prepare_closed);
      backend->prepare = NULL;
    }
  if (backend->check != NULL)
    {
      uv_close ((uv_handle_t *) backend->check, guv_check_closed);
      backend->check = NULL;
    }
  backend->loop = NULL;

  g_main_context_release (context);

  return FALSE;
}

void guv_main_context_stop (GMainContext *context)
{
  GUvLoopBackend *backend;

  g_return_if_fail (g_main_context_get_backend_funcs (context) == &guv_main_context_funcs);

  backend = g_main_context_get_backend_data (context);

  g_return_if_fail (backend->loop != NULL);

  g_hash_table_foreach (backend->poll_records, guv_poll_stop_walk, backend->loop);

  uv_close ((uv_handle_t *) backend->timer, guv_timer_closed);
  backend->timer = NULL;
  uv_close ((uv_handle_t *) backend->prepare, guv_prepare_closed);
  backend->prepare = NULL;
  uv_close ((uv_handle_t *) backend->check, guv_check_closed);
  backend->check = NULL;

  backend->loop = NULL;

  g_thread_unref (backend->thread);
  backend->thread = NULL;

  g_main_context_release (context);
}

static gpointer guv_context_create (gpointer user_data)
{
  GUvLoopBackend *backend;
  int status;

  backend = g_slice_new0 (GUvLoopBackend);

  backend->poll_records = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) guv_poll_remove_record);

  backend->dispatch_sources = TRUE;

  return backend;
}

static void
guv_context_set_context (gpointer backend_data,
                         GMainContext *context)
{
  GUvLoopBackend *backend = backend_data;
  backend->context = context;
}

static void
guv_context_free (gpointer backend_data)
{
  GUvLoopBackend *backend = backend_data;

  if (backend->loop != NULL)
    {
      g_critical ("guv_main_context_stop is never called, handles leaked");
      g_thread_unref (backend->thread);
    }

  g_hash_table_destroy (backend->poll_records);

  g_free (backend->fds);

  g_slice_free (GUvLoopBackend, backend);
}

static gboolean
guv_context_acquire (gpointer backend_data)
{
  GUvLoopBackend *backend = backend_data;

  return backend->thread == NULL || backend->thread == g_thread_self ();
}

static gboolean
guv_context_iterate (gpointer backend_data,
                     gboolean block,
                     gboolean dispatch)
{
  GUvLoopBackend *backend = backend_data;

  if (G_UNLIKELY (backend->loop == NULL))
    {
      if (block)
        g_error ("can't block for events without guv_main_context_start");
      return FALSE;
    }

  backend->sources_ready = FALSE;
  backend->dispatch_sources = dispatch;

  uv_run (backend->loop, block? UV_RUN_ONCE : UV_RUN_NOWAIT);

  backend->dispatch_sources = TRUE;

  return backend->sources_ready;
}

static gboolean
guv_context_add_fd (gpointer backend_data,
                    gint     fd,
                    gushort  events,
                    gint     priority)
{
  GUvLoopBackend *backend = backend_data;
  GUvPollData *pd;

  pd = guv_poll_new (fd, events, backend);

  g_hash_table_replace (backend->poll_records, GINT_TO_POINTER (pd->fd), pd);

  pd->gc_flags |= GUV_POLL_IN_CONTEXT;

  if (backend->loop != NULL)
    guv_poll_start (pd, backend->loop);

  return TRUE;
}

static gboolean
guv_context_remove_fd (gpointer backend_data,
                       gint     fd)
{
  GUvLoopBackend *backend = backend_data;
  GUvPollData *pd;
  gint status;

  pd = g_hash_table_lookup (backend->poll_records, GINT_TO_POINTER (fd));
  g_return_val_if_fail (pd != NULL, FALSE);

  if (backend->loop != NULL)
    guv_poll_stop (pd, backend->loop);

  return g_hash_table_remove (backend->poll_records, GINT_TO_POINTER (fd));
}

static gboolean
guv_context_modify_fd   (gpointer backend_data,
                         gint     fd,
                         gushort  events,
                         gint     priority)
{
  GUvLoopBackend *backend = backend_data;
  GUvPollData *pd;
  int status;

  pd = g_hash_table_lookup (backend->poll_records, GINT_TO_POINTER (fd));

  g_return_val_if_fail (pd != NULL, FALSE);

  if (backend->loop != NULL)
    guv_poll_update (pd, events, backend->loop);

  return TRUE;
}
