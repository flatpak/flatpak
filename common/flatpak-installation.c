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

#include "config.h"

#include <glib/gi18n-lib.h>

#include <string.h>

#include <ostree.h>
#include <ostree-repo-finder-avahi.h>

#include "flatpak-dir-private.h"
#include "flatpak-enum-types.h"
#include "flatpak-error.h"
#include "flatpak-installation-private.h"
#include "flatpak-installation.h"
#include "flatpak-installed-ref-private.h"
#include "flatpak-instance-private.h"
#include "flatpak-related-ref-private.h"
#include "flatpak-progress-private.h"
#include "flatpak-remote-private.h"
#include "flatpak-remote-ref-private.h"
#include "flatpak-run-private.h"
#include "flatpak-transaction-private.h"
#include "flatpak-utils-private.h"

/**
 * SECTION:flatpak-installation
 * @Title: FlatpakInstallation
 * @Short_description: Installation information
 * @See_also: FlatpakTransaction
 *
 * FlatpakInstallation is the toplevel object that software installers
 * should use to operate on an flatpak applications.
 *
 * An FlatpakInstallation object provides information about an installation
 * location for flatpak applications. Typical installation locations are either
 * system-wide (in $prefix/var/lib/flatpak) or per-user (in ~/.local/share/flatpak).
 *
 * FlatpakInstallation can list configured remotes as well as installed application
 * and runtime references (in short: refs), and it can add, remove and modify remotes.
 *
 * FlatpakInstallation can also run, install, update and uninstall applications and
 * runtimes, but #FlatpakTransaction is a better, high-level API for these tasks.
 *
 * To get a list of all configured installations, use flatpak_get_system_installations(),
 * together with flatpak_installation_new_user().
 *
 * The FlatpakInstallation API is threadsafe in the sense that it is safe to run two
 * operations at the same time, in different threads (or processes).
 */

typedef struct _FlatpakInstallationPrivate FlatpakInstallationPrivate;

G_LOCK_DEFINE_STATIC (dir);

struct _FlatpakInstallationPrivate
{
  /* All raw access to this should be protected by the dir lock. The FlatpakDir object is mostly
     threadsafe (apart from pull transactions being a singleton on it), however we replace it during
     flatpak_installation_drop_caches(), so every user needs to keep its own reference alive until
     done. */
  FlatpakDir *dir_unlocked;
  char       *display_name;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakInstallation, flatpak_installation, G_TYPE_OBJECT)

enum {
  PROP_0,
};

static void
flatpak_installation_finalize (GObject *object)
{
  FlatpakInstallation *self = FLATPAK_INSTALLATION (object);
  FlatpakInstallationPrivate *priv = flatpak_installation_get_instance_private (self);

  g_object_unref (priv->dir_unlocked);
  g_free (priv->display_name);

  G_OBJECT_CLASS (flatpak_installation_parent_class)->finalize (object);
}

static void
flatpak_installation_class_init (FlatpakInstallationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_installation_finalize;

  /* Avoid weird recursive type initialization deadlocks from libsoup */
  g_type_ensure (G_TYPE_SOCKET);
}

static void
flatpak_installation_init (FlatpakInstallation *self)
{
}

static FlatpakInstallation *
flatpak_installation_new_steal_dir (FlatpakDir   *dir,
                                    GCancellable *cancellable,
                                    GError      **error)
{
  FlatpakInstallation *self;
  FlatpakInstallationPrivate *priv;

  if (!flatpak_dir_maybe_ensure_repo (dir, NULL, error))
    {
      g_object_unref (dir);
      return NULL;
    }

  self = g_object_new (FLATPAK_TYPE_INSTALLATION, NULL);
  priv = flatpak_installation_get_instance_private (self);

  priv->dir_unlocked = dir;

  return self;
}

FlatpakInstallation *
flatpak_installation_new_for_dir (FlatpakDir   *dir,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  return flatpak_installation_new_steal_dir (g_object_ref (dir),
                                             cancellable,
                                             error);
}

/**
 * flatpak_installation_set_no_interaction:
 * @self: a #FlatpakInstallation
 * @no_interaction: Whether to disallow interactive authorization for operations
 *
 * This method can be used to prevent interactive authorization dialogs to appear
 * for operations on @self. This is useful for background operations that are not
 * directly triggered by a user action.
 *
 * By default, interaction is allowed.
 *
 * Since: 1.1.1
 */
void
flatpak_installation_set_no_interaction (FlatpakInstallation *self,
                                         gboolean             no_interaction)
{
  FlatpakInstallationPrivate *priv = flatpak_installation_get_instance_private (self);

  flatpak_dir_set_no_interaction (priv->dir_unlocked, no_interaction);
}

/**
 * flatpak_installation_get_no_interaction:
 * @self: a #FlatpakTransaction
 *
 * Returns the value set with flatpak_installation_set_no_interaction().
 *
 * Returns: %TRUE if interactive authorization dialogs are not allowed
 *
 * Since: 1.1.1
 */
gboolean
flatpak_installation_get_no_interaction (FlatpakInstallation *self)
{
  FlatpakInstallationPrivate *priv = flatpak_installation_get_instance_private (self);

  return flatpak_dir_get_no_interaction (priv->dir_unlocked);
}

/**
 * flatpak_get_default_arch:
 *
 * Returns the canonical name for the arch of the current machine.
 *
 * Returns: an arch string
 */
const char  *
flatpak_get_default_arch (void)
{
  return flatpak_get_arch ();
}

/**
 * flatpak_get_supported_arches:
 *
 * Returns the canonical names for the arches that are supported (i.e. can run)
 * on the current machine, in order of priority (default is first).
 *
 * Returns: a zero terminated array of arch strings
 */
const char * const *
flatpak_get_supported_arches (void)
{
  return (const char * const *) flatpak_get_arches ();
}

/**
 * flatpak_get_system_installations:
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the system installations according to the current configuration and current
 * availability (e.g. doesn't return a configured installation if not reachable).
 *
 * Returns: (transfer container) (element-type FlatpakInstallation): a GPtrArray of
 *   #FlatpakInstallation instances
 *
 * Since: 0.8
 */
GPtrArray *
flatpak_get_system_installations (GCancellable *cancellable,
                                  GError      **error)
{
  g_autoptr(GPtrArray) system_dirs = NULL;
  g_autoptr(GPtrArray) installs = NULL;
  GPtrArray *ret = NULL;
  int i;

  system_dirs = flatpak_dir_get_system_list (cancellable, error);
  if (system_dirs == NULL)
    goto out;

  installs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  for (i = 0; i < system_dirs->len; i++)
    {
      g_autoptr(GError) local_error = NULL;
      FlatpakDir *install_dir = g_ptr_array_index (system_dirs, i);
      g_autoptr(FlatpakInstallation) installation = NULL;

      installation = flatpak_installation_new_for_dir (install_dir,
                                                       cancellable,
                                                       &local_error);
      if (installation != NULL)
        g_ptr_array_add (installs, g_steal_pointer (&installation));
      else
        {
          /* Warn about the problem and continue without listing this installation. */
          g_autofree char *dir_name = flatpak_dir_get_name (install_dir);
          g_warning ("Unable to create FlatpakInstallation for %s: %s",
                     dir_name, local_error->message);
        }
    }

  if (installs->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No system installations found");
    }

  ret = g_steal_pointer (&installs);

out:
  return ret;
}

/**
 * flatpak_installation_new_system:
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #FlatpakInstallation for the default system-wide installation.
 *
 * Returns: (transfer full): a new #FlatpakInstallation
 */
FlatpakInstallation *
flatpak_installation_new_system (GCancellable *cancellable,
                                 GError      **error)
{
  return flatpak_installation_new_steal_dir (flatpak_dir_get_system_default (), cancellable, error);
}

/**
 * flatpak_installation_new_system_with_id:
 * @id: (nullable): the ID of the system-wide installation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #FlatpakInstallation for the system-wide installation @id.
 *
 * Returns: (transfer full): a new #FlatpakInstallation
 *
 * Since: 0.8
 */
FlatpakInstallation *
flatpak_installation_new_system_with_id (const char   *id,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  g_autoptr(FlatpakDir) install_dir = NULL;
  g_autoptr(FlatpakInstallation) installation = NULL;
  g_autoptr(GError) local_error = NULL;

  install_dir = flatpak_dir_get_system_by_id (id, cancellable, error);
  if (install_dir == NULL)
    return NULL;

  installation = flatpak_installation_new_for_dir (install_dir,
                                                   cancellable,
                                                   &local_error);
  if (installation == NULL)
    {
      g_debug ("Error creating Flatpak installation: %s", local_error->message);
      g_propagate_error (error, g_steal_pointer (&local_error));
    }

  g_debug ("Found Flatpak installation for '%s'", id);
  return g_steal_pointer (&installation);
}

/**
 * flatpak_installation_new_user:
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #FlatpakInstallation for the per-user installation.
 *
 * Returns: (transfer full): a new #FlatpakInstallation
 */
FlatpakInstallation *
flatpak_installation_new_user (GCancellable *cancellable,
                               GError      **error)
{
  return flatpak_installation_new_steal_dir (flatpak_dir_get_user (), cancellable, error);
}

/**
 * flatpak_installation_new_for_path:
 * @path: a #GFile
 * @user: whether this is a user-specific location
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #FlatpakInstallation for the installation at the given @path.
 *
 * Returns: (transfer full): a new #FlatpakInstallation
 */
FlatpakInstallation *
flatpak_installation_new_for_path (GFile *path, gboolean user,
                                   GCancellable *cancellable,
                                   GError **error)
{
  return flatpak_installation_new_steal_dir (flatpak_dir_new (path, user), cancellable, error);
}

static FlatpakDir *
_flatpak_installation_get_dir (FlatpakInstallation *self, gboolean ensure_repo, GError **error)
{
  FlatpakInstallationPrivate *priv = flatpak_installation_get_instance_private (self);
  FlatpakDir *dir;

  G_LOCK (dir);

  if (ensure_repo && flatpak_dir_get_repo (priv->dir_unlocked) == NULL)
    {
      if (!flatpak_dir_ensure_repo (priv->dir_unlocked, NULL, error))
        {
          dir = NULL;
          goto out;
        }
    }

  dir = g_object_ref (priv->dir_unlocked);

out:
  G_UNLOCK (dir);
  return dir;
}

FlatpakDir *
flatpak_installation_get_dir (FlatpakInstallation *self, GError **error)
{
  return _flatpak_installation_get_dir (self, TRUE, error);
}

static FlatpakDir *
flatpak_installation_get_dir_maybe_no_repo (FlatpakInstallation *self)
{
  return _flatpak_installation_get_dir (self, FALSE, NULL);
}

FlatpakDir *
flatpak_installation_clone_dir_noensure (FlatpakInstallation *self)
{
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir_maybe_no_repo (self);

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);

  return g_steal_pointer (&dir_clone);
}

FlatpakDir *
flatpak_installation_clone_dir (FlatpakInstallation *self,
                                GCancellable        *cancellable,
                                GError             **error)
{
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  return g_steal_pointer (&dir_clone);
}

/**
 * flatpak_installation_drop_caches:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Drops all internal (in-memory) caches. For instance, this may be needed to pick up new or changed
 * remotes configured outside this installation instance.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
flatpak_installation_drop_caches (FlatpakInstallation *self,
                                  GCancellable        *cancellable,
                                  GError             **error)
{
  FlatpakInstallationPrivate *priv = flatpak_installation_get_instance_private (self);
  FlatpakDir *clone, *old;
  gboolean res = FALSE;

  G_LOCK (dir);

  old = priv->dir_unlocked;
  clone = flatpak_dir_clone (priv->dir_unlocked);

  if (flatpak_dir_maybe_ensure_repo (clone, cancellable, error))
    {
      priv->dir_unlocked = clone;
      g_object_unref (old);
      res = TRUE;
    }

  G_UNLOCK (dir);

  return res;
}

/**
 * flatpak_installation_get_is_user:
 * @self: a #FlatpakInstallation
 *
 * Returns whether the installation is for a user-specific location.
 *
 * Returns: %TRUE if @self is a per-user installation
 */
gboolean
flatpak_installation_get_is_user (FlatpakInstallation *self)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);

  return flatpak_dir_is_user (dir);
}

/**
 * flatpak_installation_get_path:
 * @self: a #FlatpakInstallation
 *
 * Returns the installation location for @self.
 *
 * Returns: (transfer full): an #GFile
 */
GFile *
flatpak_installation_get_path (FlatpakInstallation *self)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);

  return g_object_ref (flatpak_dir_get_path (dir));
}

/**
 * flatpak_installation_get_id:
 * @self: a #FlatpakInstallation
 *
 * Returns the ID of the installation for @self.
 *
 * The ID for the default system installation is "default".
 * The ID for the user installation is "user".
 *
 * Returns: (transfer none): a string with the installation's ID
 *
 * Since: 0.8
 */
const char *
flatpak_installation_get_id (FlatpakInstallation *self)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);

  return flatpak_dir_get_id (dir);
}

/**
 * flatpak_installation_get_display_name:
 * @self: a #FlatpakInstallation
 *
 * Returns the display name of the installation for @self.
 *
 * Note that this function may return %NULL if the installation
 * does not have a display name.
 *
 * Returns: (transfer none): a string with the installation's display name
 *
 * Since: 0.8
 */
const char *
flatpak_installation_get_display_name (FlatpakInstallation *self)
{
  FlatpakInstallationPrivate *priv = flatpak_installation_get_instance_private (self);
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);

  if (priv->display_name == NULL)
    priv->display_name = flatpak_dir_get_display_name (dir);

  return (const char *) priv->display_name;
}

/**
 * flatpak_installation_get_priority:
 * @self: a #FlatpakInstallation
 *
 * Returns the numeric priority of the installation for @self.
 *
 * Returns: an integer with the configured priority value
 *
 * Since: 0.8
 */
gint
flatpak_installation_get_priority (FlatpakInstallation *self)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);

  return flatpak_dir_get_priority (dir);
}

/**
 * flatpak_installation_get_storage_type:
 * @self: a #FlatpakInstallation
 *
 * Returns the type of storage of the installation for @self.
 *
 * Returns: a #FlatpakStorageType
 *
 * Since: 0.8
 */FlatpakStorageType
flatpak_installation_get_storage_type (FlatpakInstallation *self)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);

  switch (flatpak_dir_get_storage_type (dir))
    {
    case FLATPAK_DIR_STORAGE_TYPE_HARD_DISK:
      return FLATPAK_STORAGE_TYPE_HARD_DISK;

    case FLATPAK_DIR_STORAGE_TYPE_SDCARD:
      return FLATPAK_STORAGE_TYPE_SDCARD;

    case FLATPAK_DIR_STORAGE_TYPE_MMC:
      return FLATPAK_STORAGE_TYPE_MMC;

    case FLATPAK_DIR_STORAGE_TYPE_NETWORK:
      return FLATPAK_STORAGE_TYPE_NETWORK;

    default:
      return FLATPAK_STORAGE_TYPE_DEFAULT;
    }

  return FLATPAK_STORAGE_TYPE_DEFAULT;
}

/**
 * flatpak_installation_launch:
 * @self: a #FlatpakInstallation
 * @name: name of the app to launch
 * @arch: (nullable): which architecture to launch (default: current architecture)
 * @branch: (nullable): which branch of the application (default: "master")
 * @commit: (nullable): the commit of @branch to launch
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Launch an installed application.
 *
 * You can use flatpak_installation_get_installed_ref() or
 * flatpak_installation_get_current_installed_app() to find out what builds
 * are available, in order to get a value for @commit.
 *
 * Returns: %TRUE, unless an error occurred
 */
gboolean
flatpak_installation_launch (FlatpakInstallation *self,
                             const char          *name,
                             const char          *arch,
                             const char          *branch,
                             const char          *commit,
                             GCancellable        *cancellable,
                             GError             **error)
{
  return flatpak_installation_launch_full (self,
                                           FLATPAK_LAUNCH_FLAGS_NONE,
                                           name, arch, branch, commit,
                                           NULL,
                                           cancellable, error);
}

/**
 * flatpak_installation_launch_full:
 * @self: a #FlatpakInstallation
 * @flags: set of #FlatpakLaunchFlags
 * @name: name of the app to launch
 * @arch: (nullable): which architecture to launch (default: current architecture)
 * @branch: (nullable): which branch of the application (default: "master")
 * @commit: (nullable): the commit of @branch to launch
 * @instance_out: (nullable): return location for a #FlatpakInstance
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Launch an installed application.
 *
 * You can use flatpak_installation_get_installed_ref() or
 * flatpak_installation_get_current_installed_app() to find out what builds
 * are available, in order to get a value for @commit.
 *
 * Compared to flatpak_installation_launch(), this function returns a #FlatpakInstance
 * that can be used to get information about the running instance. You can also use
 * it to wait for the instance to be done with g_child_watch_add() if you pass the
 * #FLATPAK_LAUNCH_FLAGS_DO_NOT_REAP flag.
 *
 * Returns: %TRUE, unless an error occurred
 *
 * Since: 1.1
 */
gboolean
flatpak_installation_launch_full (FlatpakInstallation *self,
                                  FlatpakLaunchFlags   flags,
                                  const char          *name,
                                  const char          *arch,
                                  const char          *branch,
                                  const char          *commit,
                                  FlatpakInstance    **instance_out,
                                  GCancellable        *cancellable,
                                  GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDeploy) app_deploy = NULL;
  g_autofree char *app_ref = NULL;
  g_autofree char *instance_dir = NULL;
  FlatpakRunFlags run_flags;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  app_ref =
    flatpak_build_app_ref (name, branch, arch);

  app_deploy =
    flatpak_dir_load_deployed (dir, app_ref,
                               commit,
                               cancellable, error);
  if (app_deploy == NULL)
    return FALSE;

  run_flags = FLATPAK_RUN_FLAG_BACKGROUND;
  if (flags & FLATPAK_LAUNCH_FLAGS_DO_NOT_REAP)
    run_flags |= FLATPAK_RUN_FLAG_DO_NOT_REAP;

  if (!flatpak_run_app (app_ref,
                        app_deploy,
                        NULL, NULL,
                        NULL, NULL,
                        0,
                        run_flags,
                        NULL,
                        NULL,
                        NULL, 0, -1,
                        &instance_dir,
                        cancellable, error))
    return FALSE;

  if (instance_out)
    *instance_out = flatpak_instance_new (instance_dir);

  return TRUE;
}


static FlatpakInstalledRef *
get_ref (FlatpakDir   *dir,
         const char   *full_ref,
         GCancellable *cancellable,
         GError      **error)
{
  g_auto(GStrv) parts = NULL;
  const char *origin = NULL;
  const char *commit = NULL;
  const char *alt_id = NULL;
  g_autofree char *latest_alt_id = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) deploy_subdir = NULL;
  g_autofree char *deploy_path = NULL;
  g_autofree char *latest_commit = NULL;
  g_autofree char *deploy_subdirname = NULL;
  g_autoptr(GBytes) deploy_data = NULL;
  g_autofree const char **subpaths = NULL;
  g_autofree char *collection_id = NULL;
  gboolean is_current = FALSE;
  guint64 installed_size = 0;

  parts = g_strsplit (full_ref, "/", -1);

  deploy_data = flatpak_dir_get_deploy_data (dir, full_ref, FLATPAK_DEPLOY_VERSION_CURRENT, cancellable, error);
  if (deploy_data == NULL)
    return NULL;
  origin = flatpak_deploy_data_get_origin (deploy_data);
  commit = flatpak_deploy_data_get_commit (deploy_data);
  alt_id = flatpak_deploy_data_get_alt_id (deploy_data);
  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
  installed_size = flatpak_deploy_data_get_installed_size (deploy_data);

  deploy_dir = flatpak_dir_get_deploy_dir (dir, full_ref);
  deploy_subdirname = flatpak_dir_get_deploy_subdir (dir, commit, subpaths);
  deploy_subdir = g_file_get_child (deploy_dir, deploy_subdirname);
  deploy_path = g_file_get_path (deploy_subdir);

  if (strcmp (parts[0], "app") == 0)
    {
      g_autofree char *current =
        flatpak_dir_current_ref (dir, parts[1], cancellable);
      if (current && strcmp (full_ref, current) == 0)
        is_current = TRUE;
    }

  latest_commit = flatpak_dir_read_latest (dir, origin, full_ref, &latest_alt_id, NULL, NULL);

  collection_id = flatpak_dir_get_remote_collection_id (dir, origin);

  return flatpak_installed_ref_new (full_ref,
                                    alt_id ? alt_id : commit,
                                    latest_alt_id ? latest_alt_id : latest_commit,
                                    origin, collection_id, subpaths,
                                    deploy_path,
                                    installed_size,
                                    is_current,
                                    flatpak_deploy_data_get_eol (deploy_data),
                                    flatpak_deploy_data_get_eol_rebase (deploy_data),
                                    flatpak_deploy_data_get_appdata_name (deploy_data),
                                    flatpak_deploy_data_get_appdata_summary (deploy_data),
                                    flatpak_deploy_data_get_appdata_version (deploy_data),
                                    flatpak_deploy_data_get_appdata_license (deploy_data),
                                    flatpak_deploy_data_get_appdata_content_rating_type (deploy_data),
                                    flatpak_deploy_data_get_appdata_content_rating (deploy_data));
}

/**
 * flatpak_installation_get_installed_ref:
 * @self: a #FlatpakInstallation
 * @kind: whether this is an app or runtime
 * @name: name of the app/runtime to fetch
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 * @branch: (nullable): which branch to fetch (default: "master")
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Returns information about an installed ref, such as the available builds,
 * its size, location, etc.
 *
 * Returns: (transfer full): an #FlatpakInstalledRef, or %NULL if an error occurred
 */
FlatpakInstalledRef *
flatpak_installation_get_installed_ref (FlatpakInstallation *self,
                                        FlatpakRefKind       kind,
                                        const char          *name,
                                        const char          *arch,
                                        const char          *branch,
                                        GCancellable        *cancellable,
                                        GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *ref = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  if (kind == FLATPAK_REF_KIND_APP)
    ref = flatpak_build_app_ref (name, branch, arch);
  else
    ref = flatpak_build_runtime_ref (name, branch, arch);


  deploy = flatpak_dir_get_if_deployed (dir,
                                        ref, NULL, cancellable);
  if (deploy == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED,
                          _("Ref %s not installed"), ref);
      return NULL;
    }

  return get_ref (dir, ref, cancellable, error);
}

/**
 * flatpak_installation_get_current_installed_app:
 * @self: a #FlatpakInstallation
 * @name: the name of the app
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Get the last build of reference @name that was installed with
 * flatpak_installation_install(), or %NULL if the reference has
 * never been installed locally.
 *
 * Returns: (transfer full): an #FlatpakInstalledRef
 */
FlatpakInstalledRef *
flatpak_installation_get_current_installed_app (FlatpakInstallation *self,
                                                const char          *name,
                                                GCancellable        *cancellable,
                                                GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *current = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  current = flatpak_dir_current_ref (dir, name, cancellable);
  if (current)
    deploy = flatpak_dir_get_if_deployed (dir,
                                          current, NULL, cancellable);

  if (deploy == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED,
                          _("App %s not installed"), name);
      return NULL;
    }

  return get_ref (dir, current, cancellable, error);
}

/**
 * flatpak_installation_list_installed_refs:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references.
 *
 * Returns: (transfer container) (element-type FlatpakInstalledRef): a GPtrArray of
 *   #FlatpakInstalledRef instances
 */
GPtrArray *
flatpak_installation_list_installed_refs (FlatpakInstallation *self,
                                          GCancellable        *cancellable,
                                          GError             **error)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);
  g_auto(GStrv) raw_refs_app = NULL;
  g_auto(GStrv) raw_refs_runtime = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  if (!flatpak_dir_list_refs (dir,
                              "app",
                              &raw_refs_app,
                              cancellable, error))
    return NULL;

  for (i = 0; raw_refs_app[i] != NULL; i++)
    {
      g_autoptr(GError) local_error = NULL;
      FlatpakInstalledRef *ref = get_ref (dir, raw_refs_app[i], cancellable, &local_error);
      if (ref != NULL)
        g_ptr_array_add (refs, ref);
      else
        g_warning ("Unexpected failure getting ref for %s: %s", raw_refs_app[i], local_error->message);
    }

  if (!flatpak_dir_list_refs (dir,
                              "runtime",
                              &raw_refs_runtime,
                              cancellable, error))
    return NULL;

  for (i = 0; raw_refs_runtime[i] != NULL; i++)
    {
      g_autoptr(GError) local_error = NULL;
      FlatpakInstalledRef *ref = get_ref (dir, raw_refs_runtime[i], cancellable, &local_error);
      if (ref != NULL)
        g_ptr_array_add (refs, ref);
      else
        g_warning ("Unexpected failure getting ref for %s: %s", raw_refs_runtime[i], local_error->message);
    }

  return g_steal_pointer (&refs);
}

/**
 * flatpak_installation_list_installed_refs_by_kind:
 * @self: a #FlatpakInstallation
 * @kind: the kind of installation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references of a specific kind.
 *
 * Returns: (transfer container) (element-type FlatpakInstalledRef): a GPtrArray of
 *   #FlatpakInstalledRef instances
 */
GPtrArray *
flatpak_installation_list_installed_refs_by_kind (FlatpakInstallation *self,
                                                  FlatpakRefKind       kind,
                                                  GCancellable        *cancellable,
                                                  GError             **error)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);
  g_auto(GStrv) raw_refs = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  if (!flatpak_dir_list_refs (dir,
                              kind == FLATPAK_REF_KIND_APP ? "app" : "runtime",
                              &raw_refs,
                              cancellable, error))
    return NULL;

  for (i = 0; raw_refs[i] != NULL; i++)
    {
      g_autoptr(GError) local_error = NULL;
      FlatpakInstalledRef *ref = get_ref (dir, raw_refs[i], cancellable, &local_error);
      if (ref != NULL)
        g_ptr_array_add (refs, ref);
      else
        g_warning ("Unexpected failure getting ref for %s: %s", raw_refs[i], local_error->message);
    }

  return g_steal_pointer (&refs);
}

static gboolean
transaction_ready (FlatpakTransaction  *transaction,
                   GHashTable         **related_to_ops)
{
  GList *ops = flatpak_transaction_get_operations (transaction);

  for (GList *l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      GPtrArray *op_related_to_ops = flatpak_transaction_operation_get_related_to_ops (op);  /* (element-type FlatpakTransactionOperation) */
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);

      /* There is currently no way for a set of updates to lead to an
       * uninstall, but check anyway.
       */
      if (type == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        {
          const char *ref = flatpak_transaction_operation_get_ref (op);
          g_warning ("Update transaction unexpectedly wants to uninstall %s", ref);
          continue;
        }

      g_hash_table_insert (*related_to_ops,
                           g_object_ref (op),
                           op_related_to_ops ? g_ptr_array_ref (op_related_to_ops) : NULL);
    }

  g_list_free_full (ops, g_object_unref);

  /* Abort the transaction; we only wanted to know what it would do */
  return FALSE;
}

static gint
installed_ref_compare (gconstpointer _iref_a,
                       gconstpointer _iref_b)
{
  const FlatpakInstalledRef *iref_a = *(const FlatpakInstalledRef **)_iref_a;
  const FlatpakInstalledRef *iref_b = *(const FlatpakInstalledRef **)_iref_b;
  g_autofree char *ref_a = flatpak_ref_format_ref (FLATPAK_REF (iref_a));
  g_autofree char *ref_b = flatpak_ref_format_ref (FLATPAK_REF (iref_b));

  return strcmp (ref_a, ref_b);
}

/**
 * flatpak_installation_list_installed_refs_for_update:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed apps and runtimes that have an update available, either
 * from the configured remote or locally available but not deployed (see
 * flatpak_transaction_set_no_deploy()).
 *
 * This also checks if any of #FlatpakInstalledRef has a missing #FlatpakRelatedRef
 * (which has `should-download` set to %TRUE) or runtime. If so, it adds the
 * ref to the returning #GPtrArray to pull in the #FlatpakRelatedRef or runtime
 * again via an update operation in #FlatpakTransaction.
 *
 * In case more than one app needs an update of the same runtime or extension,
 * this function will return all of those apps.
 *
 * Returns: (transfer container) (element-type FlatpakInstalledRef): a GPtrArray of
 *   #FlatpakInstalledRef instances, or %NULL on error
 */
GPtrArray *
flatpak_installation_list_installed_refs_for_update (FlatpakInstallation *self,
                                                     GCancellable        *cancellable,
                                                     GError             **error)
{
  g_autoptr(GPtrArray) installed_refs = NULL; /* (element-type FlatpakInstalledRef) */
  g_autoptr(GHashTable) installed_refs_hash = NULL; /* (element-type utf8 FlatpakInstalledRef) */
  g_autoptr(GPtrArray) installed_refs_for_update = NULL; /* (element-type FlatpakInstalledRef) */
  g_autoptr(GHashTable) installed_refs_for_update_set = NULL; /* (element-type utf8) */
  g_autoptr(GHashTable) related_to_ops = NULL; /* (element-type FlatpakTransactionOperation GPtrArray<FlatpakTransactionOperation>) */
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) local_error = NULL;

  installed_refs = flatpak_installation_list_installed_refs (self, cancellable, error);
  if (installed_refs == NULL)
    return NULL;

  /* Here we use a FlatpakTransaction to determine what needs updating, and
   * abort it before actually doing the updates. This ensures we are consistent
   * with the CLI update command.
   */
  transaction = flatpak_transaction_new_for_installation (self, cancellable, error);
  if (transaction == NULL)
    return NULL;

  installed_refs_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < installed_refs->len; i++)
    {
      FlatpakInstalledRef *installed_ref = g_ptr_array_index (installed_refs, i);
      g_autofree char *ref = flatpak_ref_format_ref (FLATPAK_REF (installed_ref));

      /* This hash table will be used later for efficient search */
      g_hash_table_insert (installed_refs_hash, g_strdup (ref), installed_ref);

      if (flatpak_transaction_add_update (transaction, ref, NULL, NULL, &local_error))
        continue;

      if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND))
        {
          g_debug ("%s: Unable to update %s: %s", G_STRFUNC, ref, local_error->message);
          g_clear_error (&local_error);
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }
    }

  related_to_ops = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, null_safe_g_ptr_array_unref);

  g_signal_connect (transaction, "ready", G_CALLBACK (transaction_ready), &related_to_ops);

  flatpak_transaction_run (transaction, cancellable, &local_error);
  g_assert (local_error != NULL);
  if (!g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  g_clear_error (&local_error);

  installed_refs_for_update = g_ptr_array_new_with_free_func (g_object_unref);
  installed_refs_for_update_set = g_hash_table_new (g_str_hash, g_str_equal);

  /* For each ref that would be affected by the transaction, if it is
   * installed, add it to the list to be returned and otherwise add the ref
   * that caused it be added. We need to cover all of the following cases:
   * 1. For an app or runtime that has an update available, add it to the list
   *    (including a locale extension which needs more subpaths downloaded).
   * 2. For an app or extension that has a missing runtime, add the
   *    app/extension to the list.
   * 3. For an app that's missing a "should-download" related ref, add the app
   *    to the list.
   */
  GLNX_HASH_TABLE_FOREACH_KV (related_to_ops,
                              FlatpakTransactionOperation *, op,
                              GPtrArray *, op_related_to_ops)
    {
      const char *op_ref = flatpak_transaction_operation_get_ref (op);
      FlatpakInstalledRef *installed_ref;

      /* Here we use the existing installed_refs_hash instead of get_ref()
       * since staying in memory should be more efficient than disk I/O
       */
      installed_ref = g_hash_table_lookup (installed_refs_hash, op_ref);
      if (installed_ref != NULL)
        {
          if (!g_hash_table_contains (installed_refs_for_update_set, op_ref))
            {
              g_hash_table_add (installed_refs_for_update_set, (char *)op_ref);
              g_debug ("%s: Installed ref %s needs update", G_STRFUNC, op_ref);
              g_ptr_array_add (installed_refs_for_update,
                               g_object_ref (installed_ref));
            }
        }
      else
        {
          for (gsize i = 0; op_related_to_ops != NULL && i < op_related_to_ops->len; i++)
            {
              FlatpakTransactionOperation *related_to_op = g_ptr_array_index (op_related_to_ops, i);

              const char *related_op_ref = flatpak_transaction_operation_get_ref (related_to_op);
              if (!g_hash_table_contains (installed_refs_for_update_set, related_op_ref))
                {
                  installed_ref = g_hash_table_lookup (installed_refs_hash, related_op_ref);
                  if (installed_ref != NULL)
                    {
                      g_hash_table_add (installed_refs_for_update_set, (char *)related_op_ref);
                      g_debug ("%s: Installed ref %s needs update", G_STRFUNC, related_op_ref);
                      g_ptr_array_add (installed_refs_for_update,
                                       g_object_ref (installed_ref));
                    }
                }
            }
        }

      /* Note: installed_ref could be NULL if for example op is installing a
       * related ref of a missing runtime.
       */
    }

  /* Remove non-determinism for the sake of the unit tests */
  g_ptr_array_sort (installed_refs_for_update, installed_ref_compare);

  return g_steal_pointer (&installed_refs_for_update);
}

/* Find all USB and LAN repositories which share the same collection ID as
 * @remote_name, and add a #FlatpakRemote to @remotes for each of them. The caller
 * must initialise @remotes. Returns %TRUE without modifying @remotes if the
 * given remote doesn’t have a collection ID configured or if the @dir doesn’t
 * have a repo.
 *
 * FIXME: If this were async, the parallelisation could be handled in the caller. */

/**
 * flatpak_installation_list_remotes_by_type:
 * @self: a #FlatpakInstallation
 * @types: (array length=num_types): an array of #FlatpakRemoteType
 * @num_types: the number of types provided in @types
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists only the remotes whose type is included in the @types argument.
 *
 * Since flatpak 1.7 this will never return any types except FLATPAK_REMOTE_TYPE_STATIC.
 * Equivalent functionallity to FLATPAK_REMOTE_TYPE_USB can be had by listing remote refs
 * with FLATPAK_QUERY_FLAGS_ONLY_SIDELOADED.
 *
 * Returns: (transfer container) (element-type FlatpakRemote): a GPtrArray of
 *   #FlatpakRemote instances
 */
GPtrArray *
flatpak_installation_list_remotes_by_type (FlatpakInstallation     *self,
                                           const FlatpakRemoteType *types,
                                           gsize                    num_types,
                                           GCancellable            *cancellable,
                                           GError                 **error)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_auto(GStrv) remote_names = NULL;
  g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func (g_object_unref);
  const guint NUM_FLATPAK_REMOTE_TYPES = 3;
  gboolean types_filter[NUM_FLATPAK_REMOTE_TYPES];
  gsize i;

  remote_names = flatpak_dir_list_remotes (dir, cancellable, error);
  if (remote_names == NULL)
    return NULL;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_maybe_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  /* If NULL or an empty array of types is passed then we list all types */
  for (i = 0; i < NUM_FLATPAK_REMOTE_TYPES; ++i)
    {
      if (types != NULL && num_types != 0)
        types_filter[i] = FALSE;
      else
        types_filter[i] = TRUE;
    }

  for (i = 0; i < num_types; ++i)
    {
      g_return_val_if_fail (types[i] < NUM_FLATPAK_REMOTE_TYPES, NULL);
      types_filter[types[i]] = TRUE;
    }

  for (i = 0; remote_names[i] != NULL; ++i)
    {
      /* These days we only support static remotes */
      if (types_filter[FLATPAK_REMOTE_TYPE_STATIC])
        g_ptr_array_add (remotes, flatpak_remote_new_with_dir (remote_names[i],
                                                               dir_clone));
    }

  return g_steal_pointer (&remotes);
}

/**
 * flatpak_installation_list_remotes:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the static remotes, in priority (highest first) order. For same
 * priority, an earlier added remote comes before a later added one.
 *
 * Returns: (transfer container) (element-type FlatpakRemote): a GPtrArray of
 *   #FlatpakRemote instances
 */
GPtrArray *
flatpak_installation_list_remotes (FlatpakInstallation *self,
                                   GCancellable        *cancellable,
                                   GError             **error)
{
  const FlatpakRemoteType types[] = { FLATPAK_REMOTE_TYPE_STATIC };

  return flatpak_installation_list_remotes_by_type (self, types, 1, cancellable, error);
}

/**
 * flatpak_installation_modify_remote:
 * @self: a #FlatpakInstallation
 * @remote: the modified #FlatpakRemote
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Saves changes in the @remote object.
 *
 * Returns: %TRUE if the modifications have been committed successfully
 */
gboolean
flatpak_installation_modify_remote (FlatpakInstallation *self,
                                    FlatpakRemote       *remote,
                                    GCancellable        *cancellable,
                                    GError             **error)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);
  g_autoptr(FlatpakDir) dir_clone = NULL;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_maybe_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  if (!flatpak_remote_commit (remote, dir_clone, cancellable, error))
    return FALSE;

  /* Make sure we pick up the new config */
  flatpak_installation_drop_caches (self, NULL, NULL);

  return TRUE;
}


/**
 * flatpak_installation_add_remote:
 * @self: a #FlatpakInstallation
 * @remote: the new #FlatpakRemote
 * @if_needed: if %TRUE, only add if it doesn't exists
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Adds a new @remote object to the set of remotes. This is similar
 * to flatpak_installation_modify_remote() for non-existing remote
 * names. However, if the named remote already exists then instead of
 * modifying it it fails with %FLATPAK_ERROR_ALREADY_INSTALLED, or if
 * @if_needed is true it silently succeeds without doing anything.
 *
 * As an exception to the last, if the local config has a filter defined,
 * but the new remote unsets the filter (for example, it comes from an
 * unfiltered .flatpakref via flatpak_remote_new_from_file()) the the local
 * remote filter gets reset. This is to allow the setup where there is a
 * default setup of a filtered remote, yet you can still use the standard
 * flatpakref file to get the full contents without getting two remotes.
 *
 * Returns: %TRUE if the modifications have been committed successfully
 * Since: 1.3.4
 */
gboolean
flatpak_installation_add_remote (FlatpakInstallation *self,
                                 FlatpakRemote       *remote,
                                 gboolean             if_needed,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);
  g_autoptr(FlatpakDir) dir_clone = NULL;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_maybe_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  if (flatpak_dir_has_remote (dir, flatpak_remote_get_name (remote), NULL))
    {
      if (!if_needed)
        return flatpak_fail_error (error, FLATPAK_ERROR_ALREADY_INSTALLED, _("Remote '%s' already exists"), flatpak_remote_get_name (remote));

      if (!flatpak_remote_commit_filter (remote, dir_clone, cancellable, error))
        return FALSE;

      return TRUE;
    }

  if (!flatpak_remote_commit (remote, dir_clone, cancellable, error))
    return FALSE;

  /* Make sure we pick up the new config */
  flatpak_installation_drop_caches (self, NULL, NULL);

  return TRUE;
}


/**
 * flatpak_installation_remove_remote:
 * @self: a #FlatpakInstallation
 * @name: the name of the remote to remove
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Removes the remote with the given name from the installation.
 *
 * Returns: %TRUE if the remote has been removed successfully
 */
gboolean
flatpak_installation_remove_remote (FlatpakInstallation *self,
                                    const char          *name,
                                    GCancellable        *cancellable,
                                    GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  if (!flatpak_dir_remove_remote (dir, FALSE, name,
                                  cancellable, error))
    return FALSE;

  /* Make sure we pick up the new config */
  flatpak_installation_drop_caches (self, NULL, NULL);

  return TRUE;
}

/**
 * flatpak_installation_set_config_sync:
 * @self: a #FlatpakInstallation
 * @key: the name of the key to set
 * @value: the new value, or %NULL to unset
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Set a global configuration option for the installation, currently
 * the only supported keys are `languages`, which is a semicolon-separated
 * list of language codes like `"sv;en;pl"`, or `""` to mean all languages,
 * and `extra-languages`, which is a semicolon-separated list of locale
 * identifiers like `"en;en_DK;zh_HK.big5hkscs;uz_UZ.utf8@cyrillic"`.
 *
 * Returns: %TRUE if the option was set correctly
 */
gboolean
flatpak_installation_set_config_sync (FlatpakInstallation *self,
                                      const char          *key,
                                      const char          *value,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  if (!flatpak_dir_set_config (dir, key, value, error))
    return FALSE;

  /* Make sure we pick up the new config */
  flatpak_installation_drop_caches (self, NULL, NULL);

  return TRUE;
}

/**
 * flatpak_installation_get_config:
 * @self: a #FlatpakInstallation
 * @key: the name of the key to get
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Get a global configuration option for the installation, see
 * flatpak_installation_set_config_sync() for supported keys.
 *
 * Returns: The (newly allocated) value, or %NULL on error (%G_KEY_FILE_ERROR_KEY_NOT_FOUND error if key is not set)
 */
char *
flatpak_installation_get_config (FlatpakInstallation *self,
                                 const char          *key,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  return flatpak_dir_get_config (dir, key, error);
}

/**
 * flatpak_installation_get_default_languages:
 * @self: a #FlatpakInstallation
 * @error: return location for a #GError
 *
 * Get the default languages used by the installation to decide which
 * subpaths to install of locale extensions. This list may also be used
 * by frontends like GNOME Software to decide which language-specific apps
 * to display. An empty array means that all languages should be installed.
 *
 * Returns: (array zero-terminated=1) (element-type utf8) (transfer full):
 *   A possibly empty array of strings, or %NULL on error.
 * Since: 1.5.0
 */
char **
flatpak_installation_get_default_languages (FlatpakInstallation  *self,
                                            GError              **error)
{
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  return flatpak_dir_get_locale_languages (dir);
}

/**
 * flatpak_installation_get_default_locales:
 * @self: a #FlatpakInstallation
 * @error: return location for a #GError
 *
 * Like flatpak_installation_get_default_languages() but includes territory
 * information (e.g. `en_US` rather than `en`) which may be included in the
 * `extra-languages` configuration.
 *
 * Strings returned by this function are in the format specified by
 * [`setlocale()`](man:setlocale): `language[_territory][.codeset][@modifier]`.
 *
 * Returns: (array zero-terminated=1) (element-type utf8) (transfer full):
 *   A possibly empty array of locale strings, or %NULL on error.
 * Since: 1.5.1
 */
char **
flatpak_installation_get_default_locales (FlatpakInstallation  *self,
                                          GError              **error)
{
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  return flatpak_dir_get_locales (dir);
}

/**
 * flatpak_installation_get_min_free_space_bytes:
 * @self: a #FlatpakInstallation
 * @out_bytes: (out): Location to store the result
 * @error: Return location for a #GError
 *
 * Returns the min-free-space config value from the OSTree repository of this installation.
 *
 * Applications can use this value, together with information about the available
 * disk space and the size of pending updates or installs, to estimate whether a
 * pull operation will fail due to running out of disk space.
 *
 * Returns: %TRUE on success, or %FALSE on error.
 * Since: 1.1
 */
gboolean
flatpak_installation_get_min_free_space_bytes (FlatpakInstallation *self,
                                               guint64             *out_bytes,
                                               GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;

  dir = flatpak_installation_get_dir (self, NULL);
  if (dir == NULL)
    return FALSE;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, NULL, error))
    return FALSE;

  return ostree_repo_get_min_free_space_bytes (flatpak_dir_get_repo (dir_clone), out_bytes, error);
}

/**
 * flatpak_installation_update_remote_sync:
 * @self: a #FlatpakInstallation
 * @name: the name of the remote to update
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Updates the local configuration of a remote repository by fetching
 * the related information from the summary file in the remote OSTree
 * repository and committing the changes to the local installation.
 *
 * Returns: %TRUE if the remote has been updated successfully
 *
 * Since: 0.6.13
 */
gboolean
flatpak_installation_update_remote_sync (FlatpakInstallation *self,
                                         const char          *name,
                                         GCancellable        *cancellable,
                                         GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  if (!flatpak_dir_update_remote_configuration (dir, name, NULL, NULL, cancellable, error))
    return FALSE;

  /* Make sure we pick up the new config */
  flatpak_installation_drop_caches (self, NULL, NULL);

  return TRUE;
}

/**
 * flatpak_installation_get_remote_by_name:
 * @self: a #FlatpakInstallation
 * @name: a remote name
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Looks up a remote by name.
 *
 * Returns: (transfer full): a #FlatpakRemote instance, or %NULL with @error
 *   set
 */
FlatpakRemote *
flatpak_installation_get_remote_by_name (FlatpakInstallation *self,
                                         const gchar         *name,
                                         GCancellable        *cancellable,
                                         GError             **error)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);
  g_autoptr(FlatpakDir) dir_clone = NULL;

  if (!flatpak_dir_has_remote (dir, name, error))
    return NULL;

  /* We clone the dir here to make sure we re-read the latest ostree repo config, in case
     it has local changes */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  return flatpak_remote_new_with_dir (name, dir_clone);
}

/**
 * flatpak_installation_load_app_overrides:
 * @self: a #FlatpakInstallation
 * @app_id: an application id
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Loads the metadata overrides file for an application.
 *
 * Returns: (transfer full): the contents of the overrides files,
 *    or %NULL if an error occurred
 */
char *
flatpak_installation_load_app_overrides (FlatpakInstallation *self,
                                         const char          *app_id,
                                         GCancellable        *cancellable,
                                         GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  char *metadata_contents;
  gsize metadata_size;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  metadata_contents = flatpak_dir_load_override (dir, app_id, &metadata_size, error);
  if (metadata_contents == NULL)
    return NULL;

  return metadata_contents;
}

/**
 * flatpak_installation_install_bundle:
 * @self: a #FlatpakInstallation
 * @file: a #GFile that is an flatpak bundle
 * @progress: (scope call) (nullable): progress callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_install_bundle()
 * instead. It has a lot more interesting features.
 *
 * Install an application or runtime from an flatpak bundle file.
 * See flatpak-build-bundle(1) for how to create bundles.
 *
 * Returns: (transfer full): The ref for the newly installed app or %NULL on failure
 * Deprecated: 1.7.0: Use flatpak_transaction_add_install_bundle() instead.
 */
FlatpakInstalledRef *
flatpak_installation_install_bundle (FlatpakInstallation    *self,
                                     GFile                  *file,
                                     FlatpakProgressCallback progress,
                                     gpointer                progress_data,
                                     GCancellable           *cancellable,
                                     GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *remote = NULL;
  FlatpakInstalledRef *result = NULL;
  gboolean created_remote;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  remote = flatpak_dir_ensure_bundle_remote (dir, file, NULL, &ref, NULL, NULL, &created_remote, cancellable, error);
  if (remote == NULL)
    return NULL;

  /* Make sure we pick up the new config */
  if (created_remote)
    flatpak_installation_drop_caches (self, NULL, NULL);

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  if (!flatpak_dir_install_bundle (dir_clone, file, remote, NULL,
                                   cancellable, error))
    return NULL;

  if (g_str_has_prefix (ref, "app"))
    flatpak_dir_run_triggers (dir_clone, cancellable, NULL);

  result = get_ref (dir, ref, cancellable, error);
  if (result == NULL)
    return NULL;

  return result;
}

/**
 * flatpak_installation_install_ref_file:
 * @self: a #FlatpakInstallation
 * @ref_file_data: The ref file contents
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_install_flatpakref()
 * instead. It has a lot more interesting features.
 *
 * Creates a remote based on the passed in .flatpakref file contents
 * in @ref_file_data and returns the #FlatpakRemoteRef that can be used
 * to install it.
 *
 * Note, the #FlatpakRemoteRef will not have the commit field set, or other details, to
 * avoid unnecessary roundtrips. If you need that you have to resolve it
 * explicitly with flatpak_installation_fetch_remote_ref_sync ().
 *
 * Returns: (transfer full): a #FlatpakRemoteRef if the remote has been added successfully, %NULL
 * on error.
 *
 * Since: 0.6.10
 * Deprecated: 1.7.0: Use flatpak_transaction_add_install_flatpakref() instead.
 */
FlatpakRemoteRef *
flatpak_installation_install_ref_file (FlatpakInstallation *self,
                                       GBytes              *ref_file_data,
                                       GCancellable        *cancellable,
                                       GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autofree char *remote = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *collection_id = NULL;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  if (!g_key_file_load_from_data (keyfile, g_bytes_get_data (ref_file_data, NULL),
                                  g_bytes_get_size (ref_file_data),
                                  0, error))
    return FALSE;

  if (!flatpak_dir_create_remote_for_ref_file (dir, keyfile, NULL, &remote, &collection_id, &ref, error))
    return NULL;

  if (!flatpak_installation_drop_caches (self, cancellable, error))
    return NULL;

  return flatpak_remote_ref_new (ref, NULL, remote, collection_id, NULL);
}

/**
 * flatpak_installation_install_full:
 * @self: a #FlatpakInstallation
 * @flags: set of #FlatpakInstallFlags flag
 * @remote_name: name of the remote to use
 * @kind: what this ref contains (an #FlatpakRefKind)
 * @name: name of the app/runtime to fetch
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 * @branch: (nullable): which branch to fetch (default: 'master')
 * @subpaths: (nullable) (array zero-terminated=1): A list of subpaths to fetch, or %NULL for everything
 * @progress: (scope call) (nullable): progress callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_install()
 * instead. It has a lot more interesting features.
 *
 * Install a new application or runtime.
 *
 * Note that this function was originally written to always return a
 * #FlatpakInstalledRef. Since 0.9.13, passing
 * FLATPAK_INSTALL_FLAGS_NO_DEPLOY will only pull refs into the local flatpak
 * repository without deploying them, however this function will
 * be unable to provide information on the installed ref, so
 * FLATPAK_ERROR_ONLY_PULLED will be set and the caller must respond
 * accordingly.
 *
 * Returns: (transfer full): The ref for the newly installed app or %NULL on failure
 * Deprecated: 1.7.0: Use flatpak_transaction_add_install() instead.
 */
FlatpakInstalledRef *
flatpak_installation_install_full (FlatpakInstallation    *self,
                                   FlatpakInstallFlags     flags,
                                   const char             *remote_name,
                                   FlatpakRefKind          kind,
                                   const char             *name,
                                   const char             *arch,
                                   const char             *branch,
                                   const char * const     *subpaths,
                                   FlatpakProgressCallback progress_cb,
                                   gpointer                progress_data,
                                   GCancellable           *cancellable,
                                   GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autofree char *ref = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(FlatpakProgress) progress = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  ref = flatpak_compose_ref (kind == FLATPAK_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
  if (deploy_dir != NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_ALREADY_INSTALLED,
                          _("%s branch %s already installed"), name, branch ? branch : "master");
      return NULL;
    }

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, FALSE, cancellable, error);
  if (state == NULL)
    return NULL;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  if (progress_cb)
      progress = flatpak_progress_new (progress_cb, progress_data);

  if (!flatpak_dir_install (dir_clone,
                            (flags & FLATPAK_INSTALL_FLAGS_NO_PULL) != 0,
                            (flags & FLATPAK_INSTALL_FLAGS_NO_DEPLOY) != 0,
                            (flags & FLATPAK_INSTALL_FLAGS_NO_STATIC_DELTAS) != 0,
                            FALSE, FALSE, state,
                            ref, NULL, (const char **) subpaths, NULL, NULL, NULL, NULL,
                            progress, cancellable, error))
    return NULL;

  if (!(flags & FLATPAK_INSTALL_FLAGS_NO_TRIGGERS) &&
      g_str_has_prefix (ref, "app"))
    flatpak_dir_run_triggers (dir_clone, cancellable, NULL);

  /* Note that if the caller sets FLATPAK_INSTALL_FLAGS_NO_DEPLOY we must
   * always return an error, as explained above. Otherwise get_ref will
   * always return an error. */
  if ((flags & FLATPAK_INSTALL_FLAGS_NO_DEPLOY) != 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_ONLY_PULLED,
                          _("As requested, %s was only pulled, but not installed"), name);
      return NULL;
    }

  return get_ref (dir, ref, cancellable, error);
}

/**
 * flatpak_installation_install:
 * @self: a #FlatpakInstallation
 * @remote_name: name of the remote to use
 * @kind: what this ref contains (an #FlatpakRefKind)
 * @name: name of the app/runtime to fetch
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 * @branch: (nullable): which branch to fetch (default: 'master')
 * @progress: (scope call) (nullable): progress callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_install()
 * instead. It has a lot more interesting features.
 *
 * Install a new application or runtime.
 *
 * Note that this function was originally written to always return a
 * #FlatpakInstalledRef. Since 0.9.13, passing
 * FLATPAK_INSTALL_FLAGS_NO_DEPLOY will only pull refs into the local flatpak
 * repository without deploying them, however this function will
 * be unable to provide information on the installed ref, so
 * FLATPAK_ERROR_ONLY_PULLED will be set and the caller must respond
 * accordingly.
 *
 * Returns: (transfer full): The ref for the newly installed app or %NULL on failure
 * Deprecated: 1.7.0: Use flatpak_transaction_add_install() instead.
 */
FlatpakInstalledRef *
flatpak_installation_install (FlatpakInstallation    *self,
                              const char             *remote_name,
                              FlatpakRefKind          kind,
                              const char             *name,
                              const char             *arch,
                              const char             *branch,
                              FlatpakProgressCallback progress,
                              gpointer                progress_data,
                              GCancellable           *cancellable,
                              GError                **error)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  return flatpak_installation_install_full (self, FLATPAK_INSTALL_FLAGS_NONE,
                                            remote_name, kind, name, arch, branch,
                                            NULL, progress, progress_data,
                                            cancellable, error);
  G_GNUC_END_IGNORE_DEPRECATIONS
}

/**
 * flatpak_installation_update_full:
 * @self: a #FlatpakInstallation
 * @flags: set of #FlatpakUpdateFlags flag
 * @kind: whether this is an app or runtime
 * @name: name of the app or runtime to update
 * @arch: (nullable): architecture of the app or runtime to update (default: current architecture)
 * @branch: (nullable): name of the branch of the app or runtime to update (default: master)
 * @subpaths: (nullable) (array zero-terminated=1): A list of subpaths to fetch, or %NULL for everything
 * @progress: (scope call) (nullable): the callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_update()
 * instead. It has a lot more interesting features.
 *
 * Update an application or runtime.
 *
 * If the specified package is not installed, then %FLATPAK_ERROR_NOT_INSTALLED
 * will be thrown.
 *
 * If no updates could be found on the remote end and the package is
 * already up to date, then %FLATPAK_ERROR_ALREADY_INSTALLED will be thrown.
 *
 * Returns: (transfer full): The ref for the newly updated app or %NULL on failure
 * Deprecated: 1.7.0: Use flatpak_transaction_add_update() instead.
 */
FlatpakInstalledRef *
flatpak_installation_update_full (FlatpakInstallation    *self,
                                  FlatpakUpdateFlags      flags,
                                  FlatpakRefKind          kind,
                                  const char             *name,
                                  const char             *arch,
                                  const char             *branch,
                                  const char * const     *subpaths,
                                  FlatpakProgressCallback progress_cb,
                                  gpointer                progress_data,
                                  GCancellable           *cancellable,
                                  GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autofree char *ref = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(FlatpakProgress) progress = NULL;
  g_autofree char *remote_name = NULL;
  FlatpakInstalledRef *result = NULL;
  g_autofree char *target_commit = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  ref = flatpak_compose_ref (kind == FLATPAK_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
  if (deploy_dir == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED,
                          _("%s branch %s is not installed"), name, branch ? branch : "master");
      return NULL;
    }

  remote_name = flatpak_dir_get_origin (dir, ref, cancellable, error);
  if (remote_name == NULL)
    return NULL;

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, FALSE, cancellable, error);
  if (state == NULL)
    return NULL;

  target_commit = flatpak_dir_check_for_update (dir, state, ref, NULL,
                                                (const char **) subpaths,
                                                (flags & FLATPAK_UPDATE_FLAGS_NO_PULL) != 0,
                                                cancellable, error);
  if (target_commit == NULL)
    return NULL;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  if (progress_cb)
    progress = flatpak_progress_new (progress_cb, progress_data);

  if (!flatpak_dir_update (dir_clone,
                           (flags & FLATPAK_UPDATE_FLAGS_NO_PULL) != 0,
                           (flags & FLATPAK_UPDATE_FLAGS_NO_DEPLOY) != 0,
                           (flags & FLATPAK_UPDATE_FLAGS_NO_STATIC_DELTAS) != 0,
                           FALSE, FALSE, FALSE, state,
                           ref, target_commit,
                           (const char **) subpaths, NULL, NULL, NULL, NULL,
                           progress, cancellable, error))
    return NULL;

  if (!(flags & FLATPAK_UPDATE_FLAGS_NO_TRIGGERS) &&
      g_str_has_prefix (ref, "app"))
    flatpak_dir_run_triggers (dir_clone, cancellable, NULL);

  result = get_ref (dir, ref, cancellable, error);
  if (result == NULL)
    return NULL;

  /* We don't get prunable objects if not pulling or if NO_PRUNE is passed */
  if (!(flags & FLATPAK_UPDATE_FLAGS_NO_PULL) && !(flags & FLATPAK_UPDATE_FLAGS_NO_PRUNE))
    flatpak_dir_prune (dir_clone, cancellable, NULL);

  return result;
}

/**
 * flatpak_installation_update:
 * @self: a #FlatpakInstallation
 * @flags: set of #FlatpakUpdateFlags flag
 * @kind: whether this is an app or runtime
 * @name: name of the app or runtime to update
 * @arch: (nullable): architecture of the app or runtime to update (default: current architecture)
 * @branch: (nullable): name of the branch of the app or runtime to update (default: master)
 * @progress: (scope call) (nullable): the callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_update()
 * instead. It has a lot more interesting features.
 *
 * Update an application or runtime.
 *
 * If the specified package is not installed, then %FLATPAK_ERROR_NOT_INSTALLED
 * will be thrown.
 *
 * If no updates could be found on the remote end and the package is
 * already up to date, then %FLATPAK_ERROR_ALREADY_INSTALLED will be thrown.
 *
 * Returns: (transfer full): The ref for the newly updated app or %NULL on failure
 * Deprecated: 1.7.0: Use flatpak_transaction_add_update() instead.
 */
FlatpakInstalledRef *
flatpak_installation_update (FlatpakInstallation    *self,
                             FlatpakUpdateFlags      flags,
                             FlatpakRefKind          kind,
                             const char             *name,
                             const char             *arch,
                             const char             *branch,
                             FlatpakProgressCallback progress,
                             gpointer                progress_data,
                             GCancellable           *cancellable,
                             GError                **error)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  return flatpak_installation_update_full (self, flags, kind, name, arch,
                                           branch, NULL, progress, progress_data,
                                           cancellable, error);
  G_GNUC_END_IGNORE_DEPRECATIONS
}

/**
 * flatpak_installation_uninstall:
 * @self: a #FlatpakInstallation
 * @kind: what this ref contains (an #FlatpakRefKind)
 * @name: name of the app or runtime to uninstall
 * @arch: (nullable): architecture of the app or runtime to uninstall; if
 *  %NULL, flatpak_get_default_arch() is assumed
 * @branch: (nullable): name of the branch of the app or runtime to uninstall;
 *  if %NULL, `master` is assumed
 * @progress: (scope call) (nullable): the callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_uninstall()
 * instead. It has a lot more interesting features.
 *
 * Uninstall an application or runtime.
 *
 * Returns: %TRUE on success
 * Deprecated: 1.7.0: Use flatpak_transaction_add_uninstall() instead.
 */
FLATPAK_EXTERN gboolean
flatpak_installation_uninstall (FlatpakInstallation    *self,
                                FlatpakRefKind          kind,
                                const char             *name,
                                const char             *arch,
                                const char             *branch,
                                FlatpakProgressCallback progress,
                                gpointer                progress_data,
                                GCancellable           *cancellable,
                                GError                **error)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  return flatpak_installation_uninstall_full (self, FLATPAK_UNINSTALL_FLAGS_NONE,
                                              kind, name, arch, branch,
                                              progress, progress_data,
                                              cancellable, error);
  G_GNUC_END_IGNORE_DEPRECATIONS
}

/**
 * flatpak_installation_uninstall_full:
 * @self: a #FlatpakInstallation
 * @flags: set of #FlatpakUninstallFlags flags
 * @kind: what this ref contains (an #FlatpakRefKind)
 * @name: name of the app or runtime to uninstall
 * @arch: (nullable): architecture of the app or runtime to uninstall; if
 *  %NULL, flatpak_get_default_arch() is assumed
 * @branch: (nullable): name of the branch of the app or runtime to uninstall;
 *  if %NULL, `master` is assumed
 * @progress: (scope call) (nullable): the callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * This is an old deprecated function, you should use
 * #FlatpakTransaction and flatpak_transaction_add_uninstall()
 * instead. It has a lot more interesting features.
 *
 * Uninstall an application or runtime.
 *
 * Returns: %TRUE on success
 *
 * Since: 0.11.8
 * Deprecated: 1.7.0: Use flatpak_transaction_add_uninstall() instead.
 */
gboolean
flatpak_installation_uninstall_full (FlatpakInstallation    *self,
                                     FlatpakUninstallFlags   flags,
                                     FlatpakRefKind          kind,
                                     const char             *name,
                                     const char             *arch,
                                     const char             *branch,
                                     FlatpakProgressCallback progress,
                                     gpointer                progress_data,
                                     GCancellable           *cancellable,
                                     GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autofree char *ref = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  ref = flatpak_compose_ref (kind == FLATPAK_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return FALSE;

  /* prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  if (!flatpak_dir_uninstall (dir_clone, ref, FLATPAK_HELPER_UNINSTALL_FLAGS_NONE,
                              cancellable, error))
    return FALSE;

  if (!(flags & FLATPAK_UNINSTALL_FLAGS_NO_TRIGGERS) &&
      g_str_has_prefix (ref, "app"))
    flatpak_dir_run_triggers (dir_clone, cancellable, NULL);

  if (!(flags & FLATPAK_UNINSTALL_FLAGS_NO_PRUNE))
    flatpak_dir_prune (dir_clone, cancellable, NULL);

  return TRUE;
}

/**
 * flatpak_installation_fetch_remote_size_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @ref: the ref
 * @download_size: (out): return location for the (maximum) download size
 * @installed_size: (out): return location for the installed size
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Gets information about the maximum amount of data that needs to be transferred
 * to pull the ref from a remote repository, and about the amount of
 * local disk space that is required to check out this commit.
 *
 * Note that if there are locally available data that are in the ref, which is common
 * for instance if you're doing an update then the real download size may be smaller
 * than what is returned here.
 *
 * NOTE: Since 0.11.4 this information is accessible in FlatpakRemoteRef, so this
 * function is not very useful anymore.
 *
 * Returns: %TRUE, unless an error occurred
 */
gboolean
flatpak_installation_fetch_remote_size_sync (FlatpakInstallation *self,
                                             const char          *remote_name,
                                             FlatpakRef          *ref,
                                             guint64             *download_size,
                                             guint64             *installed_size,
                                             GCancellable        *cancellable,
                                             GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autofree char *full_ref = flatpak_ref_format_ref (ref);

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, FALSE, cancellable, error);
  if (state == NULL)
    return FALSE;

  return flatpak_remote_state_load_data (state, full_ref,
                                         download_size, installed_size, NULL,
                                         error);
}

/**
 * flatpak_installation_fetch_remote_metadata_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @ref: the ref
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Obtains the metadata file from a commit.
 *
 * NOTE: Since 0.11.4 this information is accessible in FlatpakRemoteRef, so this
 * function is not very useful anymore.
 *
 * Returns: (transfer full): a #GBytes containing the flatpak metadata file,
 *   or %NULL if an error occurred
 */
GBytes *
flatpak_installation_fetch_remote_metadata_sync (FlatpakInstallation *self,
                                                 const char          *remote_name,
                                                 FlatpakRef          *ref,
                                                 GCancellable        *cancellable,
                                                 GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autofree char *full_ref = flatpak_ref_format_ref (ref);
  g_autofree char *res = NULL;
  gsize len;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, FALSE, cancellable, error);
  if (state == NULL)
    return FALSE;

  if (!flatpak_remote_state_load_data (state, full_ref,
                                       NULL, NULL, &res,
                                       error))
    return NULL;

  len = strlen (res);
  return g_bytes_new_take (g_steal_pointer (&res), len);
}

/**
 * flatpak_installation_list_remote_refs_sync:
 * @self: a #FlatpakInstallation
 * @remote_or_uri: the name or URI of the remote
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists all the applications and runtimes in a remote.
 *
 * Returns: (transfer container) (element-type FlatpakRemoteRef): a GPtrArray of
 *   #FlatpakRemoteRef instances
 */
GPtrArray *
flatpak_installation_list_remote_refs_sync (FlatpakInstallation *self,
                                            const char          *remote_or_uri,
                                            GCancellable        *cancellable,
                                            GError             **error)
{
  return flatpak_installation_list_remote_refs_sync_full (self, remote_or_uri, 0, cancellable, error);
}

/**
 * flatpak_installation_list_remote_refs_sync_full:
 * @self: a #FlatpakInstallation
 * @remote_or_uri: the name or URI of the remote
 * @flags: set of #FlatpakQueryFlags
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists all the applications and runtimes in a remote.
 *
 * Returns: (transfer container) (element-type FlatpakRemoteRef): a GPtrArray of
 *   #FlatpakRemoteRef instances
 *
 * Since: 1.3.3
 */
GPtrArray *
flatpak_installation_list_remote_refs_sync_full (FlatpakInstallation *self,
                                                 const char          *remote_or_uri,
                                                 FlatpakQueryFlags    flags,
                                                 GCancellable        *cancellable,
                                                 GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autoptr(GHashTable) ht = NULL;
  g_autoptr(GError) local_error = NULL;
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  if (flags & FLATPAK_QUERY_FLAGS_ONLY_SIDELOADED)
    state = flatpak_dir_get_remote_state_local_only (dir, remote_or_uri, cancellable, error);
  else
    state = flatpak_dir_get_remote_state (dir, remote_or_uri, (flags & FLATPAK_QUERY_FLAGS_ONLY_CACHED) != 0, cancellable, error);
  if (state == NULL)
    return NULL;

  if (!flatpak_dir_list_remote_refs (dir, state, &ht,
                                     cancellable, &local_error))
    {
      if (flags & FLATPAK_QUERY_FLAGS_ONLY_SIDELOADED)
        {
          /* Just return no sideloaded refs rather than a summary download failed error if there are none */
          return g_steal_pointer (&refs);
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }
    }

  g_hash_table_iter_init (&iter, ht);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *ref_name = key;
      const gchar *ref_commit = value;
      FlatpakRemoteRef *ref;

      ref = flatpak_remote_ref_new (ref_name, ref_commit, remote_or_uri, state->collection_id, state);

      if (ref)
        g_ptr_array_add (refs, ref);
    }

  return g_steal_pointer (&refs);
}

/**
 * flatpak_installation_fetch_remote_ref_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @kind: what this ref contains (an #FlatpakRefKind)
 * @name: name of the app/runtime to fetch
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 * @branch: (nullable): which branch to fetch (default: 'master')
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Gets the current remote branch of a ref in the remote.
 *
 * Returns: (transfer full): a #FlatpakRemoteRef instance, or %NULL
 */
FlatpakRemoteRef *
flatpak_installation_fetch_remote_ref_sync (FlatpakInstallation *self,
                                            const char          *remote_name,
                                            FlatpakRefKind       kind,
                                            const char          *name,
                                            const char          *arch,
                                            const char          *branch,
                                            GCancellable        *cancellable,
                                            GError             **error)
{
  return flatpak_installation_fetch_remote_ref_sync_full (self, remote_name,
                                                          kind, name, arch, branch, 0,
                                                          cancellable, error);
}

/**
 * flatpak_installation_fetch_remote_ref_sync_full:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @kind: what this ref contains (an #FlatpakRefKind)
 * @name: name of the app/runtime to fetch
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 * @branch: (nullable): which branch to fetch (default: 'master')
 * @flags: set of #FlatpakQueryFlags
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Gets the current remote branch of a ref in the remote.
 *
 * Returns: (transfer full): a #FlatpakRemoteRef instance, or %NULL
 *
 * Since: 1.3.3
 */
FlatpakRemoteRef *
flatpak_installation_fetch_remote_ref_sync_full (FlatpakInstallation *self,
                                                 const char          *remote_name,
                                                 FlatpakRefKind       kind,
                                                 const char          *name,
                                                 const char          *arch,
                                                 const char          *branch,
                                                 FlatpakQueryFlags    flags,
                                                 GCancellable        *cancellable,
                                                 GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GHashTable) ht = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autofree char *ref = NULL;
  const char *checksum;

  if (branch == NULL)
    branch = "master";

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  if (flags & FLATPAK_QUERY_FLAGS_ONLY_SIDELOADED)
    state = flatpak_dir_get_remote_state_local_only (dir, remote_name, cancellable, error);
  else
    state = flatpak_dir_get_remote_state (dir, remote_name, (flags & FLATPAK_QUERY_FLAGS_ONLY_CACHED) != 0, cancellable, error);
  if (state == NULL)
    return NULL;

  if (!flatpak_dir_list_remote_refs (dir, state, &ht,
                                     cancellable, error))
    return NULL;

  if (kind == FLATPAK_REF_KIND_APP)
    ref = flatpak_build_app_ref (name,
                                 branch,
                                 arch);
  else
    ref = flatpak_build_runtime_ref (name,
                                     branch,
                                     arch);

  checksum = g_hash_table_lookup (ht, ref);

  if (checksum != NULL)
    return flatpak_remote_ref_new (ref, checksum, remote_name, state->collection_id, state);

  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_REF_NOT_FOUND,
               "Reference %s doesn't exist in remote", ref);
  return NULL;
}

/**
 * flatpak_installation_update_appstream_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @arch: (nullable): Architecture to update, or %NULL for the local machine arch
 * @out_changed: (nullable): Set to %TRUE if the contents of the appstream changed, %FALSE if nothing changed
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Updates the local copy of appstream for @remote_name for the specified @arch.
 * If you need progress feedback, use flatpak_installation_update_appstream_full_sync().
 *
 * Returns: %TRUE on success, or %FALSE on error
 */
gboolean
flatpak_installation_update_appstream_sync (FlatpakInstallation *self,
                                            const char          *remote_name,
                                            const char          *arch,
                                            gboolean            *out_changed,
                                            GCancellable        *cancellable,
                                            GError             **error)
{
  return flatpak_installation_update_appstream_full_sync (self, remote_name, arch,
                                                          NULL, NULL, out_changed,
                                                          cancellable, error);
}

/**
 * flatpak_installation_update_appstream_full_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @arch: (nullable): Architecture to update, or %NULL for the local machine arch
 * @progress: (scope call) (nullable): progress callback
 * @progress_data: (closure progress) (nullable): user data passed to @progress
 * @out_changed: (nullable): Set to %TRUE if the contents of the appstream changed, %FALSE if nothing changed
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Updates the local copy of appstream for @remote_name for the specified @arch.
 *
 * Returns: %TRUE on success, or %FALSE on error
 */
gboolean
flatpak_installation_update_appstream_full_sync (FlatpakInstallation    *self,
                                                 const char             *remote_name,
                                                 const char             *arch,
                                                 FlatpakProgressCallback progress_cb,
                                                 gpointer                progress_data,
                                                 gboolean               *out_changed,
                                                 GCancellable           *cancellable,
                                                 GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(FlatpakProgress) progress = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  if (progress_cb)
    progress = flatpak_progress_new (progress_cb, progress_data);

  return flatpak_dir_update_appstream (dir_clone,
                                       remote_name,
                                       arch,
                                       out_changed,
                                       progress,
                                       cancellable,
                                       error);
}


/**
 * flatpak_installation_create_monitor:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Gets monitor object for the installation. The returned file monitor will
 * emit the #GFileMonitor::changed signal whenever an application or runtime
 * was installed, uninstalled or updated.
 *
 * Returns: (transfer full): a new #GFileMonitor instance, or %NULL on error
 */
GFileMonitor *
flatpak_installation_create_monitor (FlatpakInstallation *self,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);
  g_autoptr(GFile) path = NULL;

  path = flatpak_dir_get_changed_path (dir);

  return g_file_monitor_file (path, G_FILE_MONITOR_NONE,
                              cancellable, error);
}


/**
 * flatpak_installation_list_remote_related_refs_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @ref: the ref
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists all the available refs on @remote_name that are related to
 * @ref, and the subpaths to use. These are things that are
 * interesting to install, update, or uninstall together with
 * @ref. For instance, locale data or debug information.
 *
 * The returned list contains all available related refs, but not
 * every one should always be installed. For example,
 * flatpak_related_ref_should_download() returns %TRUE if the
 * reference should be installed/updated with the app, and
 * flatpak_related_ref_should_delete() returns %TRUE if it
 * should be uninstalled with the main ref.
 *
 * The commit property of each #FlatpakRelatedRef is not guaranteed to be
 * non-%NULL.
 *
 * Returns: (transfer container) (element-type FlatpakRelatedRef): a GPtrArray of
 *   #FlatpakRelatedRef instances
 *
 * Since: 0.6.7
 */
GPtrArray *
flatpak_installation_list_remote_related_refs_sync (FlatpakInstallation *self,
                                                    const char          *remote_name,
                                                    const char          *ref,
                                                    GCancellable        *cancellable,
                                                    GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GPtrArray) related = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(FlatpakRemoteState) state = NULL;
  int i;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, FALSE, cancellable, error);
  if (state == NULL)
    return NULL;

  related = flatpak_dir_find_remote_related (dir, state, ref,
                                             cancellable, error);
  if (related == NULL)
    return NULL;

  for (i = 0; i < related->len; i++)
    {
      FlatpakRelated *rel = g_ptr_array_index (related, i);
      FlatpakRelatedRef *rel_ref;

      rel_ref = flatpak_related_ref_new (rel->ref, rel->commit,
                                         rel->subpaths, rel->download, rel->delete);

      if (rel_ref)
        g_ptr_array_add (refs, rel_ref);
    }

  return g_steal_pointer (&refs);
}

/**
 * flatpak_installation_list_installed_related_refs_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @ref: the ref
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists all the locally installed refs from @remote_name that are
 * related to @ref. These are things that are interesting to install,
 * update, or uninstall together with @ref. For instance, locale data
 * or debug information.
 *
 * This function is similar to flatpak_installation_list_remote_related_refs_sync,
 * but instead of looking at what is available on the remote, it only looks
 * at the locally installed refs. This is useful for instance when you're
 * looking for related refs to uninstall, or when you're planning to use
 * FLATPAK_UPDATE_FLAGS_NO_PULL to install previously pulled refs.
 *
 * Returns: (transfer container) (element-type FlatpakRelatedRef): a GPtrArray of
 *   #FlatpakRelatedRef instances
 *
 * Since: 0.6.7
 */
GPtrArray *
flatpak_installation_list_installed_related_refs_sync (FlatpakInstallation *self,
                                                       const char          *remote_name,
                                                       const char          *ref,
                                                       GCancellable        *cancellable,
                                                       GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GPtrArray) related = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  related = flatpak_dir_find_local_related (dir, ref, remote_name, TRUE,
                                            cancellable, error);
  if (related == NULL)
    return NULL;

  for (i = 0; i < related->len; i++)
    {
      FlatpakRelated *rel = g_ptr_array_index (related, i);
      FlatpakRelatedRef *rel_ref;

      rel_ref = flatpak_related_ref_new (rel->ref, rel->commit,
                                         rel->subpaths, rel->download, rel->delete);

      if (rel_ref)
        g_ptr_array_add (refs, rel_ref);
    }

  return g_steal_pointer (&refs);
}

/**
 * flatpak_installation_remove_local_ref_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @ref: the ref
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Remove the OSTree ref given by @remote_name:@ref from the local flatpak
 * repository. The next time the underlying OSTree repo is pruned, objects
 * which were attached to that ref will be removed. This is useful if you
 * pulled a flatpak ref using flatpak_installation_install_full() and
 * specified %FLATPAK_INSTALL_FLAGS_NO_DEPLOY but then decided not to
 * deploy the ref later on and want to remove the local ref to prevent it
 * from taking up disk space. Note that this will not remove the objects
 * referred to by @ref from the underlying OSTree repo, you should use
 * flatpak_installation_prune_local_repo() to do that.
 *
 * Since: 0.10.0
 * Returns: %TRUE on success
 */
gboolean
flatpak_installation_remove_local_ref_sync (FlatpakInstallation *self,
                                            const char          *remote_name,
                                            const char          *ref,
                                            GCancellable        *cancellable,
                                            GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  return flatpak_dir_remove_ref (dir, remote_name, ref, cancellable, error);
}

/**
 * flatpak_installation_cleanup_local_refs_sync:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Remove all OSTree refs from the local flatpak repository which are not
 * in a deployed state. The next time the underlying OSTree repo is pruned,
 * objects which were attached to that ref will be removed. This is useful if
 * you pulled a flatpak refs using flatpak_installation_install_full() and
 * specified %FLATPAK_INSTALL_FLAGS_NO_DEPLOY but then decided not to
 * deploy the refs later on and want to remove the local refs to prevent them
 * from taking up disk space. Note that this will not remove the objects
 * referred to by @ref from the underlying OSTree repo, you should use
 * flatpak_installation_prune_local_repo() to do that.
 *
 * Since: 0.10.0
 * Returns: %TRUE on success
 */
gboolean
flatpak_installation_cleanup_local_refs_sync (FlatpakInstallation *self,
                                              GCancellable        *cancellable,
                                              GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  return flatpak_dir_cleanup_undeployed_refs (dir, cancellable, error);
}

/**
 * flatpak_installation_prune_local_repo:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Remove all orphaned OSTree objects from the underlying OSTree repo in
 * @self.
 *
 * Since: 0.10.0
 * Returns: %TRUE on success
 */
gboolean
flatpak_installation_prune_local_repo (FlatpakInstallation *self,
                                       GCancellable        *cancellable,
                                       GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  return flatpak_dir_prune (dir, cancellable, error);
}

/**
 * flatpak_installation_run_triggers:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Run the trigger commands to update the files exported by the apps in
 * @self. Should be used after one or more app install, upgrade or
 * uninstall operations with the %FLATPAK_INSTALL_FLAGS_NO_TRIGGERS,
 * %FLATPAK_UPDATE_FLAGS_NO_TRIGGERS or %FLATPAK_UNINSTALL_FLAGS_NO_TRIGGERS
 * flags set.
 *
 * Since: 1.0.3
 * Returns: %TRUE on success
 */
gboolean
flatpak_installation_run_triggers (FlatpakInstallation *self,
                                   GCancellable        *cancellable,
                                   GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  return flatpak_dir_run_triggers (dir, cancellable, error);
}


static void
find_used_refs (FlatpakDir *dir,
                GHashTable *used_refs,
                const char *ref,
                const char *origin)
{
  g_autoptr(GPtrArray) related = NULL;
  int i;

  g_hash_table_add (used_refs, g_strdup (ref));

  related = flatpak_dir_find_local_related (dir, ref, origin, TRUE, NULL, NULL);
  if (related == NULL)
    return;

  for (i = 0; i < related->len; i++)
    {
      FlatpakRelated *rel = g_ptr_array_index (related, i);

      if (!rel->auto_prune && !g_hash_table_contains (used_refs, rel->ref))
        {
          g_autofree char *related_origin = NULL;

          g_hash_table_add (used_refs, g_strdup (rel->ref));

          related_origin = flatpak_dir_get_origin (dir, rel->ref, NULL, NULL);
          if (related_origin != NULL)
            find_used_refs (dir, used_refs, rel->ref, related_origin);
        }
    }
}

/**
 * flatpak_installation_list_unused_refs:
 * @self: a #FlatpakInstallation
 * @arch: (nullable): if non-%NULL, the architecture of refs to collect
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references that are not 'used'.
 *
 * A reference is used if it is either an application, or an sdk,
 * or the runtime of a used ref, or an extension of a used ref.
 * Pinned runtimes are also considered used; see flatpak-pin(1).
 *
 * Returns: (transfer container) (element-type FlatpakInstalledRef): a GPtrArray of
 *   #FlatpakInstalledRef instances
 *
 * Since: 1.1.2
 */
GPtrArray *
flatpak_installation_list_unused_refs (FlatpakInstallation *self,
                                       const char          *arch,
                                       GCancellable        *cancellable,
                                       GError             **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GHashTable) refs_hash = NULL;
  g_autoptr(GPtrArray) refs =  NULL;
  g_auto(GStrv) app_refs = NULL;
  g_auto(GStrv) runtime_refs = NULL;
  g_autoptr(GHashTable) used_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) used_runtimes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  int i;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  if (!flatpak_dir_list_refs (dir, "app", &app_refs, cancellable, error))
    return NULL;

  if (!flatpak_dir_list_refs (dir, "runtime", &runtime_refs, cancellable, error))
    return NULL;

  refs_hash = g_hash_table_new (g_str_hash, g_str_equal);
  refs = g_ptr_array_new_with_free_func (g_object_unref);

  for (i = 0; app_refs[i] != NULL; i++)
    {
      const char *ref = app_refs[i];
      g_autoptr(FlatpakDeploy) deploy = NULL;
      g_autofree char *origin = NULL;
      g_autofree char *runtime = NULL;
      g_autofree char *sdk = NULL;
      g_autoptr(GKeyFile) metakey = NULL;
      g_auto(GStrv) parts = g_strsplit (ref, "/", -1);

      if (arch != NULL && strcmp (parts[2], arch) != 0)
        continue;

      deploy = flatpak_dir_load_deployed (dir, ref, NULL, NULL, NULL);
      if (deploy == NULL)
        continue;

      origin = flatpak_dir_get_origin (dir, ref, NULL, NULL);
      if (origin == NULL)
        continue;

      find_used_refs (dir, used_refs, ref, origin);

      metakey = flatpak_deploy_get_metadata (deploy);
      runtime = g_key_file_get_string (metakey, "Application", "runtime", NULL);
      if (runtime)
        g_hash_table_add (used_runtimes, g_steal_pointer (&runtime));

      sdk = g_key_file_get_string (metakey, "Application", "sdk", NULL);
      if (sdk)
        g_hash_table_add (used_runtimes, g_steal_pointer (&sdk));
    }

  GLNX_HASH_TABLE_FOREACH (used_runtimes, const char *, runtime)
  {
    g_autofree char *runtime_ref = g_strconcat ("runtime/", runtime, NULL);
    g_autoptr(FlatpakDeploy) deploy = NULL;
    g_autofree char *origin = NULL;
    g_autofree char *sdk = NULL;
    g_autoptr(GKeyFile) metakey = NULL;

    deploy = flatpak_dir_load_deployed (dir, runtime_ref, NULL, NULL, NULL);
    if (deploy == NULL)
      continue;

    origin = flatpak_dir_get_origin (dir, runtime_ref, NULL, NULL);
    if (origin == NULL)
      continue;

    find_used_refs (dir, used_refs, runtime_ref, origin);

    metakey = flatpak_deploy_get_metadata (deploy);
    sdk = g_key_file_get_string (metakey, "Runtime", "sdk", NULL);
    if (sdk)
      {
        g_autofree char *sdk_ref = g_strconcat ("runtime/", sdk, NULL);
        g_autofree char *sdk_origin = flatpak_dir_get_origin (dir, sdk_ref, NULL, NULL);
        if (sdk_origin)
          find_used_refs (dir, used_refs, sdk_ref, sdk_origin);
      }
  }

  for (i = 0; runtime_refs[i] != NULL; i++)
    {
      const char *ref = runtime_refs[i];
      g_auto(GStrv) parts = g_strsplit (ref, "/", -1);

      if (arch != NULL && strcmp (parts[2], arch) != 0)
        continue;

      if (flatpak_dir_ref_is_pinned (dir, ref))
        {
          g_debug ("Ref %s is pinned, considering as used", ref);
          continue;
        }

      if (!g_hash_table_contains (used_refs, ref))
        {
          if (g_hash_table_add (refs_hash, (gpointer) ref))
            g_ptr_array_add (refs, get_ref (dir, ref, NULL, NULL));
        }
    }

  return g_steal_pointer (&refs);
}
