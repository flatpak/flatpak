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

#ifndef FLATPAK_PROGRESS_H
#define FLATPAK_PROGRESS_H

#include <glib-object.h>
#include <ostree.h>

#include "flatpak-installation.h"

#define FLATPAK_TYPE_PROGRESS flatpak_progress_get_type ()

G_DECLARE_FINAL_TYPE (FlatpakProgress, flatpak_progress, FLATPAK, PROGRESS, GObject);

#define FLATKPAK_MAIN_CONTEXT_INIT {NULL}
#define FLATPAK_DEFAULT_UPDATE_INTERVAL_MS 100

struct _FlatpakMainContext {
  GMainContext        *context;
  FlatpakProgress     *flatpak_progress;
  OstreeAsyncProgress *ostree_progress;
};
typedef struct _FlatpakMainContext FlatpakMainContext;

void flatpak_main_context_wait (FlatpakMainContext *self,
                                gpointer           *watch_location);
void flatpak_main_context_finish (FlatpakMainContext *self);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (FlatpakMainContext, flatpak_main_context_finish);

FlatpakProgress *flatpak_progress_new (FlatpakProgressCallback callback,
                                       gpointer                user_data);

void flatpak_progress_init_extra_data (FlatpakProgress *self,
                                       guint64          n_extra_data,
                                       guint64          total_download_size);
gboolean flatpak_progress_get_extra_data_initialized (FlatpakProgress *self);
void flatpak_progress_start_extra_data (FlatpakProgress *self);
void flatpak_progress_reset_extra_data (FlatpakProgress *self);
void flatpak_progress_update_extra_data (FlatpakProgress *self,
                                         guint64          downloaded_bytes);
void flatpak_progress_complete_extra_data_download (FlatpakProgress *self,
                                                    guint64          download_size);

void flatpak_progress_start_oci_pull (FlatpakProgress *self);
void flatpak_progress_update_oci_pull (FlatpakProgress *self,
                                       guint64          total_size,
                                       guint64          pulled_size,
                                       guint32          n_layers,
                                       guint32          pulled_layers);

guint32 flatpak_progress_get_update_interval (FlatpakProgress *self);
void flatpak_progress_set_update_interval (FlatpakProgress *self,
                                           guint32          interval);

guint64 flatpak_progress_get_bytes_transferred (FlatpakProgress *self);
guint64 flatpak_progress_get_transferred_extra_data_bytes (FlatpakProgress *self);
guint64 flatpak_progress_get_start_time (FlatpakProgress *self);
const char *flatpak_progress_get_status (FlatpakProgress *self);
int flatpak_progress_get_progress (FlatpakProgress *self);
gboolean flatpak_progress_get_estimating (FlatpakProgress *self);

gboolean flatpak_progress_is_done (FlatpakProgress *self);
void flatpak_progress_done (FlatpakProgress *self);

void flatpak_progress_init_main_context (FlatpakProgress *maybe_progress,
                                         FlatpakMainContext *context);

#endif  /* FLATPAK_PROGRESS_H */
