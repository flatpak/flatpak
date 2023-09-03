/*
 * Copyright © 2014 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 */

#ifndef __FLATPAK_RUN_H__
#define __FLATPAK_RUN_H__

#include "libglnx.h"
#include "flatpak-common-types-private.h"
#include "flatpak-context-private.h"
#include "flatpak-bwrap-private.h"
#include "flatpak-metadata-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-exports-private.h"

gboolean flatpak_run_in_transient_unit (const char *app_id,
                                        GError    **error);

void     flatpak_run_extend_ld_path       (FlatpakBwrap       *bwrap,
                                           const char         *prepend,
                                           const char         *append);
gboolean flatpak_run_add_extension_args   (FlatpakBwrap       *bwrap,
                                           GKeyFile           *metakey,
                                           FlatpakDecomposed  *ref,
                                           gboolean            use_ld_so_cache,
                                           const char         *target_path,
                                           char              **extensions_out,
                                           char              **ld_path_out,
                                           GCancellable       *cancellable,
                                           GError            **error);
gboolean flatpak_run_add_environment_args (FlatpakBwrap       *bwrap,
                                           const char         *app_info_path,
                                           FlatpakRunFlags     flags,
                                           const char         *app_id,
                                           FlatpakContext     *context,
                                           GFile              *app_id_dir,
                                           GPtrArray          *previous_app_id_dirs,
                                           int                 per_app_dir_lock_fd,
                                           const char         *instance_id,
                                           FlatpakExports    **exports_out,
                                           GCancellable       *cancellable,
                                           GError            **error);
char **  flatpak_run_get_minimal_env (gboolean devel,
                                      gboolean use_ld_so_cache);
void     flatpak_run_apply_env_default (FlatpakBwrap *bwrap,
                                        gboolean      use_ld_so_cache);
void      flatpak_run_apply_env_vars (FlatpakBwrap   *bwrap,
                                      FlatpakContext *context);
FlatpakContext *flatpak_app_compute_permissions (GKeyFile *app_metadata,
                                                 GKeyFile *runtime_metadata,
                                                 GError  **error);
gboolean flatpak_ensure_data_dir (GFile        *app_id_dir,
                                  GCancellable *cancellable,
                                  GError      **error);

gboolean flatpak_run_setup_base_argv (FlatpakBwrap   *bwrap,
                                      GFile          *runtime_files,
                                      GFile          *app_id_dir,
                                      const char     *arch,
                                      FlatpakRunFlags flags,
                                      GError        **error);
gboolean flatpak_run_add_app_info_args (FlatpakBwrap       *bwrap,
                                        GFile              *app_files,
                                        GFile              *original_app_files,
                                        GBytes             *app_deploy_data,
                                        const char         *app_extensions,
                                        GFile              *runtime_files,
                                        GFile              *original_runtime_files,
                                        GBytes             *runtime_deploy_data,
                                        const char         *runtime_extensions,
                                        const char         *app_id,
                                        const char         *app_branch,
                                        FlatpakDecomposed  *runtime_ref,
                                        GFile              *app_id_dir,
                                        FlatpakContext     *final_app_context,
                                        FlatpakContext     *cmdline_context,
                                        gboolean            sandbox,
                                        gboolean            build,
                                        gboolean            devel,
                                        char              **app_info_path_out,
                                        int                 instance_id_fd,
                                        char              **host_instance_id_host_dir_out,
                                        char              **host_instance_id_host_private_dir_out,
                                        char              **instance_id_out,
                                        GError            **error);

gboolean flatpak_run_app (FlatpakDecomposed  *app_ref,
                          FlatpakDeploy      *app_deploy,
                          const char         *custom_app_path,
                          FlatpakContext     *extra_context,
                          const char         *custom_runtime,
                          const char         *custom_runtime_version,
                          const char         *custom_runtime_commit,
                          const char         *custom_usr_path,
                          int                 parent_pid,
                          FlatpakRunFlags     flags,
                          const char         *cwd,
                          const char         *custom_command,
                          char               *args[],
                          int                 n_args,
                          int                 instance_id_fd,
                          char              **instance_dir_out,
                          GCancellable       *cancellable,
                          GError            **error);

#endif /* __FLATPAK_RUN_H__ */
