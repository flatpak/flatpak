/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthias Clasen
 */

#ifndef __FLATPAK_INSTANCE_PRIVATE_H__
#define __FLATPAK_INSTANCE_PRIVATE_H__

#include <glib.h>

#include "flatpak-instance.h"

FlatpakInstance *flatpak_instance_new (const char *dir);
FlatpakInstance *flatpak_instance_new_for_id (const char *id);
char *flatpak_instance_get_apps_directory (void);
char *flatpak_instance_get_instances_directory (void);
char *flatpak_instance_allocate_id (char **host_dir_out,
                                    char **host_private_dir_out,
                                    int   *lock_fd_out);

gboolean flatpak_instance_claim_per_app_temp_directory (const char *app_id,
                                                        int per_app_dir_lock_fd,
                                                        int at_fd,
                                                        const char *link_path,
                                                        const char *parent,
                                                        char **path_out,
                                                        GError **error);

void flatpak_instance_iterate_all_and_gc (GPtrArray *out_instances);

gboolean flatpak_instance_ensure_per_app_dir (const char *app_id,
                                              int *lock_fd_out,
                                              char **lock_path_out,
                                              GError **error);
gboolean flatpak_instance_ensure_per_app_dev_shm (const char *app_id,
                                                  int per_app_dir_lock_fd,
                                                  char **shared_dev_shm_out,
                                                  GError **error);
gboolean flatpak_instance_ensure_per_app_tmp (const char *app_id,
                                              int per_app_dir_lock_fd,
                                              char **shared_tmp_out,
                                              GError **error);
gboolean flatpak_instance_ensure_per_app_xdg_runtime_dir (const char *app_id,
                                                          int per_app_dir_lock_fd,
                                                          char **shared_dir_out,
                                                          GError **error);

GStrv flatpak_instance_get_run_environ (FlatpakInstance *self, GError **error);


#endif /* __FLATPAK_INSTANCE_PRIVATE_H__ */
