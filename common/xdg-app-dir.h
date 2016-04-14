/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#ifndef __XDG_APP_DIR_H__
#define __XDG_APP_DIR_H__

#include <ostree.h>

#include <xdg-app-common-types.h>

#define XDG_APP_TYPE_DIR xdg_app_dir_get_type()
#define XDG_APP_DIR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_DIR, XdgAppDir))
#define XDG_APP_IS_DIR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_DIR))

#define XDG_APP_TYPE_DEPLOY xdg_app_deploy_get_type()
#define XDG_APP_DEPLOY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_DEPLOY, XdgAppDeploy))
#define XDG_APP_IS_DEPLOY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_DEPLOY))

GType xdg_app_dir_get_type (void);
GType xdg_app_deploy_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppDir, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppDeploy, g_object_unref)

#define XDG_APP_DIR_ERROR xdg_app_dir_error_quark()

typedef enum {
  XDG_APP_DIR_ERROR_ALREADY_DEPLOYED,
  XDG_APP_DIR_ERROR_ALREADY_UNDEPLOYED,
  XDG_APP_DIR_ERROR_NOT_DEPLOYED,
} XdgAppDirErrorEnum;

GQuark       xdg_app_dir_error_quark      (void);

GFile *  xdg_app_get_system_base_dir_location (void);
GFile *  xdg_app_get_user_base_dir_location   (void);

GKeyFile *     xdg_app_load_override_keyfile (const char  *app_id,
                                              gboolean     user,
                                              GError     **error);
XdgAppContext *xdg_app_load_override_file    (const char  *app_id,
                                              gboolean     user,
                                              GError     **error);
gboolean       xdg_app_save_override_keyfile (GKeyFile    *metakey,
                                              const char  *app_id,
                                              gboolean     user,
                                              GError     **error);

GFile *        xdg_app_deploy_get_dir       (XdgAppDeploy *deploy);
GFile *        xdg_app_deploy_get_files     (XdgAppDeploy *deploy);
XdgAppContext *xdg_app_deploy_get_overrides (XdgAppDeploy *deploy);
GKeyFile *     xdg_app_deploy_get_metadata  (XdgAppDeploy *deploy);

XdgAppDir*  xdg_app_dir_new             (GFile          *basedir,
                                         gboolean        user);
XdgAppDir*  xdg_app_dir_clone           (XdgAppDir      *self);
XdgAppDir  *xdg_app_dir_get             (gboolean        user);
XdgAppDir  *xdg_app_dir_get_system      (void);
XdgAppDir  *xdg_app_dir_get_user        (void);
gboolean    xdg_app_dir_is_user         (XdgAppDir      *self);
GFile *     xdg_app_dir_get_path        (XdgAppDir      *self);
GFile *     xdg_app_dir_get_changed_path (XdgAppDir     *self);
GFile *     xdg_app_dir_get_deploy_dir  (XdgAppDir      *self,
                                         const char     *ref);
char *      xdg_app_dir_get_origin      (XdgAppDir      *self,
                                         const char     *ref,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_set_origin      (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *remote,
                                         GCancellable   *cancellable,
                                         GError        **error);
char **     xdg_app_dir_get_subpaths    (XdgAppDir      *self,
                                         const char     *ref,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_set_subpaths     (XdgAppDir      *self,
                                         const char     *ref,
                                         const char    **subpaths,
                                         GCancellable   *cancellable,
                                         GError        **error);
GFile *     xdg_app_dir_get_exports_dir (XdgAppDir      *self);
GFile *     xdg_app_dir_get_removed_dir (XdgAppDir      *self);
GFile *     xdg_app_dir_get_if_deployed (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         GCancellable   *cancellable);
char *      xdg_app_dir_find_remote_ref (XdgAppDir      *self,
                                         const char     *remote,
                                         const char     *name,
                                         const char     *opt_branch,
                                         const char     *opt_arch,
                                         gboolean        app,
                                         gboolean        runtime,
                                         gboolean       *is_app,
                                         GCancellable   *cancellable,
                                         GError        **error);
char *      xdg_app_dir_find_installed_ref (XdgAppDir      *self,
                                            const char     *name,
                                            const char     *opt_branch,
                                            const char     *opt_arch,
                                            gboolean        app,
                                            gboolean        runtime,
                                            gboolean       *is_app,
                                            GError        **error);
XdgAppDeploy *xdg_app_dir_load_deployed (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         GCancellable   *cancellable,
                                         GError        **error);
char *    xdg_app_dir_load_override     (XdgAppDir      *dir,
                                         const char     *app_id,
                                         gsize          *length,
                                         GError        **error);
OstreeRepo *xdg_app_dir_get_repo        (XdgAppDir      *self);
gboolean    xdg_app_dir_ensure_path     (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_ensure_repo     (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_mark_changed    (XdgAppDir      *self,
                                         GError        **error);
gboolean    xdg_app_dir_remove_appstream(XdgAppDir      *self,
                                         const char     *remote,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_update_appstream(XdgAppDir      *self,
                                         const char     *remote,
                                         const char     *arch,
                                         gboolean       *out_changed,
                                         OstreeAsyncProgress *progress,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_pull            (XdgAppDir      *self,
                                         const char     *repository,
                                         const char     *ref,
                                         char          **subpaths,
                                         OstreeAsyncProgress *progress,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean   xdg_app_dir_pull_from_bundle (XdgAppDir      *self,
                                         GFile          *file,
                                         const char     *remote,
                                         const char     *ref,
                                         gboolean        require_gpg_signature,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_list_refs_for_name (XdgAppDir      *self,
                                            const char     *kind,
                                            const char     *name,
                                            char          ***refs,
                                            GCancellable   *cancellable,
                                            GError        **error);
gboolean    xdg_app_dir_list_refs       (XdgAppDir      *self,
                                         const char     *kind,
                                         char          ***refs,
                                         GCancellable   *cancellable,
                                         GError        **error);
char *      xdg_app_dir_read_latest     (XdgAppDir      *self,
                                         const char     *remote,
                                         const char     *ref,
                                         GCancellable   *cancellable,
                                         GError        **error);
char *      xdg_app_dir_read_active     (XdgAppDir      *self,
                                         const char     *ref,
                                         GCancellable   *cancellable);
gboolean    xdg_app_dir_set_active      (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         GCancellable   *cancellable,
                                         GError        **error);
char *      xdg_app_dir_current_ref     (XdgAppDir      *self,
                                         const char     *name,
                                         GCancellable   *cancellable);
gboolean    xdg_app_dir_drop_current_ref (XdgAppDir      *self,
                                          const char     *name,
                                          GCancellable   *cancellable,
                                          GError        **error);
gboolean    xdg_app_dir_make_current_ref (XdgAppDir      *self,
                                          const char     *ref,
                                          GCancellable   *cancellable,
                                          GError        **error);
gboolean    xdg_app_dir_list_deployed   (XdgAppDir      *self,
                                         const char     *ref,
                                         char         ***deployed_checksums,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_lock            (XdgAppDir      *self,
                                         GLnxLockFile   *lockfile,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_deploy          (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_deploy_update   (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         gboolean       *was_updated,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_undeploy        (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
					 gboolean        force_remove,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_undeploy_all    (XdgAppDir      *self,
                                         const char     *ref,
					 gboolean        force_remove,
                                         gboolean       *was_deployed_out,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_remove_all_refs (XdgAppDir      *self,
                                         const char     *remote,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_remove_ref      (XdgAppDir      *self,
                                         const char     *remote_name,
                                         const char     *ref,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_update_exports  (XdgAppDir      *self,
                                         const char     *app,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_prune           (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_cleanup_removed (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_collect_deployed_refs (XdgAppDir *self,
					       const char *type,
					       const char *name_prefix,
					       const char *branch,
					       const char *arch,
					       GHashTable *hash,
					       GCancellable *cancellable,
					       GError **error);
char      *xdg_app_dir_create_origin_remote (XdgAppDir *self,
                                             const char *url,
                                             const char *id,
                                             const char *title,
                                             GBytes *gpg_data,
                                             GCancellable *cancellable,
                                             GError **error);
char     **xdg_app_dir_list_remotes     (XdgAppDir *self,
                                         GCancellable *cancellable,
                                         GError **error);
char      *xdg_app_dir_get_remote_title (XdgAppDir *self,
                                         const char *remote_name);
int        xdg_app_dir_get_remote_prio  (XdgAppDir *self,
                                         const char *remote_name);
gboolean   xdg_app_dir_get_remote_noenumerate (XdgAppDir *self,
                                               const char *remote_name);
gboolean   xdg_app_dir_list_remote_refs (XdgAppDir *self,
                                         const char *remote,
                                         GHashTable **refs,
                                         GCancellable *cancellable,
                                         GError **error);
char *   xdg_app_dir_fetch_remote_title (XdgAppDir *self,
                                         const char *remote,
                                         GCancellable *cancellable,
                                         GError **error);
GBytes * xdg_app_dir_fetch_remote_object (XdgAppDir *self,
                                          const char *remote,
                                          const char *checksum,
                                          const char *type,
                                          GCancellable *cancellable,
                                          GError **error);
GBytes * xdg_app_dir_fetch_metadata      (XdgAppDir *self,
                                          const char *remote_name,
                                          const char *commit,
                                          GCancellable *cancellable,
                                          GError **error);
gboolean xdg_app_dir_get_installed_size (XdgAppDir *self,
                                         const char *commit,
                                         guint64 *installed_size,
                                         GCancellable *cancellable,
                                         GError **error);
gboolean xdg_app_dir_fetch_sizes         (XdgAppDir *self,
                                          const char *remote_name,
                                          const char *commit,
                                          guint64    *new_archived,
                                          guint64    *new_unpacked,
                                          guint64    *total_archived,
                                          guint64    *total_unpacked,
                                          GCancellable *cancellable,
                                          GError **error);
gboolean xdg_app_dir_fetch_ref_cache (XdgAppDir    *self,
                                      const char   *remote_name,
                                      const char   *ref,
                                      guint64      *download_size,
                                      guint64      *installed_size,
                                      char        **metadata,
                                      GCancellable *cancellable,
                                      GError      **error);

#endif /* __XDG_APP_DIR_H__ */
