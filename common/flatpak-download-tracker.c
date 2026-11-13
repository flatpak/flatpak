/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include "flatpak-download-tracker-private.h"

typedef struct
{
  FlatpakDownloadItem item;
  uint64_t            initial_total_bytes;
  gboolean            has_update;
  gboolean            retired;
} Download;

struct _FlatpakDownloadTracker
{
  GHashTable *downloads;
  GPtrArray  *order;
};

FlatpakDownloadTracker *
flatpak_download_tracker_new (void)
{
  FlatpakDownloadTracker *self = g_new0 (FlatpakDownloadTracker, 1);

  self->downloads = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  self->order = g_ptr_array_new ();
  return self;
}

void
flatpak_download_tracker_free (FlatpakDownloadTracker *self)
{
  g_hash_table_unref (self->downloads);
  g_ptr_array_unref (self->order);
  g_free (self);
}

void
flatpak_download_tracker_add (FlatpakDownloadTracker *self, FlatpakTransactionOperation *operation, uint64_t initial_total_bytes)
{
  Download *download;

  g_return_if_fail (!g_hash_table_contains (self->downloads, operation));
  download = g_new0 (Download, 1);
  download->item.operation = operation;
  download->initial_total_bytes = initial_total_bytes;
  g_hash_table_insert (self->downloads, operation, download);
  g_ptr_array_add (self->order, download);
}

void
flatpak_download_tracker_update (FlatpakDownloadTracker *self, FlatpakTransactionOperation *operation,
                                 uint64_t bytes_transferred, uint64_t bytes_total,
                                 uint64_t bytes_per_second, gboolean active)
{
  Download *download = g_hash_table_lookup (self->downloads, operation);
  FlatpakDownloadStats *stats;

  if (download == NULL || download->retired)
    return;

  stats = &download->item.stats;
  bytes_transferred = MAX (stats->bytes_transferred, bytes_transferred);
  bytes_total = active ? bytes_total : bytes_transferred;
  bytes_per_second = active ? bytes_per_second : 0;
  download->item.changed |= !download->has_update || stats->bytes_transferred != bytes_transferred ||
                            stats->bytes_total != bytes_total || stats->bytes_per_second != bytes_per_second ||
                            stats->active_downloads != active;
  download->has_update = TRUE;
  *stats = (FlatpakDownloadStats){ bytes_transferred, bytes_total, bytes_per_second, active };
}

void
flatpak_download_tracker_retire (FlatpakDownloadTracker *self, FlatpakTransactionOperation *operation)
{
  Download *download = g_hash_table_lookup (self->downloads, operation);

  if (download == NULL || download->retired)
    return;

  download->retired = TRUE;
  download->item.changed = TRUE;
  download->item.stats.active_downloads = 0;
  download->item.stats.bytes_per_second = 0;
  download->item.stats.bytes_total = download->item.stats.bytes_transferred;
}

FlatpakDownloadSnapshot *
flatpak_download_tracker_take_snapshot (FlatpakDownloadTracker *self)
{
  FlatpakDownloadSnapshot *snapshot = g_new0 (FlatpakDownloadSnapshot, 1);
  gboolean known = TRUE;

  snapshot->items = g_new (FlatpakDownloadItem, self->order->len);
  for (size_t i = 0; i < self->order->len; i++)
    {
      Download *download = g_ptr_array_index (self->order, i);
      FlatpakDownloadItem item = download->item;
      uint64_t estimated_total_bytes = download->retired ? item.stats.bytes_transferred :
                                       item.stats.bytes_total ? item.stats.bytes_total : download->initial_total_bytes;
      gboolean estimate_known = download->retired || estimated_total_bytes != 0;

      item.stats.bytes_total = estimate_known ? MAX (estimated_total_bytes, item.stats.bytes_transferred) : 0;
      snapshot->aggregate.bytes_transferred += item.stats.bytes_transferred;
      snapshot->aggregate.bytes_total += item.stats.bytes_total;
      snapshot->aggregate.bytes_per_second += item.stats.bytes_per_second;
      snapshot->aggregate.active_downloads += item.stats.active_downloads;
      known &= estimate_known;
      if (download->has_update)
        snapshot->items[snapshot->n_items++] = item;
      download->item.changed = FALSE;
    }
  if (!known)
    snapshot->aggregate.bytes_total = 0;
  return snapshot;
}

void
flatpak_download_snapshot_free (FlatpakDownloadSnapshot *self)
{
  g_free (self->items);
  g_free (self);
}
