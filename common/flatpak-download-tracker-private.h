/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include <glib.h>
#include <stdint.h>

typedef struct _FlatpakTransactionOperation FlatpakTransactionOperation;
typedef struct _FlatpakDownloadTracker FlatpakDownloadTracker;

typedef struct
{
  uint64_t bytes_transferred;
  uint64_t bytes_total;
  uint64_t bytes_per_second;
  guint    active_downloads;
} FlatpakDownloadStats;

typedef struct
{
  FlatpakTransactionOperation *operation;
  FlatpakDownloadStats         stats;
  gboolean                     changed;
} FlatpakDownloadItem;

typedef struct
{
  FlatpakDownloadStats aggregate;
  FlatpakDownloadItem *items;
  size_t               n_items;
} FlatpakDownloadSnapshot;

FlatpakDownloadTracker *flatpak_download_tracker_new (void);
void flatpak_download_tracker_free (FlatpakDownloadTracker *self);
/* Operation keys are borrowed. Register them once, in notification order. */
void flatpak_download_tracker_add (FlatpakDownloadTracker      *self,
                                   FlatpakTransactionOperation *operation,
                                   uint64_t                     initial_total_bytes);
/* Inactive downloads may resume; updates to retired downloads are ignored. */
void flatpak_download_tracker_update (FlatpakDownloadTracker      *self,
                                      FlatpakTransactionOperation *operation,
                                      uint64_t                     bytes_transferred,
                                      uint64_t                     bytes_total,
                                      uint64_t                     bytes_per_second,
                                      gboolean                     active);
void flatpak_download_tracker_retire (FlatpakDownloadTracker      *self,
                                      FlatpakTransactionOperation *operation);
/* Includes downloads with progress updates and totals for queued work.
 * Clears change flags; later updates do not modify the returned snapshot.
 * A zero total means unknown unless all downloads have finished. */
FlatpakDownloadSnapshot *flatpak_download_tracker_take_snapshot (FlatpakDownloadTracker *self);
void flatpak_download_snapshot_free (FlatpakDownloadSnapshot *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDownloadTracker, flatpak_download_tracker_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDownloadSnapshot, flatpak_download_snapshot_free)
