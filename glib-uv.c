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

typedef enum
{
  GUV_LIVE_ASYNC   = 1 << 0,
  GUV_LIVE_PREPARE = 1 << 1,
  GUV_LIVE_CHECK   = 1 << 2,
  GUV_LIVE_ALL     = (1 << 3) - 1
} GUvLifeFlags;

struct _GUvLoopBackend {
  GMainContext *context;
  uv_loop_t    *loop;
  GPollFD      *fds;
  gsize         fds_size;
  gsize         fds_ready;
  gint          max_priority;
  GHashTable   *poll_records;           /* map<fd, GUvPollData*> */
  GHashTable   *pending_poll_records;   /* map<fd, GUvPollData*> */
  GHashTable   *pending_poll_updates;   /* map<fd, guint> */
  GSList       *remove_list;
  GThread      *thread;
  GMutex        mutex;
  uv_async_t    async;
  uv_prepare_t  prepare;
  uv_check_t    check;
  guint         life_state;
  gboolean      async_termination;
  gboolean      sources_ready;
  gboolean      glib_iteration;
};

struct _GUvPollData {
  uv_poll_t       poll;
  gint            fd;
  gushort         events;
  GUvLoopBackend *backend;
};

static gpointer guv_context_create      (gpointer user_data);
static void     guv_context_set_context (gpointer backend_data,
                                         GMainContext *context);
static void     guv_context_free        (gpointer backend_data);
static gboolean guv_context_acquire     (gpointer backend_data);
static gboolean guv_context_iterate     (gpointer backend_data,
                                         gboolean block);
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
guv_main_context_new (uv_loop_t *loop)
{
  return g_main_context_new_custom (&guv_main_context_funcs, loop);
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
  pd->poll.data = pd;
  pd->fd = fd;
  pd->events = events;
  pd->backend = backend;

  return pd;
}

static void
guv_poll_closed (uv_handle_t *handle)
{
  /* The poll structure is a member of GUvPollData */
  g_slice_free (GUvPollData, handle->data);
}

static void
guv_poll_close (GUvPollData *pd)
{
  uv_close ((uv_handle_t *) &pd->poll, guv_poll_closed);
}

static void
guv_poll_free_pending (GUvPollData *pd)
{
  g_slice_free (GUvPollData, pd);
}

static void
guv_poll_cb (uv_poll_t* handle, int status, int events)
{
  GUvPollData *pd = handle->data;
  GUvLoopBackend *backend = pd->backend;
  GPollFD *pollfd;

  if (backend->fds_ready >= backend->fds_size)
    {
      gsize new_fds_size = (backend->fds_size == 0)? 2
                                                   : backend->fds_size * 2;
      gsize poll_size = g_hash_table_size (backend->poll_records);
      if (new_fds_size > poll_size)
        new_fds_size = poll_size;
      backend->fds_size = new_fds_size;
      backend->fds = g_renew (GPollFD, backend->fds, new_fds_size);
    }

  pollfd = &backend->fds[backend->fds_ready++];
  pollfd->fd = pd->fd;
  pollfd->events = pd->events;

  if (status == 0)
    pollfd->revents = guv_events_uv_to_glib (events);
  else
    pollfd->revents = G_IO_ERR;
}

static gboolean
guv_context_add_poll (GUvLoopBackend *backend, GUvPollData *pd)
{
  int status;

  status = uv_poll_init (backend->loop, &pd->poll, pd->fd);

  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR ("uv_poll_init failed", backend->loop);
      g_slice_free (GUvPollData, pd);
      return FALSE;
    }

  /* FIXME: libuv can't poll for hangup */

  status = uv_poll_start (&pd->poll, guv_events_glib_to_uv (pd->events),
      guv_poll_cb);

  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR ("uv_poll_start failed", backend->loop);
      guv_poll_close (pd);
      return FALSE;
    }

  g_hash_table_replace (backend->poll_records, GINT_TO_POINTER (pd->fd), pd);

  return TRUE;
}

static void
guv_pending_poll_record_walk (gpointer key, gpointer value, gpointer user_data)
{
  GUvLoopBackend *backend = user_data;
  GUvPollData *pd = value;
  guv_context_add_poll (backend, pd);
}

static void
guv_pending_poll_update_walk (gpointer key, gpointer value, gpointer user_data)
{
  GUvLoopBackend *backend = user_data;
  GUvPollData *pd;
  int status;

  pd = g_hash_table_lookup (backend->poll_records, key);

  g_return_if_fail (pd != NULL);

  pd->events = GPOINTER_TO_UINT (value);

  status = uv_poll_start (&pd->poll, guv_events_glib_to_uv (pd->events),
      guv_poll_cb);

  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR ("uv_poll_start failed to update the event mask", backend->loop);
    }
}

static void
guv_prepare_cb (uv_prepare_t* handle, int status)
{
  GUvLoopBackend *backend = handle->data;
  GUvPollData *pd;
  GSList *item;

  g_return_if_fail (status == 0);

  g_mutex_lock (&backend->mutex);

  for (item = backend->remove_list; item != NULL; item = item->next)
    {
      pd = g_hash_table_lookup (backend->poll_records, item->data);
      if (G_UNLIKELY (pd == NULL))
        {
          g_critical ("file descriptor %d is not found in the poll, can't remove", GPOINTER_TO_INT (item->data));
          continue;
        }
      g_hash_table_remove (backend->poll_records, item->data);
    }
  g_slist_free (backend->remove_list);
  backend->remove_list = NULL;

  g_hash_table_foreach (backend->pending_poll_records,
      guv_pending_poll_record_walk, backend);
  g_hash_table_steal_all (backend->pending_poll_records);

  g_hash_table_foreach (backend->pending_poll_updates,
      guv_pending_poll_update_walk, backend);
  g_hash_table_remove_all (backend->pending_poll_updates);

  g_mutex_unlock (&backend->mutex);

  g_main_context_prepare (backend->context, &backend->max_priority);
}

static void
guv_check_cb (uv_check_t* handle, int status)
{
  GUvLoopBackend *backend = handle->data;
  gboolean sources_ready;

  g_return_if_fail (status == 0);

  sources_ready = g_main_context_check (backend->context, backend->max_priority,
      backend->fds, backend->fds_ready);

  backend->fds_ready = 0;

  if (sources_ready)
    {
      if (backend->glib_iteration)
        backend->sources_ready = TRUE;
      else
        g_main_context_dispatch (backend->context);
    }
}

static void
guv_async_cb (uv_async_t* handle, int status)
{
  GUvLoopBackend *backend = handle->data;

  if (backend->async_termination)
    guv_context_free (backend);
}

static inline gboolean
guv_context_gc(GUvLoopBackend *backend)
{
  if (backend->life_state == 0)
    {
      g_slice_free (GUvLoopBackend, backend);
      return TRUE;
    }
  return FALSE;
}

static void
guv_context_async_closed (uv_handle_t *handle)
{
  GUvLoopBackend *backend = handle->data;

  backend->life_state &= ~GUV_LIVE_ASYNC;

  guv_context_gc (backend);
}

static void
guv_context_prepare_closed (uv_handle_t *handle)
{
  GUvLoopBackend *backend = handle->data;

  backend->life_state &= ~GUV_LIVE_PREPARE;

  guv_context_gc (backend);
}

static void
guv_context_check_closed (uv_handle_t *handle)
{
  GUvLoopBackend *backend = handle->data;

  backend->life_state &= ~GUV_LIVE_CHECK;

  guv_context_gc (backend);
}

static gpointer guv_context_create (gpointer user_data)
{
  GUvLoopBackend *backend;
  int status;

  backend = g_slice_new0 (GUvLoopBackend);

  backend->loop = user_data;

  status = uv_async_init (backend->loop, &backend->async, guv_async_cb);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_async_init failed", backend->loop);
      goto uv_init_cleanup;
    }
  backend->async.data = backend;
  backend->life_state |= GUV_LIVE_ASYNC;

  status = uv_prepare_init (backend->loop, &backend->prepare);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_prepare_init failed", backend->loop);
      goto uv_init_cleanup;
    }
  backend->prepare.data = backend;
  backend->life_state |= GUV_LIVE_PREPARE;

  status = uv_check_init (backend->loop, &backend->check);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_check_init failed", backend->loop);
      goto uv_init_cleanup;
    }
  backend->check.data = backend;
  backend->life_state |= GUV_LIVE_CHECK;

  status = uv_prepare_start (&backend->prepare, guv_prepare_cb);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_prepare_start failed", backend->loop);
      goto uv_init_cleanup;
    }

  status = uv_check_start (&backend->check, guv_check_cb);
  if (G_UNLIKELY (status != 0))
    {
      WARN_UV_LAST_ERROR("uv_check_start failed", backend->loop);
      goto uv_init_cleanup;
    }

  g_mutex_init (&backend->mutex);

  backend->thread = g_thread_ref (g_thread_self ());

  backend->poll_records = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) guv_poll_close);

  backend->pending_poll_records = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) guv_poll_free_pending);

  backend->pending_poll_updates = g_hash_table_new (g_direct_hash, g_direct_equal);

  return backend;

uv_init_cleanup:

  if (!guv_context_gc (backend))
    {
      if ((backend->life_state & GUV_LIVE_ASYNC) != 0)
        uv_close ((uv_handle_t *) &backend->async, guv_context_async_closed);
      if ((backend->life_state & GUV_LIVE_PREPARE) != 0)
        uv_close ((uv_handle_t *) &backend->prepare, guv_context_prepare_closed);
      if ((backend->life_state & GUV_LIVE_CHECK) != 0)
        uv_close ((uv_handle_t *) &backend->check, guv_context_check_closed);
    }

  return NULL;
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

  g_assert (backend->life_state == GUV_LIVE_ALL);

  if (backend->thread != g_thread_self ())
    {
      g_return_if_fail (!backend->async_termination);

      backend->async_termination = TRUE;

      uv_async_send (&backend->async);

      return;
    }

  backend->async_termination = FALSE;

  uv_close ((uv_handle_t *) &backend->async, guv_context_async_closed);
  uv_close ((uv_handle_t *) &backend->prepare, guv_context_prepare_closed);
  uv_close ((uv_handle_t *) &backend->check, guv_context_check_closed);

  g_slist_free (backend->remove_list);

  g_hash_table_destroy (backend->poll_records);

  g_hash_table_destroy (backend->pending_poll_records);

  g_hash_table_destroy (backend->pending_poll_updates);

  g_free (backend->fds);

  g_mutex_clear (&backend->mutex);

  g_thread_unref (backend->thread);
}

static gboolean
guv_context_acquire (gpointer backend_data)
{
  GUvLoopBackend *backend = backend_data;

  return backend->thread == g_thread_self ();
}

static gboolean
guv_context_iterate (gpointer backend_data,
                     gboolean block)
{
  GUvLoopBackend *backend = backend_data;

  backend->sources_ready = FALSE;
  backend->glib_iteration = TRUE;

  uv_run (backend->loop, block? UV_RUN_ONCE : UV_RUN_NOWAIT);

  backend->glib_iteration = FALSE;

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

  if (backend->thread == g_thread_self ())
    {
      return guv_context_add_poll (backend, pd);
    }
  else
    {
      g_mutex_lock (&backend->mutex);

      g_hash_table_insert (backend->pending_poll_records, GINT_TO_POINTER (fd), pd);

      g_mutex_unlock (&backend->mutex);

      uv_async_send (&backend->async);

      return TRUE;
    }
}

static gboolean
guv_context_remove_fd (gpointer backend_data,
                       gint     fd)
{
  GUvLoopBackend *backend = backend_data;
  GUvPollData *pd;
  int status;

  if (backend->thread == g_thread_self ())
    {
      pd = g_hash_table_lookup (backend->poll_records, GINT_TO_POINTER (fd));
      g_return_val_if_fail (pd != NULL, FALSE);

      g_hash_table_steal (backend->poll_records, GINT_TO_POINTER (fd));

      status = uv_poll_stop (&pd->poll);

      if (G_UNLIKELY (status != 0))
        {
          WARN_UV_LAST_ERROR ("uv_poll_stop failed", backend->loop);
          guv_poll_close (pd);
          return FALSE;
        }

      guv_poll_close (pd);
    }
  else
    {
      g_mutex_lock (&backend->mutex);

      pd = g_hash_table_lookup (backend->pending_poll_records,
          GINT_TO_POINTER (fd));

      if (pd == NULL)
        {
          backend->remove_list = g_slist_prepend (backend->remove_list,
              GINT_TO_POINTER (fd));

          g_mutex_unlock (&backend->mutex);

          uv_async_send (&backend->async);
        }
      else
        {
          g_hash_table_remove (backend->pending_poll_records,
              GINT_TO_POINTER (fd));

          g_mutex_unlock (&backend->mutex);

          /* Stole pending work from the owner thread, no wakeup needed */
        }
    }

  return TRUE;
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

  if (backend->thread == g_thread_self ())
    {
      pd = g_hash_table_lookup (backend->poll_records, GINT_TO_POINTER (fd));

      if (pd != NULL)
        {
          status = uv_poll_start (&pd->poll, guv_events_glib_to_uv (pd->events),
              guv_poll_cb);

          if (G_UNLIKELY (status != 0))
            {
              WARN_UV_LAST_ERROR ("uv_poll_start failed", backend->loop);
              return FALSE;
            }
        }
      else
        {
          pd = g_hash_table_lookup (backend->pending_poll_records,
                  GINT_TO_POINTER (fd));
        }

      g_return_val_if_fail (pd != NULL, FALSE);

      pd->events = events;
    }
  else
    {
      g_mutex_lock (&backend->mutex);

      g_hash_table_replace (backend->pending_poll_updates,
                            GINT_TO_POINTER (fd), GUINT_TO_POINTER(events));

      g_mutex_unlock (&backend->mutex);

      uv_async_send (&backend->async);
    }

  return TRUE;
}
