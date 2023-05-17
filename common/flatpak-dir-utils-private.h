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
 */

#pragma once

#include "libglnx.h"

#include "flatpak-ref-utils-private.h"

G_BEGIN_DECLS

typedef struct
{
  char               *id;
  char               *installed_id;
  char               *commit;
  FlatpakDecomposed *ref;
  char              *directory;
  char              *files_path;
  char              *subdir_suffix;
  char              *add_ld_path;
  char             **merge_dirs;
  int                priority;
  gboolean           needs_tmpfs;
  gboolean           is_unmaintained;
} FlatpakExtension;

void flatpak_extension_free (FlatpakExtension *extension);

FlatpakDecomposed *flatpak_find_current_ref (const char   *app_id,
                                             GCancellable *cancellable,
                                             GError      **error);
GFile *flatpak_find_deploy_dir_for_ref (FlatpakDecomposed  *ref,
                                        FlatpakDir        **dir_out,
                                        GCancellable       *cancellable,
                                        GError            **error);
GFile * flatpak_find_files_dir_for_ref (FlatpakDecomposed *ref,
                                        GCancellable      *cancellable,
                                        GError           **error);
GFile * flatpak_find_unmaintained_extension_dir_if_exists (const char   *name,
                                                           const char   *arch,
                                                           const char   *branch,
                                                           GCancellable *cancellable);
FlatpakDeploy * flatpak_find_deploy_for_ref_in (GPtrArray    *dirs,
                                                const char   *ref,
                                                const char   *commit,
                                                GCancellable *cancellable,
                                                GError      **error);
FlatpakDeploy * flatpak_find_deploy_for_ref (const char   *ref,
                                             const char   *commit,
                                             FlatpakDir   *opt_user_dir,
                                             GCancellable *cancellable,
                                             GError      **error);
char ** flatpak_list_deployed_refs (const char   *type,
                                    const char   *name_prefix,
                                    const char   *arch,
                                    const char   *branch,
                                    GCancellable *cancellable,
                                    GError      **error);
char ** flatpak_list_unmaintained_refs (const char   *name_prefix,
                                        const char   *branch,
                                        const char   *arch,
                                        GCancellable *cancellable,
                                        GError      **error);
GList *flatpak_list_extensions (GKeyFile   *metakey,
                                const char *arch,
                                const char *branch);

void flatpak_log_dir_access (FlatpakDir *dir);

G_END_DECLS
