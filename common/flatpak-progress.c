/*
 * Copyright © 2019 Endless Mobile, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Philip Chimento <philip@endlessm.com>
 */

#include "config.h"

#include <glib-object.h>
#include <glib/gi18n-lib.h>

#include "flatpak-progress-private.h"

static OstreeAsyncProgress *flatpak_progress_issue_ostree_progress (FlatpakProgress *self);
static void flatpak_progress_revoke_ostree_progress (FlatpakProgress     *self,
                                                     OstreeAsyncProgress *ostree_progress);

void
flatpak_main_context_wait (FlatpakMainContext *self,
                           gpointer           *watch_location)
{
  while (*watch_location == NULL)
    g_main_context_iteration (self->context, TRUE);
}

void
flatpak_main_context_finish (FlatpakMainContext *self)
{
  if (self->context == NULL)
    return;

  if (self->flatpak_progress)
    flatpak_progress_revoke_ostree_progress (self->flatpak_progress, self->ostree_progress);
  else
    g_object_unref (self->ostree_progress);

  g_main_context_pop_thread_default (self->context);
  g_main_context_unref (self->context);
}

void
flatpak_progress_init_main_context (FlatpakProgress    *maybe_progress,
                                    FlatpakMainContext *context)
{
  context->context = g_main_context_new ();
  g_main_context_push_thread_default (context->context);

  context->flatpak_progress = maybe_progress;
  if (maybe_progress)
    context->ostree_progress = flatpak_progress_issue_ostree_progress (maybe_progress);
  else
    context->ostree_progress = ostree_async_progress_new ();
}

struct _FlatpakProgress
{
  GObject parent;

  /* Callback */
  FlatpakProgressCallback callback;
  gpointer                user_data;

  char                   *status;

  /* Extra data information */

  guint64 start_time_extra_data;
  guint64 outstanding_extra_data;
  guint64 total_extra_data;
  guint64 transferred_extra_data_bytes;
  guint64 total_extra_data_bytes;    /* the sum of all extra data file sizes (in bytes) */
  guint64 extra_data_previous_dl;

  /* OCI pull information */
  char   *ostree_status;    /* only sent by OSTree when the pull ends (with or without an error) */
  guint64 start_time;
  guint64 bytes_transferred;    /* every and all transferred data (in bytes) */
  guint64 fetched_delta_part_size;    /* the size (in bytes) of already fetched static deltas */
  guint64 total_delta_part_size;    /* the total size (in bytes) of static deltas */
  guint64 total_delta_part_usize;
  guint   outstanding_fetches;    /* missing fetches (metadata + content + deltas) */
  guint   outstanding_writes;    /* all missing writes (sum of outstanding content, metadata and delta writes) */
  guint   fetched;    /* sum of content + metadata fetches */
  guint   requested;    /* sum of requested content + metadata fetches */
  guint   scanning;
  guint   scanned_metadata;
  guint   outstanding_metadata_fetches;    /* missing metadata-only fetches */
  guint   metadata_fetched;    /* the number of fetched metadata objects */
  guint   fetched_delta_parts;
  guint   total_delta_parts;
  guint   fetched_delta_fallbacks;
  guint   total_delta_fallbacks;
  guint   total_delta_superblocks;

  /* Self-progress-reporting fields, not from OSTree */
  guint   progress;
  guint   last_total;

  guint32 update_interval;

  /* Flags */
  guint downloading_extra_data : 1;   /* whether extra-data files are being downloaded or not */
  guint caught_error           : 1;
  guint estimating             : 1;
  guint last_was_metadata      : 1;
  guint done                   : 1;
  guint reported_overflow      : 1;
};

G_DEFINE_TYPE (FlatpakProgress, flatpak_progress, G_TYPE_OBJECT);

static void
flatpak_progress_finalize (GObject *object)
{
  FlatpakProgress *self = FLATPAK_PROGRESS (object);

  g_clear_pointer (&self->status, g_free);
  g_clear_pointer (&self->ostree_status, g_free);

  G_OBJECT_CLASS (flatpak_progress_parent_class)->finalize (object);
}

static void
flatpak_progress_class_init (FlatpakProgressClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = flatpak_progress_finalize;
}

static void
flatpak_progress_init (FlatpakProgress *self)
{
  self->status = g_strdup ("Initializing");
  self->ostree_status = g_strdup ("");
  self->estimating = TRUE;
  self->last_was_metadata = TRUE;
  self->update_interval = FLATPAK_DEFAULT_UPDATE_INTERVAL_MS;
}

FlatpakProgress *
flatpak_progress_new (FlatpakProgressCallback callback,
                      gpointer                user_data)
{
  FlatpakProgress *retval;

  retval = g_object_new (FLATPAK_TYPE_PROGRESS, NULL);
  retval->callback = callback;
  retval->user_data = user_data;

  return retval;
}

static inline guint
get_write_progress (guint outstanding_writes)
{
  return outstanding_writes > 0 ? (guint) (3 / (gdouble) outstanding_writes) : 3;
}

static void
update_status_progress_and_estimating (FlatpakProgress *self)
{
  GString *buf;
  guint64 total = 0;
  guint64 elapsed_time;
  guint new_progress = 0;
  gboolean estimating = FALSE;
  guint64 total_transferred;
  gboolean last_was_metadata = self->last_was_metadata;
  g_autofree gchar *formatted_bytes_total_transferred = NULL;

  buf = g_string_new ("");

  /* We get some extra calls before we've really started due to the initialization of the
     extra data, so ignore those */
  if (self->requested == 0)
    {
      return;
    }

  /* The heuristic here goes as follows:
   *  - While fetching metadata, grow up to 5%
   *  - Download goes up to 97%
   *  - Writing objects adds the last 3%
   */

  elapsed_time = (g_get_monotonic_time () - self->start_time) / G_USEC_PER_SEC;

  /* When we receive the status, it means that the ostree pull operation is
   * finished. We only have to be careful about the extra-data fields. */
  if (*self->ostree_status && self->total_extra_data_bytes == 0)
    {
      g_string_append (buf, self->ostree_status);
      new_progress = 100;
      goto out;
    }

  total_transferred = self->bytes_transferred + self->transferred_extra_data_bytes;
  formatted_bytes_total_transferred =  g_format_size_full (total_transferred, 0);

  self->last_was_metadata = FALSE;

  if (self->total_delta_parts == 0 &&
      (self->outstanding_metadata_fetches > 0 || last_was_metadata)  &&
      self->metadata_fetched < 20)
    {
      /* We need to hit two callbacks with no metadata outstanding, because
         sometimes we get called when we just handled a metadata, but did
         not yet process it and add more metadata */
      if (self->outstanding_metadata_fetches > 0)
        self->last_was_metadata = TRUE;

      /* At this point we don't really know how much data there is, so we have to make a guess.
       * Since its really hard to figure out early how much data there is we report 1% until
       * all objects are scanned. */

      estimating = TRUE;

      g_string_append_printf (buf, _("Downloading metadata: %u/(estimating) %s"),
                              self->fetched, formatted_bytes_total_transferred);

      /* Go up to 5% until the metadata is all fetched */
      new_progress = 0;
      if (self->requested > 0)
        new_progress = self->fetched * 5 / self->requested;
    }
  else
    {
      if (self->total_delta_parts > 0)
        {
          g_autofree gchar *formatted_bytes_total = NULL;

          /* We're only using deltas, so we can ignore regular objects
           * and get perfect sizes.
           *
           * fetched_delta_part_size is the total size of all the
           * delta parts and fallback objects that were already
           * available at the start and need not be downloaded.
           */
          total = self->total_delta_part_size - self->fetched_delta_part_size + self->total_extra_data_bytes;
          formatted_bytes_total = g_format_size_full (total, 0);

          g_string_append_printf (buf, _("Downloading: %s/%s"),
                                  formatted_bytes_total_transferred,
                                  formatted_bytes_total);
        }
      else
        {
          /* Non-deltas, so we can't know anything other than object
             counts, except the additional extra data which we know
             the byte size of. To be able to compare them with the
             extra data we use the average object size to estimate a
             total size. */
          double average_object_size = 1;
          if (self->fetched > 0)
            average_object_size = self->bytes_transferred / (double) self->fetched;

          total = average_object_size * self->requested + self->total_extra_data_bytes;

          if (self->downloading_extra_data)
            {
              g_autofree gchar *formatted_bytes_total = g_format_size_full (total, 0);
              g_string_append_printf (buf, _("Downloading extra data: %s/%s"),
                                      formatted_bytes_total_transferred,
                                      formatted_bytes_total);
            }
          else
            g_string_append_printf (buf, _("Downloading files: %d/%d %s"),
                                    self->fetched, self->requested, formatted_bytes_total_transferred);
        }

      /* The download progress goes up to 97% */
      if (total > 0)
        {
          new_progress = 5 + ((total_transferred / (gdouble) total) * 92);
        }
      else
        {
          new_progress = 97;
        }

      /* And the writing of the objects adds 3% to the progress */
      new_progress += get_write_progress (self->outstanding_writes);
    }

  if (elapsed_time > 0) // Ignore first second
    {
      g_autofree gchar *formatted_bytes_sec = g_format_size (total_transferred / elapsed_time);
      g_string_append_printf (buf, " (%s/s)", formatted_bytes_sec);
    }

out:
  if (new_progress < self->progress && self->last_total == total)
    new_progress = self->progress;
  self->last_total = total;

  if (new_progress > 100)
    {
      if (!self->reported_overflow)
        g_debug ("Unexpectedly got > 100%% progress, limiting");
      self->reported_overflow = TRUE;
      new_progress = 100;
    }

  g_free (self->status);
  self->status = g_string_free (buf, FALSE);
  self->progress = new_progress;
  self->estimating = estimating;
}

void
flatpak_progress_init_extra_data (FlatpakProgress *self,
                                  guint64          n_extra_data,
                                  guint64          total_download_size)
{
  if (self == NULL)
    return;

  self->outstanding_extra_data = n_extra_data;
  self->total_extra_data = n_extra_data;
  self->transferred_extra_data_bytes = 0;
  self->total_extra_data_bytes = total_download_size;
  self->downloading_extra_data = FALSE;
  self->progress = 0;
  update_status_progress_and_estimating (self);
}

void
flatpak_progress_start_extra_data (FlatpakProgress *self)
{
  if (self == NULL)
    return;

  g_assert (self->outstanding_extra_data > 0);

  self->start_time_extra_data = g_get_monotonic_time ();
  self->downloading_extra_data = TRUE;
  update_status_progress_and_estimating (self);
}

void
flatpak_progress_reset_extra_data (FlatpakProgress *self)
{
  if (self == NULL)
    return;

  self->downloading_extra_data = FALSE;
  update_status_progress_and_estimating (self);
}

void
flatpak_progress_update_extra_data (FlatpakProgress *self,
                                    guint64          downloaded_bytes)
{
  if (self == NULL)
    return;

  self->transferred_extra_data_bytes = self->extra_data_previous_dl + downloaded_bytes;
  update_status_progress_and_estimating (self);
}

void
flatpak_progress_complete_extra_data_download (FlatpakProgress *self,
                                               guint64          download_size)
{
  if (self == NULL)
    return;

  g_assert (self->outstanding_extra_data > 0);

  self->extra_data_previous_dl += download_size;
  self->outstanding_extra_data--;
  update_status_progress_and_estimating (self);
}

void
flatpak_progress_start_oci_pull (FlatpakProgress *self)
{
  if (self == NULL)
    return;

  self->start_time = g_get_monotonic_time () - 2;
  self->outstanding_fetches = 0;
  self->outstanding_writes = 0;
  self->fetched = 0;
  self->requested = 0;
  self->scanning = 0;
  self->scanned_metadata = 0;
  self->bytes_transferred = 0;
  self->outstanding_metadata_fetches = 0;
  self->metadata_fetched = 0;
  self->outstanding_extra_data = 0;
  self->total_extra_data = 0;
  self->total_extra_data_bytes = 0;
  self->downloading_extra_data = FALSE;
  self->fetched_delta_parts = 0;
  self->total_delta_parts = 0;
  self->fetched_delta_fallbacks = 0;
  self->total_delta_fallbacks = 0;
  self->fetched_delta_part_size = 0;
  self->total_delta_part_size = 0;
  self->total_delta_part_usize = 0;
  self->total_delta_superblocks = 0;
  self->caught_error = FALSE;
  update_status_progress_and_estimating (self);
}

void
flatpak_progress_update_oci_pull (FlatpakProgress *self,
                                  guint64          total_size,
                                  guint64          pulled_size,
                                  guint32          n_layers,
                                  guint32          pulled_layers)
{
  if (self == NULL)
    return;

  self->requested = n_layers; /* Need to set this to trigger start of progress reporting, see update_status_progress_and_estimating() */
  self->outstanding_fetches = n_layers - pulled_layers;
  self->fetched_delta_parts = pulled_layers;
  self->total_delta_parts = n_layers;
  self->fetched_delta_fallbacks = 0;
  self->total_delta_fallbacks = 0;
  self->bytes_transferred = pulled_size;
  self->total_delta_part_size = total_size;
  self->total_delta_part_usize = total_size;
  self->total_delta_superblocks = 0;
  update_status_progress_and_estimating (self);

  self->callback (self->status, self->progress, self->estimating, self->user_data);
}

guint32
flatpak_progress_get_update_interval (FlatpakProgress *self)
{
  if (self == NULL)
    return FLATPAK_DEFAULT_UPDATE_INTERVAL_MS;
  return self->update_interval;
}

void
flatpak_progress_set_update_interval (FlatpakProgress *self,
                                      guint32          interval)
{
  self->update_interval = interval;
}

guint64
flatpak_progress_get_bytes_transferred (FlatpakProgress *self)
{
  return self->bytes_transferred;
}

guint64
flatpak_progress_get_transferred_extra_data_bytes (FlatpakProgress *self)
{
  return self->transferred_extra_data_bytes;
}

guint64
flatpak_progress_get_start_time (FlatpakProgress *self)
{
  return self->start_time;
}

const char *
flatpak_progress_get_status (FlatpakProgress *self)
{
  return self->status;
}

int
flatpak_progress_get_progress (FlatpakProgress *self)
{
  return self->progress;
}

gboolean
flatpak_progress_get_estimating (FlatpakProgress *self)
{
  return self->estimating;
}

static void
copy_ostree_progress_state (OstreeAsyncProgress *ostree_progress,
                            FlatpakProgress     *self)
{
  gboolean downloading_extra_data, caught_error;

  g_clear_pointer (&self->ostree_status, g_free);

  ostree_async_progress_get (ostree_progress,
                             "start-time-extra-data", "t", &self->start_time_extra_data,
                             "outstanding-extra-data", "t", &self->outstanding_extra_data,
                             "total-extra-data", "t", &self->total_extra_data,
                             "transferred-extra-data-bytes", "t", &self->transferred_extra_data_bytes,
                             "total-extra-data-bytes", "t", &self->total_extra_data_bytes,
                             "status", "s", &self->ostree_status,
                             "start-time", "t", &self->start_time,
                             "bytes-transferred", "t", &self->bytes_transferred,
                             "fetched-delta-part-size", "t", &self->fetched_delta_part_size,
                             "total-delta-part-size", "t", &self->total_delta_part_size,
                             "total-delta-part-usize", "t", &self->total_delta_part_usize,
                             "outstanding-fetches", "u", &self->outstanding_fetches,
                             "outstanding-writes", "u", &self->outstanding_writes,
                             "fetched", "u", &self->fetched,
                             "requested", "u", &self->requested,
                             "scanning", "u", &self->scanning,
                             "scanned-metadata", "u", &self->scanned_metadata,
                             "outstanding-metadata-fetches", "u", &self->outstanding_metadata_fetches,
                             "metadata-fetched", "u", &self->metadata_fetched,
                             "fetched-delta-parts", "u", &self->fetched_delta_parts,
                             "total-delta-parts", "u", &self->total_delta_parts,
                             "fetched-delta-fallbacks", "u", &self->fetched_delta_fallbacks,
                             "total-delta-fallbacks", "u", &self->total_delta_fallbacks,
                             "total-delta-superblocks", "u", &self->total_delta_superblocks,
                             "downloading-extra-data", "u", &downloading_extra_data,
                             "caught-error", "b", &caught_error,
                             NULL);
  /* Bitfield members */
  self->downloading_extra_data = downloading_extra_data;
  self->caught_error = caught_error;

  update_status_progress_and_estimating (self);
}

static void
invoke_callback (OstreeAsyncProgress *ostree_progress,
                 FlatpakProgress     *progress)
{
  copy_ostree_progress_state (ostree_progress, progress);
  progress->callback (progress->status, progress->progress, progress->estimating, progress->user_data);
}

static OstreeAsyncProgress *
flatpak_progress_issue_ostree_progress (FlatpakProgress *self)
{
  OstreeAsyncProgress *ostree_progress = ostree_async_progress_new ();
  ostree_async_progress_set (ostree_progress,
                             "start-time-extra-data", "t", self->start_time_extra_data,
                             "outstanding-extra-data", "t", self->outstanding_extra_data,
                             "total-extra-data", "t", self->total_extra_data,
                             "transferred-extra-data-bytes", "t", self->transferred_extra_data_bytes,
                             "total-extra-data-bytes", "t", self->total_extra_data_bytes,
                             "status", "s", self->ostree_status,
                             "start-time", "t", self->start_time,
                             "bytes-transferred", "t", self->bytes_transferred,
                             "fetched-delta-part-size", "t", self->fetched_delta_part_size,
                             "total-delta-part-size", "t", self->total_delta_part_size,
                             "total-delta-part-usize", "t", self->total_delta_part_usize,
                             "outstanding-fetches", "u", self->outstanding_fetches,
                             "outstanding-writes", "u", self->outstanding_writes,
                             "fetched", "u", self->fetched,
                             "requested", "u", self->requested,
                             "scanning", "u", self->scanning,
                             "scanned-metadata", "u", self->scanned_metadata,
                             "outstanding-metadata-fetches", "u", self->outstanding_metadata_fetches,
                             "metadata-fetched", "u", self->metadata_fetched,
                             "fetched-delta-parts", "u", self->fetched_delta_parts,
                             "total-delta-parts", "u", self->total_delta_parts,
                             "fetched-delta-fallbacks", "u", self->fetched_delta_fallbacks,
                             "total-delta-fallbacks", "u", self->total_delta_fallbacks,
                             "total-delta-superblocks", "u", self->total_delta_superblocks,
                             "downloading-extra-data", "u", (guint) self->downloading_extra_data,
                             "caught-error", "b", self->caught_error,
                             NULL);
  g_signal_connect (ostree_progress, "changed", G_CALLBACK (invoke_callback), self);

  return ostree_progress;
}

static void
flatpak_progress_revoke_ostree_progress (FlatpakProgress     *self,
                                         OstreeAsyncProgress *ostree_progress)
{
  ostree_async_progress_finish (ostree_progress);
  copy_ostree_progress_state (ostree_progress, self);
  g_object_unref (ostree_progress);
}

gboolean
flatpak_progress_is_done (FlatpakProgress *self)
{
  return self->done;
}

void
flatpak_progress_done (FlatpakProgress *self)
{
  self->done = TRUE;
}
