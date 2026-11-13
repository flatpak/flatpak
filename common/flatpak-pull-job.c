/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include <sys/file.h>

#include "flatpak-pull-job-private.h"

struct _FlatpakPullJob
{
  FlatpakDir         *dir;
  FlatpakRemoteState *state;
  char               *ref;
  char               *commit;
  char               *token;
  GBytes             *metadata;
  char              **subpaths;
  GLnxLockFile        repo_lock;
  FlatpakProgress    *worker_progress;
  FlatpakProgress    *snapshot;
  FlatpakPullFlags    flags;
  GCancellable       *cancel;
  GCancellable       *parent_cancel;
  gulong              cancel_handler;
  GThread            *thread;
  GMutex              mutex;
  GCond               condition;
  GError             *error;
  gboolean            done;
};

static void
pull_job_progress (const char *status, guint percent, gboolean estimating, gpointer data)
{
  FlatpakPullJob *self = data;

  g_mutex_lock (&self->mutex);
  flatpak_progress_copy_state (self->snapshot, self->worker_progress);
  g_cond_signal (&self->condition);
  g_mutex_unlock (&self->mutex);
}

static gpointer
pull_job_run (gpointer data)
{
  FlatpakPullJob *self = data;

  flatpak_dir_pull (self->dir, self->state, self->ref,
                    self->commit, (const char **) self->subpaths,
                    NULL, NULL, self->metadata, self->token,
                    NULL, self->flags, OSTREE_REPO_PULL_FLAGS_NONE,
                    self->worker_progress, self->cancel, &self->error);
  g_mutex_lock (&self->mutex);
  flatpak_progress_copy_state (self->snapshot, self->worker_progress);
  self->done = TRUE;
  g_cond_signal (&self->condition);
  g_mutex_unlock (&self->mutex);
  g_debug ("Background pull completed for %s (%s)",
           self->ref, self->error ? "error" : "success");
  return NULL;
}

static void
pull_job_cancelled (GCancellable *parent, gpointer data)
{
  g_cancellable_cancel (data);
}

void
flatpak_pull_job_cancel (FlatpakPullJob *self)
{
  g_cancellable_cancel (self->cancel);
}

void
flatpak_pull_job_cancel_and_join (FlatpakPullJob *self)
{
  flatpak_pull_job_cancel (self);
  if (self->thread != NULL)
    {
      g_thread_join (self->thread);
      self->thread = NULL;
    }
}

void
flatpak_pull_job_free (FlatpakPullJob *self)
{
  flatpak_pull_job_cancel_and_join (self);
  glnx_release_lock_file (&self->repo_lock);
  if (self->parent_cancel != NULL)
    g_cancellable_disconnect (self->parent_cancel, self->cancel_handler);
  g_clear_object (&self->parent_cancel);
  g_object_unref (self->cancel);
  g_object_unref (self->dir);
  flatpak_remote_state_unref (self->state);
  g_free (self->ref);
  g_free (self->commit);
  g_free (self->token);
  g_clear_pointer (&self->metadata, g_bytes_unref);
  g_strfreev (self->subpaths);
  g_object_unref (self->worker_progress);
  g_object_unref (self->snapshot);
  g_clear_error (&self->error);
  g_mutex_clear (&self->mutex);
  g_cond_clear (&self->condition);
  g_free (self);
}

FlatpakPullJob *
flatpak_pull_job_new (FlatpakDir         *dir,
                      FlatpakRemoteState *state,
                      const char         *ref,
                      const char         *commit,
                      const char * const *subpaths,
                      GBytes             *metadata,
                      const char         *token,
                      FlatpakPullFlags    flags,
                      GCancellable       *cancellable)
{
  g_autoptr(FlatpakPullJob) self = NULL;
  self = g_new0 (FlatpakPullJob, 1);
  g_mutex_init (&self->mutex);
  g_cond_init (&self->condition);
  self->dir = flatpak_dir_clone (dir);
  self->state = flatpak_remote_state_snapshot_for_pull (state);
  self->ref = g_strdup (ref);
  self->commit = g_strdup (commit);
  self->token = g_strdup (token);
  self->metadata = metadata ? g_bytes_ref (metadata) : NULL;
  self->subpaths = g_strdupv ((char **) subpaths);
  self->flags = flags;
  self->cancel = g_cancellable_new ();
  self->worker_progress = flatpak_progress_new (pull_job_progress, self);
  self->snapshot = flatpak_progress_new (NULL, NULL);
  flatpak_progress_set_update_interval (self->worker_progress, 10);
  if (cancellable != NULL)
    {
      self->parent_cancel = g_object_ref (cancellable);
      self->cancel_handler = g_cancellable_connect (cancellable, G_CALLBACK (pull_job_cancelled), self->cancel, NULL);
    }

  /* Opening can rewrite config, so do it before deployment can write pins. */
  if (flatpak_dir_ensure_repo (self->dir, self->cancel, &self->error))
    {
      g_autofree char *expected_config = g_key_file_to_data (ostree_repo_get_config (flatpak_dir_get_repo (dir)), NULL, NULL);
      g_autofree char *worker_config = g_key_file_to_data (ostree_repo_get_config (flatpak_dir_get_repo (self->dir)), NULL, NULL);

      /* Tokens must only reach the original URL and verification policy.
       * Comparing repo configs does not cover parent configuration. */
      if (ostree_repo_get_parent (flatpak_dir_get_repo (dir)) != NULL ||
          ostree_repo_get_parent (flatpak_dir_get_repo (self->dir)) != NULL ||
          g_strcmp0 (expected_config, worker_config) != 0 ||
          flatpak_dir_get_remote_oci (self->dir, self->state->remote_name))
        return NULL;

      if (flatpak_dir_repo_lock (self->dir, &self->repo_lock, LOCK_SH, self->cancel, &self->error))
        self->thread = g_thread_try_new ("flatpak-pull", pull_job_run, self, &self->error);
    }
  if (self->thread == NULL)
    self->done = TRUE;
  return g_steal_pointer (&self);
}

const char **
flatpak_pull_job_get_subpaths (FlatpakPullJob *self)
{
  return (const char **) self->subpaths;
}

gboolean
flatpak_pull_job_snapshot (FlatpakPullJob *self, FlatpakProgress *progress)
{
  gboolean done;

  g_mutex_lock (&self->mutex);
  flatpak_progress_copy_state (progress, self->snapshot);
  done = self->done;
  g_mutex_unlock (&self->mutex);
  return done;
}

gboolean
flatpak_pull_job_wait (FlatpakPullJob *self, FlatpakPullJobUpdate update, gpointer user_data, GError **error)
{
  g_autoptr(GMainContext) context = g_main_context_ref_thread_default ();
  gboolean done;

  if (g_cancellable_set_error_if_cancelled (self->cancel, error))
    return FALSE;
  do
    {
      g_main_context_iteration (context, FALSE);
      g_mutex_lock (&self->mutex);
      if (!self->done)
        g_cond_wait_until (&self->condition, &self->mutex, g_get_monotonic_time () + 25 * G_TIME_SPAN_MILLISECOND);
      done = self->done;
      g_mutex_unlock (&self->mutex);
      update (self, user_data);
    }
  while (!done);
  /* The last progress callback can cancel an already completed download. */
  if (g_cancellable_set_error_if_cancelled (self->cancel, error))
    return FALSE;
  if (self->error != NULL)
    {
      g_propagate_error (error, g_error_copy (self->error));
      return FALSE;
    }
  return TRUE;
}
