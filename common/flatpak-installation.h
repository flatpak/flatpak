/*
 * Copyright © 2015 Red Hat, Inc
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

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_INSTALLATION_H__
#define __FLATPAK_INSTALLATION_H__

typedef struct _FlatpakInstallation FlatpakInstallation;

#include <gio/gio.h>
#include <flatpak-installed-ref.h>
#include <flatpak-instance.h>
#include <flatpak-remote.h>

#define FLATPAK_TYPE_INSTALLATION flatpak_installation_get_type ()
#define FLATPAK_INSTALLATION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_INSTALLATION, FlatpakInstallation))
#define FLATPAK_IS_INSTALLATION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_INSTALLATION))

FLATPAK_EXTERN GType flatpak_installation_get_type (void);

struct _FlatpakInstallation
{
  GObject parent;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakInstallationClass;

/**
 * FlatpakUpdateFlags:
 * @FLATPAK_UPDATE_FLAGS_NONE: Fetch remote builds and install the latest one (default)
 * @FLATPAK_UPDATE_FLAGS_NO_DEPLOY: Don't install any new builds that might be fetched
 * @FLATPAK_UPDATE_FLAGS_NO_PULL: Don't try to fetch new builds from the remote repo
 * @FLATPAK_UPDATE_FLAGS_NO_STATIC_DELTAS: Don't use static deltas when pulling
 * @FLATPAK_UPDATE_FLAGS_NO_PRUNE: Don't prune the local OSTree repository after updating (Since: 0.11.8)
 * @FLATPAK_UPDATE_FLAGS_NO_TRIGGERS: Don't call triggers after updating. If used,
 * the caller must later call flatpak_installation_run_triggers() to update
 * the exported files. (Since: 1.0.3)
 *
 * Flags to alter the behavior of flatpak_installation_update().
 */
typedef enum {
  FLATPAK_UPDATE_FLAGS_NONE             = 0,
  FLATPAK_UPDATE_FLAGS_NO_DEPLOY        = (1 << 0),
  FLATPAK_UPDATE_FLAGS_NO_PULL          = (1 << 1),
  FLATPAK_UPDATE_FLAGS_NO_STATIC_DELTAS = (1 << 2),
  FLATPAK_UPDATE_FLAGS_NO_PRUNE         = (1 << 3),
  FLATPAK_UPDATE_FLAGS_NO_TRIGGERS      = (1 << 4),
} FlatpakUpdateFlags;

/**
 * FlatpakInstallFlags:
 * @FLATPAK_INSTALL_FLAGS_NONE: Default
 * @FLATPAK_INSTALL_FLAGS_NO_STATIC_DELTAS: Don't use static deltas when pulling
 * @FLATPAK_INSTALL_FLAGS_NO_DEPLOY: Don't install any new builds that might be fetched
 * @FLATPAK_INSTALL_FLAGS_NO_PULL: Don't try to fetch new builds from the remote repo
 * @FLATPAK_INSTALL_FLAGS_NO_TRIGGERS: Don't call triggers after installing. If used,
 * the caller must later call flatpak_installation_run_triggers() to update
 * the exported files. (Since: 1.0.3)
 *
 * Flags to alter the behavior of flatpak_installation_install_full().
 */
typedef enum {
  FLATPAK_INSTALL_FLAGS_NONE             = 0,
  FLATPAK_INSTALL_FLAGS_NO_STATIC_DELTAS = (1 << 0),
  FLATPAK_INSTALL_FLAGS_NO_DEPLOY        = (1 << 2),
  FLATPAK_INSTALL_FLAGS_NO_PULL          = (1 << 3),
  FLATPAK_INSTALL_FLAGS_NO_TRIGGERS      = (1 << 4),
} FlatpakInstallFlags;

/**
 * FlatpakUninstallFlags:
 * @FLATPAK_UNINSTALL_FLAGS_NONE: Default
 * @FLATPAK_UNINSTALL_FLAGS_NO_PRUNE: Don't prune the local OSTree repository after uninstalling
 * @FLATPAK_UNINSTALL_FLAGS_NO_TRIGGERS: Don't call triggers after uninstalling. If used,
 * the caller must later call flatpak_installation_run_triggers() to update
 * the exported file. (Since: 1.0.3)
 *
 * Flags to alter the behavior of flatpak_installation_uninstall_full().
 *
 * Since: 0.11.8
 */
typedef enum {
  FLATPAK_UNINSTALL_FLAGS_NONE            = 0,
  FLATPAK_UNINSTALL_FLAGS_NO_PRUNE        = (1 << 0),
  FLATPAK_UNINSTALL_FLAGS_NO_TRIGGERS     = (1 << 1),
} FlatpakUninstallFlags;

/**
 * FlatpakLaunchFlags:
 * @FLATPAK_LAUNCH_FLAGS_NONE: Default
 * @FLATPAK_LAUNCH_FLAGS_DO_NOT_REAP: Do not reap the child. Use this if you want to wait
 * for the child with g_child_watch_add(). (Snce: 1.1)
 *
 * Flags to alter the behavior of flatpak_installation_launch_full().
 */
typedef enum {
  FLATPAK_LAUNCH_FLAGS_NONE        = 0,
  FLATPAK_LAUNCH_FLAGS_DO_NOT_REAP = (1 << 0),
} FlatpakLaunchFlags;

/**
 * FlatpakQueryFlags:
 * @FLATPAK_QUERY_FLAGS_NONE: Default
 * @FLATPAK_QUERY_FLAGS_ONLY_CACHED: Don't do any network i/o, but only return cached data.
 * This can return stale data, or a #FLATPAK_ERROR_NOT_CACHED error, however it is a
 * lot more efficient if you're doing many requests.
 * @FLATPAK_QUERY_FLAGS_ONLY_SIDELOADED: Only list refs available from any eventuall sideload repos. (Snce: 1.7)
 *
 * Flags to alter the behavior of e.g flatpak_installation_list_remote_refs_sync_full().
 *
 * Since: 1.3.3
 */
typedef enum {
  FLATPAK_QUERY_FLAGS_NONE        = 0,
  FLATPAK_QUERY_FLAGS_ONLY_CACHED = (1 << 0),
  FLATPAK_QUERY_FLAGS_ONLY_SIDELOADED = (1 << 1),
} FlatpakQueryFlags;

/**
 * FlatpakStorageType:
 * @FLATPAK_STORAGE_TYPE_DEFAULT: default
 * @FLATPAK_STORAGE_TYPE_HARD_DISK: installation is on a hard disk
 * @FLATPAK_STORAGE_TYPE_SDCARD: installation is on a SD card
 * @FLATPAK_STORAGE_TYPE_MMC: installation is on an MMC
 * @FLATPAK_STORAGE_TYPE_NETWORK: installation is on the network
 *
 * Information about the storage of an installation.
 *
 * Since: 0.6.15
 */
typedef enum {
  FLATPAK_STORAGE_TYPE_DEFAULT = 0,
  FLATPAK_STORAGE_TYPE_HARD_DISK,
  FLATPAK_STORAGE_TYPE_SDCARD,
  FLATPAK_STORAGE_TYPE_MMC,
  FLATPAK_STORAGE_TYPE_NETWORK,
} FlatpakStorageType;


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakInstallation, g_object_unref)
#endif

FLATPAK_EXTERN const char  *flatpak_get_default_arch (void);

FLATPAK_EXTERN const char *const *flatpak_get_supported_arches (void);

FLATPAK_EXTERN GPtrArray *flatpak_get_system_installations (GCancellable *cancellable,
                                                            GError      **error);
FLATPAK_EXTERN FlatpakInstallation *flatpak_installation_new_system (GCancellable *cancellable,
                                                                     GError      **error);
FLATPAK_EXTERN FlatpakInstallation *flatpak_installation_new_system_with_id (const char   *id,
                                                                             GCancellable *cancellable,
                                                                             GError      **error);
FLATPAK_EXTERN FlatpakInstallation *flatpak_installation_new_user (GCancellable *cancellable,
                                                                   GError      **error);
FLATPAK_EXTERN FlatpakInstallation *flatpak_installation_new_for_path (GFile        *path,
                                                                       gboolean      user,
                                                                       GCancellable *cancellable,
                                                                       GError      **error);
FLATPAK_EXTERN void flatpak_installation_set_no_interaction (FlatpakInstallation *self,
                                                             gboolean             no_interaction);
FLATPAK_EXTERN gboolean flatpak_installation_get_no_interaction (FlatpakInstallation *self);

/**
 * FlatpakProgressCallback:
 * @status: A status string, suitable for display
 * @progress: percentage of completion
 * @estimating: whether @progress is just an estimate
 * @user_data: User data passed to the caller
 *
 * The progress callback is called repeatedly during long-running operations
 * such as installations or updates, and can be used to update progress information
 * in a user interface.
 *
 * The callback occurs in the thread-default context of the caller.
 */
typedef void (*FlatpakProgressCallback)(const char *status,
                                        guint       progress,
                                        gboolean    estimating,
                                        gpointer    user_data);

FLATPAK_EXTERN gboolean             flatpak_installation_drop_caches (FlatpakInstallation *self,
                                                                      GCancellable        *cancellable,
                                                                      GError             **error);
FLATPAK_EXTERN gboolean             flatpak_installation_get_is_user (FlatpakInstallation *self);
FLATPAK_EXTERN GFile               *flatpak_installation_get_path (FlatpakInstallation *self);
FLATPAK_EXTERN const char          *flatpak_installation_get_id (FlatpakInstallation *self);
FLATPAK_EXTERN const char          *flatpak_installation_get_display_name (FlatpakInstallation *self);
FLATPAK_EXTERN gint                 flatpak_installation_get_priority (FlatpakInstallation *self);
FLATPAK_EXTERN FlatpakStorageType   flatpak_installation_get_storage_type (FlatpakInstallation *self);
FLATPAK_EXTERN gboolean             flatpak_installation_launch (FlatpakInstallation *self,
                                                                 const char          *name,
                                                                 const char          *arch,
                                                                 const char          *branch,
                                                                 const char          *commit,
                                                                 GCancellable        *cancellable,
                                                                 GError             **error);
FLATPAK_EXTERN gboolean             flatpak_installation_launch_full (FlatpakInstallation *self,
                                                                      FlatpakLaunchFlags   flags,
                                                                      const char          *name,
                                                                      const char          *arch,
                                                                      const char          *branch,
                                                                      const char          *commit,
                                                                      FlatpakInstance    **instance_out,
                                                                      GCancellable        *cancellable,
                                                                      GError             **error);
FLATPAK_EXTERN GFileMonitor        *flatpak_installation_create_monitor (FlatpakInstallation *self,
                                                                         GCancellable        *cancellable,
                                                                         GError             **error);
FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_installed_refs (FlatpakInstallation *self,
                                                                              GCancellable        *cancellable,
                                                                              GError             **error);
FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_installed_refs_by_kind (FlatpakInstallation *self,
                                                                                      FlatpakRefKind       kind,
                                                                                      GCancellable        *cancellable,
                                                                                      GError             **error);
FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_installed_refs_for_update (FlatpakInstallation *self,
                                                                                         GCancellable        *cancellable,
                                                                                         GError             **error);
FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_unused_refs (FlatpakInstallation *self,
                                                                           const char          *arch,
                                                                           GCancellable        *cancellable,
                                                                           GError             **error);
FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_unused_refs_with_options (FlatpakInstallation *self,
                                                                                        const char          *arch,
                                                                                        GVariant            *options,
                                                                                        GCancellable        *cancellable,
                                                                                        GError             **error);
FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_pinned_refs (FlatpakInstallation *self,
                                                                           const char          *arch,
                                                                           GCancellable        *cancellable,
                                                                           GError             **error);
FLATPAK_EXTERN FlatpakInstalledRef * flatpak_installation_get_installed_ref (FlatpakInstallation *self,
                                                                             FlatpakRefKind       kind,
                                                                             const char          *name,
                                                                             const char          *arch,
                                                                             const char          *branch,
                                                                             GCancellable        *cancellable,
                                                                             GError             **error);
FLATPAK_EXTERN FlatpakInstalledRef * flatpak_installation_get_current_installed_app (FlatpakInstallation *self,
                                                                                     const char          *name,
                                                                                     GCancellable        *cancellable,
                                                                                     GError             **error);

FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_remotes (FlatpakInstallation *self,
                                                                       GCancellable        *cancellable,
                                                                       GError             **error);
FLATPAK_EXTERN GPtrArray           *flatpak_installation_list_remotes_by_type (FlatpakInstallation    *self,
                                                                               const FlatpakRemoteType types[],
                                                                               gsize                   num_types,
                                                                               GCancellable           *cancellable,
                                                                               GError                **error);
FLATPAK_EXTERN FlatpakRemote        *flatpak_installation_get_remote_by_name (FlatpakInstallation *self,
                                                                              const gchar         *name,
                                                                              GCancellable        *cancellable,
                                                                              GError             **error);
FLATPAK_EXTERN gboolean              flatpak_installation_add_remote (FlatpakInstallation *self,
                                                                      FlatpakRemote       *remote,
                                                                      gboolean             if_needed,
                                                                      GCancellable        *cancellable,
                                                                      GError             **error);
FLATPAK_EXTERN gboolean              flatpak_installation_modify_remote (FlatpakInstallation *self,
                                                                         FlatpakRemote       *remote,
                                                                         GCancellable        *cancellable,
                                                                         GError             **error);
FLATPAK_EXTERN gboolean              flatpak_installation_remove_remote (FlatpakInstallation *self,
                                                                         const char          *name,
                                                                         GCancellable        *cancellable,
                                                                         GError             **error);
FLATPAK_EXTERN gboolean              flatpak_installation_update_remote_sync (FlatpakInstallation *self,
                                                                              const char          *name,
                                                                              GCancellable        *cancellable,
                                                                              GError             **error);
FLATPAK_EXTERN gboolean              flatpak_installation_set_config_sync (FlatpakInstallation *self,
                                                                           const char          *key,
                                                                           const char          *value,
                                                                           GCancellable        *cancellable,
                                                                           GError             **error);
FLATPAK_EXTERN gboolean              flatpak_installation_get_min_free_space_bytes (FlatpakInstallation *self,
                                                                                    guint64             *out_bytes,
                                                                                    GError             **error);
FLATPAK_EXTERN char *                flatpak_installation_get_config (FlatpakInstallation *self,
                                                                      const char          *key,
                                                                      GCancellable        *cancellable,
                                                                      GError             **error);
FLATPAK_EXTERN char **               flatpak_installation_get_default_languages (FlatpakInstallation  *self,
                                                                                 GError              **error);
FLATPAK_EXTERN char **               flatpak_installation_get_default_locales (FlatpakInstallation  *self,
                                                                               GError              **error);
FLATPAK_EXTERN char *              flatpak_installation_load_app_overrides (FlatpakInstallation *self,
                                                                            const char          *app_id,
                                                                            GCancellable        *cancellable,
                                                                            GError             **error);
G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_install)
FLATPAK_EXTERN FlatpakInstalledRef * flatpak_installation_install (FlatpakInstallation    *self,
                                                                   const char             *remote_name,
                                                                   FlatpakRefKind          kind,
                                                                   const char             *name,
                                                                   const char             *arch,
                                                                   const char             *branch,
                                                                   FlatpakProgressCallback progress,
                                                                   gpointer                progress_data,
                                                                   GCancellable           *cancellable,
                                                                   GError                **error);
G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_install)
FLATPAK_EXTERN FlatpakInstalledRef * flatpak_installation_install_full (FlatpakInstallation    *self,
                                                                        FlatpakInstallFlags     flags,
                                                                        const char             *remote_name,
                                                                        FlatpakRefKind          kind,
                                                                        const char             *name,
                                                                        const char             *arch,
                                                                        const char             *branch,
                                                                        const char * const     *subpaths,
                                                                        FlatpakProgressCallback progress,
                                                                        gpointer                progress_data,
                                                                        GCancellable           *cancellable,
                                                                        GError                **error);
G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_update)
FLATPAK_EXTERN FlatpakInstalledRef * flatpak_installation_update (FlatpakInstallation    *self,
                                                                  FlatpakUpdateFlags      flags,
                                                                  FlatpakRefKind          kind,
                                                                  const char             *name,
                                                                  const char             *arch,
                                                                  const char             *branch,
                                                                  FlatpakProgressCallback progress,
                                                                  gpointer                progress_data,
                                                                  GCancellable           *cancellable,
                                                                  GError                **error);
G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_update)
FLATPAK_EXTERN FlatpakInstalledRef * flatpak_installation_update_full (FlatpakInstallation    *self,
                                                                       FlatpakUpdateFlags      flags,
                                                                       FlatpakRefKind          kind,
                                                                       const char             *name,
                                                                       const char             *arch,
                                                                       const char             *branch,
                                                                       const char * const     *subpaths,
                                                                       FlatpakProgressCallback progress,
                                                                       gpointer                progress_data,
                                                                       GCancellable           *cancellable,
                                                                       GError                **error);
G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_install_bundle)
FLATPAK_EXTERN FlatpakInstalledRef * flatpak_installation_install_bundle (FlatpakInstallation    *self,
                                                                          GFile                  *file,
                                                                          FlatpakProgressCallback progress,
                                                                          gpointer                progress_data,
                                                                          GCancellable           *cancellable,
                                                                          GError                **error);
G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_install_flatpakref)
FLATPAK_EXTERN FlatpakRemoteRef *   flatpak_installation_install_ref_file (FlatpakInstallation *self,
                                                                           GBytes              *ref_file_data,
                                                                           GCancellable        *cancellable,
                                                                           GError             **error);
G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_uninstall)
FLATPAK_EXTERN gboolean             flatpak_installation_uninstall (FlatpakInstallation    *self,
                                                                    FlatpakRefKind          kind,
                                                                    const char             *name,
                                                                    const char             *arch,
                                                                    const char             *branch,
                                                                    FlatpakProgressCallback progress,
                                                                    gpointer                progress_data,
                                                                    GCancellable           *cancellable,
                                                                    GError                **error);

G_GNUC_DEPRECATED_FOR(flatpak_transaction_add_uninstall)
FLATPAK_EXTERN gboolean             flatpak_installation_uninstall_full (FlatpakInstallation    *self,
                                                                         FlatpakUninstallFlags   flags,
                                                                         FlatpakRefKind          kind,
                                                                         const char             *name,
                                                                         const char             *arch,
                                                                         const char             *branch,
                                                                         FlatpakProgressCallback progress,
                                                                         gpointer                progress_data,
                                                                         GCancellable           *cancellable,
                                                                         GError                **error);

FLATPAK_EXTERN gboolean          flatpak_installation_fetch_remote_size_sync (FlatpakInstallation *self,
                                                                              const char          *remote_name,
                                                                              FlatpakRef          *ref,
                                                                              guint64             *download_size,
                                                                              guint64             *installed_size,
                                                                              GCancellable        *cancellable,
                                                                              GError             **error);
FLATPAK_EXTERN GBytes        *   flatpak_installation_fetch_remote_metadata_sync (FlatpakInstallation *self,
                                                                                  const char          *remote_name,
                                                                                  FlatpakRef          *ref,
                                                                                  GCancellable        *cancellable,
                                                                                  GError             **error);
FLATPAK_EXTERN GPtrArray    *    flatpak_installation_list_remote_refs_sync (FlatpakInstallation *self,
                                                                             const char          *remote_or_uri,
                                                                             GCancellable        *cancellable,
                                                                             GError             **error);
FLATPAK_EXTERN GPtrArray    *    flatpak_installation_list_remote_refs_sync_full (FlatpakInstallation *self,
                                                                                  const char          *remote_or_uri,
                                                                                  FlatpakQueryFlags    flags,
                                                                                  GCancellable        *cancellable,
                                                                                  GError             **error);
FLATPAK_EXTERN FlatpakRemoteRef  *flatpak_installation_fetch_remote_ref_sync (FlatpakInstallation *self,
                                                                              const char          *remote_name,
                                                                              FlatpakRefKind       kind,
                                                                              const char          *name,
                                                                              const char          *arch,
                                                                              const char          *branch,
                                                                              GCancellable        *cancellable,
                                                                              GError             **error);
FLATPAK_EXTERN FlatpakRemoteRef  *flatpak_installation_fetch_remote_ref_sync_full (FlatpakInstallation *self,
                                                                                   const char          *remote_name,
                                                                                   FlatpakRefKind       kind,
                                                                                   const char          *name,
                                                                                   const char          *arch,
                                                                                   const char          *branch,
                                                                                   FlatpakQueryFlags    flags,
                                                                                   GCancellable        *cancellable,
                                                                                   GError             **error);
FLATPAK_EXTERN gboolean          flatpak_installation_update_appstream_sync (FlatpakInstallation *self,
                                                                             const char          *remote_name,
                                                                             const char          *arch,
                                                                             gboolean            *out_changed,
                                                                             GCancellable        *cancellable,
                                                                             GError             **error);
FLATPAK_EXTERN gboolean          flatpak_installation_update_appstream_full_sync (FlatpakInstallation    *self,
                                                                                  const char             *remote_name,
                                                                                  const char             *arch,
                                                                                  FlatpakProgressCallback progress,
                                                                                  gpointer                progress_data,
                                                                                  gboolean               *out_changed,
                                                                                  GCancellable           *cancellable,
                                                                                  GError                **error);
FLATPAK_EXTERN GPtrArray    *    flatpak_installation_list_remote_related_refs_sync (FlatpakInstallation *self,
                                                                                     const char          *remote_name,
                                                                                     const char          *ref,
                                                                                     GCancellable        *cancellable,
                                                                                     GError             **error);
FLATPAK_EXTERN GPtrArray    *    flatpak_installation_list_installed_related_refs_sync (FlatpakInstallation *self,
                                                                                        const char          *remote_name,
                                                                                        const char          *ref,
                                                                                        GCancellable        *cancellable,
                                                                                        GError             **error);

FLATPAK_EXTERN gboolean          flatpak_installation_remove_local_ref_sync (FlatpakInstallation *self,
                                                                             const char          *remote_name,
                                                                             const char          *ref,
                                                                             GCancellable        *cancellable,
                                                                             GError             **error);
FLATPAK_EXTERN gboolean          flatpak_installation_cleanup_local_refs_sync (FlatpakInstallation *self,
                                                                               GCancellable        *cancellable,
                                                                               GError             **error);
FLATPAK_EXTERN gboolean          flatpak_installation_prune_local_repo (FlatpakInstallation *self,
                                                                        GCancellable        *cancellable,
                                                                        GError             **error);
FLATPAK_EXTERN gboolean          flatpak_installation_run_triggers (FlatpakInstallation *self,
                                                                    GCancellable        *cancellable,
                                                                    GError             **error);


#endif /* __FLATPAK_INSTALLATION_H__ */
