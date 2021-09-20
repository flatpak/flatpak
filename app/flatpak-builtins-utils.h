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

#ifndef __FLATPAK_BUILTINS_UTILS_H__
#define __FLATPAK_BUILTINS_UTILS_H__

#include <glib.h>
#include <appstream.h>
#include "libglnx/libglnx.h"
#include "flatpak-utils-private.h"
#include "flatpak-dir-private.h"
#include "flatpak-permission-dbus-generated.h"

/* Appstream data expires after a day */
#define FLATPAK_APPSTREAM_TTL 86400

typedef struct RemoteDirPair
{
  gchar      *remote_name;
  FlatpakDir *dir;
} RemoteDirPair;

typedef struct RefDirPair
{
  FlatpakDecomposed *ref;
  FlatpakDir *dir;
} RefDirPair;

void         ref_dir_pair_free (RefDirPair *pair);
RefDirPair * ref_dir_pair_new (FlatpakDecomposed *ref,
                               FlatpakDir *dir);

void            remote_dir_pair_free (RemoteDirPair *pair);
RemoteDirPair * remote_dir_pair_new (const char *remote_name,
                                     FlatpakDir *dir);

gboolean    looks_like_branch (const char *branch);

GBytes * flatpak_load_gpg_keys (char        **gpg_import,
                                GCancellable *cancellable,
                                GError      **error);

FlatpakDir * flatpak_find_installed_pref (const char         *pref,
                                          FlatpakKinds        kinds,
                                          const char         *default_arch,
                                          const char         *default_branch,
                                          gboolean            search_all,
                                          gboolean            search_user,
                                          gboolean            search_system,
                                          char              **search_installations,
                                          FlatpakDecomposed **out_ref,
                                          GCancellable       *cancellable,
                                          GError            **error);

gboolean flatpak_resolve_duplicate_remotes (GPtrArray    *dirs,
                                            const char   *remote_name,
                                            FlatpakDir  **out_dir,
                                            GCancellable *cancellable,
                                            GError      **error);

gboolean flatpak_resolve_matching_refs (const char *remote_name,
                                        FlatpakDir *dir,
                                        gboolean    assume_yes,
                                        GPtrArray  *refs,
                                        const char *opt_search_ref,
                                        char      **out_ref,
                                        GError    **error);

gboolean flatpak_resolve_matching_installed_refs (gboolean    assume_yes,
                                                  gboolean    only_one,
                                                  GPtrArray  *ref_dir_pairs,
                                                  const char *opt_search_ref,
                                                  GPtrArray  *out_pairs,
                                                  GError    **error);

gboolean flatpak_resolve_matching_remotes (GPtrArray      *remote_dir_pairs,
                                           const char     *opt_search_ref,
                                           RemoteDirPair **out_pair,
                                           GError        **error);

gboolean update_appstream (GPtrArray    *dirs,
                           const char   *remote,
                           const char   *arch,
                           guint64       ttl,
                           gboolean      quiet,
                           GCancellable *cancellable,
                           GError      **error);

char ** get_permission_tables (XdpDbusPermissionStore *store);
gboolean reset_permissions_for_app (const char *app_id,
                                    GError    **error);


/* --columns handling */

typedef enum {
  FLATPAK_ELLIPSIZE_MODE_NONE,
  FLATPAK_ELLIPSIZE_MODE_START,
  FLATPAK_ELLIPSIZE_MODE_MIDDLE,
  FLATPAK_ELLIPSIZE_MODE_END,
} FlatpakEllipsizeMode;

typedef struct
{
  const char          *name;
  const char          *title; /* use N_() */
  const char          *desc; /* use N_() */
  gboolean             expand;
  FlatpakEllipsizeMode ellipsize;
  gboolean             all;
  gboolean             def;
  gboolean             skip_unique_if_default;
} Column;

int find_column (Column     *columns,
                 const char *name,
                 GError    **error);
char   *column_help (Column *columns);
Column *handle_column_args (Column      *all_columns,
                            gboolean     opt_show_all,
                            const char **opt_cols,
                            GError     **error);

char *  format_timestamp (guint64 timestamp);


char *  ellipsize_string (const char *text,
                          int         len);
char *  ellipsize_string_full (const char          *text,
                               int                  len,
                               FlatpakEllipsizeMode mode);

void print_aligned (int         len,
                    const char *title,
                    const char *value);
void print_aligned_take (int         len,
                         const char *title,
                         char       *value);

AsComponent *as_store_find_app (AsMetadata *mdata,
                                const char *ref);
const char *as_app_get_localized_name (AsComponent *component);
const char *as_app_get_localized_comment (AsComponent *component);
const char *as_app_get_version (AsComponent *component);

gboolean    flatpak_dir_load_appstream_store (FlatpakDir   *self,
                                              const gchar  *remote_name,
                                              const gchar  *arch,
                                              AsMetadata   *mdata,
                                              GCancellable *cancellable,
                                              GError      **error);

int         cell_width (const char *text);
const char *cell_advance (const char *text,
                          int         num);

void print_wrapped (int         columns,
                    const char *text,
                    ...) G_GNUC_PRINTF (2, 3);

FlatpakRemoteState * get_remote_state (FlatpakDir   *dir,
                                       const char   *remote,
                                       gboolean      cached,
                                       gboolean      only_sideloaded,
                                       const char   *opt_arch,
                                       const char  **opt_sideload_repos,
                                       GCancellable *cancellable,
                                       GError      **error);

gboolean ensure_remote_state_arch (FlatpakDir         *dir,
                                   FlatpakRemoteState *state,
                                   const char         *arch,
                                   gboolean            cached,
                                   gboolean            only_sideloaded,
                                   GCancellable       *cancellable,
                                   GError            **error);
gboolean ensure_remote_state_arch_for_ref (FlatpakDir         *dir,
                                           FlatpakRemoteState *state,
                                           const char         *ref,
                                           gboolean            cached,
                                           gboolean            only_sideloaded,
                                           GCancellable       *cancellable,
                                           GError            **error);
gboolean ensure_remote_state_all_arches (FlatpakDir         *dir,
                                         FlatpakRemoteState *state,
                                         gboolean            cached,
                                         gboolean            only_sideloaded,
                                         GCancellable       *cancellable,
                                         GError            **error);

#endif /* __FLATPAK_BUILTINS_UTILS_H__ */
