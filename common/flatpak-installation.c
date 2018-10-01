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

#include <string.h>

#include <ostree.h>
#include <ostree-repo-finder-avahi.h>

#include "flatpak-installation-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-installation.h"
#include "flatpak-installed-ref-private.h"
#include "flatpak-transaction-private.h"
#include "flatpak-related-ref-private.h"
#include "flatpak-remote-private.h"
#include "flatpak-remote-ref-private.h"
#include "flatpak-enum-types.h"
#include "flatpak-dir-private.h"
#include "flatpak-run-private.h"
#include "flatpak-error.h"

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
 * The FlatpakInstallatio nAPI is threadsafe in the sense that it is safe to run two
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
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakInstallation, flatpak_installation, G_TYPE_OBJECT)

enum {
  PROP_0,
};

static void
no_progress_cb (OstreeAsyncProgress *progress, gpointer user_data)
{
}

static void
flatpak_installation_finalize (GObject *object)
{
  FlatpakInstallation *self = FLATPAK_INSTALLATION (object);
  FlatpakInstallationPrivate *priv = flatpak_installation_get_instance_private (self);

  g_object_unref (priv->dir_unlocked);

  G_OBJECT_CLASS (flatpak_installation_parent_class)->finalize (object);
}

static void
flatpak_installation_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_installation_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_installation_class_init (FlatpakInstallationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_installation_get_property;
  object_class->set_property = flatpak_installation_set_property;
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
 * Creates a new #FlatpakInstallation for the system-wide installation.
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
  flatpak_migrate_from_xdg_app ();

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
  flatpak_migrate_from_xdg_app ();

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

static FlatpakDir *
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
 * Returns the ID of the system installation for @self.
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
 * Returns the display name of the system installation for @self.
 *
 * Returns: (transfer none): a string with the installation's display name
 *
 * Since: 0.8
 */
const char *
flatpak_installation_get_display_name (FlatpakInstallation *self)
{
  g_autoptr(FlatpakDir) dir = flatpak_installation_get_dir_maybe_no_repo (self);

  return flatpak_dir_get_display_name (dir);
}

/**
 * flatpak_installation_get_priority:
 * @self: a #FlatpakInstallation
 *
 * Returns the numeric priority of the system installation for @self.
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
 * Returns the type of storage of the system installation for @self.
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
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDeploy) app_deploy = NULL;
  g_autofree char *app_ref = NULL;

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

  return flatpak_run_app (app_ref,
                          app_deploy,
                          NULL, NULL,
                          NULL, NULL,
                          FLATPAK_RUN_FLAG_BACKGROUND,
                          NULL,
                          NULL, 0,
                          cancellable, error);
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
  g_autoptr(GVariant) deploy_data = NULL;
  g_autofree const char **subpaths = NULL;
  gboolean is_current = FALSE;
  guint64 installed_size = 0;

  parts = g_strsplit (full_ref, "/", -1);

  deploy_data = flatpak_dir_get_deploy_data (dir, full_ref, cancellable, error);
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

  return flatpak_installed_ref_new (full_ref,
                                    alt_id ? alt_id : commit,
                                    latest_alt_id ? latest_alt_id : latest_commit,
                                    origin, subpaths,
                                    deploy_path,
                                    installed_size,
                                    is_current,
                                    flatpak_deploy_data_get_eol (deploy_data),
                                    flatpak_deploy_data_get_eol_rebase (deploy_data));
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
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   "Ref %s not installed", ref);
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
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   "App %s not installed", name);
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

static void
async_result_cb (GObject      *obj,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GAsyncResult **result_out = user_data;

  *result_out = g_object_ref (result);
}

/**
 * flatpak_installation_list_installed_refs_for_update:
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references that has a remote update that is not
 * locally available. However, even though an app is not returned by this
 * it can have local updates available that has not been deployed. Look
 * at commit vs latest_commit on installed apps for this.
 *
 * Returns: (transfer container) (element-type FlatpakInstalledRef): a GPtrArray of
 *   #FlatpakInstalledRef instances, or %NULL on error
 */
GPtrArray *
flatpak_installation_list_installed_refs_for_update (FlatpakInstallation *self,
                                                     GCancellable        *cancellable,
                                                     GError             **error)
{
  g_autoptr(GPtrArray) updates = NULL; /* (element-type FlatpakInstalledRef) */
  g_autoptr(GPtrArray) installed = NULL; /* (element-type FlatpakInstalledRef) */
  g_autoptr(GPtrArray) remotes = NULL; /* (element-type FlatpakRemote) */
  g_autoptr(GHashTable) remote_commits = NULL; /* (element-type utf8 utf8) */
  int i, j;
  g_autoptr(FlatpakDir) dir = NULL;
  g_auto(OstreeRepoFinderResultv) results = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GPtrArray) collection_refs = NULL; /* (element-type OstreeCollectionRef) */

  remote_commits = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  remotes = flatpak_installation_list_remotes (self, cancellable, error);
  if (remotes == NULL)
    return NULL;

  for (i = 0; i < remotes->len; i++)
    {
      FlatpakRemote *remote = g_ptr_array_index (remotes, i);
      g_autoptr(GPtrArray) refs = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autofree char *collection_id = NULL;
      const char *remote_name = flatpak_remote_get_name (remote);

      if (flatpak_remote_get_disabled (remote))
        continue;

      /* Remotes with collection IDs will be handled separately below */
      collection_id = flatpak_remote_get_collection_id (remote);
      if (collection_id != NULL)
        continue;

      /* We ignore errors here. we don't want one remote to fail us */
      refs = flatpak_installation_list_remote_refs_sync (self,
                                                         remote_name,
                                                         cancellable, &local_error);
      if (refs != NULL)
        {
          for (j = 0; j < refs->len; j++)
            {
              FlatpakRemoteRef *remote_ref = g_ptr_array_index (refs, j);
              g_autofree char *full_ref = flatpak_ref_format_ref (FLATPAK_REF (remote_ref));
              g_autofree char *key = g_strdup_printf ("%s:%s", remote_name, full_ref);

              g_hash_table_insert (remote_commits, g_steal_pointer (&key),
                                   g_strdup (flatpak_ref_get_commit (FLATPAK_REF (remote_ref))));
            }
        }
      else
        {
          g_debug ("Update: Failed to read remote %s: %s",
                   flatpak_remote_get_name (remote),
                   local_error->message);
        }
    }

  installed = flatpak_installation_list_installed_refs (self, cancellable, error);
  if (installed == NULL)
    return NULL;

  updates = g_ptr_array_new_with_free_func (g_object_unref);

  for (i = 0; i < installed->len; i++)
    {
      FlatpakInstalledRef *installed_ref = g_ptr_array_index (installed, i);
      const char *remote_name = flatpak_installed_ref_get_origin (installed_ref);
      g_autofree char *full_ref = flatpak_ref_format_ref (FLATPAK_REF (installed_ref));
      g_autofree char *key = g_strdup_printf ("%s:%s", remote_name, full_ref);
      const char *remote_commit = g_hash_table_lookup (remote_commits, key);
      const char *local_commit = flatpak_installed_ref_get_latest_commit (installed_ref);

      if (remote_commit != NULL &&
          g_strcmp0 (remote_commit, local_commit) != 0)
        g_ptr_array_add (updates, g_object_ref (installed_ref));
    }

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  collection_refs = g_ptr_array_new ();

  for (i = 0; i < installed->len; i++)
    {
      FlatpakInstalledRef *installed_ref = g_ptr_array_index (installed, i);
      g_autofree char *collection_id = NULL;
      const char *remote_name = flatpak_installed_ref_get_origin (installed_ref);

      collection_id = flatpak_dir_get_remote_collection_id (dir, remote_name);
      if (collection_id != NULL)
        {
          g_autofree char *ref = flatpak_ref_format_ref (FLATPAK_REF (installed_ref));
          OstreeCollectionRef *c_r = ostree_collection_ref_new (collection_id, ref);
          g_ptr_array_add (collection_refs, c_r);
        }
    }

  /* if we do not have any collection refs, then we shouldn't try to find
   * dynamic remotes for them, to avoid extra unnecessary processing, and also
   * because the refs array cannot be empty in ostree_repo_find_remotes_async
   * (otherwise it early returns and we never get our callback called) */
  if (collection_refs->len > 0)
    {
      g_autoptr(GMainContextPopDefault) context = NULL;

      g_ptr_array_add (collection_refs, NULL);

      context = flatpak_main_context_new_default ();

      ostree_repo_find_remotes_async (flatpak_dir_get_repo (dir),
                                      (const OstreeCollectionRef * const *) collection_refs->pdata,
                                      NULL,  /* no options */
                                      NULL, /* default finders */
                                      NULL,  /* no progress */
                                      cancellable,
                                      async_result_cb,
                                      &result);

      while (result == NULL)
        g_main_context_iteration (context, TRUE);

      results = ostree_repo_find_remotes_finish (flatpak_dir_get_repo (dir), result, error);

      if (results == NULL)
        return NULL;
    }

  for (i = 0; i < installed->len; i++)
    {
      FlatpakInstalledRef *installed_ref = g_ptr_array_index (installed, i);
      const char *remote_name = flatpak_installed_ref_get_origin (installed_ref);
      g_autofree char *ref = flatpak_ref_format_ref (FLATPAK_REF (installed_ref));
      g_autofree char *collection_id = NULL;
      g_autoptr(OstreeCollectionRef) collection_ref = NULL;

      collection_id = flatpak_dir_get_remote_collection_id (dir, remote_name);
      collection_ref = ostree_collection_ref_new (collection_id, ref);

      /* Look for matching remote refs that are updates */
      for (j = 0; results != NULL && results[j] != NULL; j++)
        {
          const char *local_commit, *remote_commit;

          local_commit = flatpak_installed_ref_get_latest_commit (installed_ref);
          remote_commit = g_hash_table_lookup (results[j]->ref_to_checksum, collection_ref);
          if (remote_commit == NULL || g_strcmp0 (remote_commit, local_commit) == 0)
            continue;

          /* The ref_to_checksum map only tells us if this remote is offering
           * the latest commit of the available remotes; we have to check
           * ref_to_timestamp to know if the commit is an update or a
           * downgrade.
           */
          {
            guint64 local_timestamp = 0;
            guint64 *remote_timestamp;
            g_autoptr(GVariant) commit_v = NULL;

            if (ostree_repo_load_commit (flatpak_dir_get_repo (dir), local_commit, &commit_v, NULL, NULL))
              local_timestamp = ostree_commit_get_timestamp (commit_v);

            remote_timestamp = g_hash_table_lookup (results[j]->ref_to_timestamp, collection_ref);
            *remote_timestamp = GUINT64_FROM_BE (*remote_timestamp);

            g_debug ("%s: Comparing local timestamp %" G_GUINT64_FORMAT " to remote timestamp %"
                     G_GUINT64_FORMAT " on ref (%s, %s)", G_STRFUNC, local_timestamp, *remote_timestamp,
                     collection_ref->collection_id, collection_ref->ref_name);

            /* The timestamp could be 0 due to an error reading it. Assume
             * it's an update until proven otherwise. */
            if (*remote_timestamp != 0 && *remote_timestamp <= local_timestamp)
              continue;
          }

          g_ptr_array_add (updates, g_object_ref (installed_ref));

          /* Move on to the next ref so we don't add duplicates */
          break;
        }
    }

  return g_steal_pointer (&updates);
}

/* Find all USB and LAN repositories which share the same collection ID as
 * @remote_name, and add a #FlatpakRemote to @remotes for each of them. The caller
 * must initialise @remotes. Returns %TRUE without modifying @remotes if the
 * given remote doesn’t have a collection ID configured.
 *
 * FIXME: If this were async, the parallelisation could be handled in the caller. */
static gboolean
list_remotes_for_configured_remote (FlatpakInstallation *self,
                                    const gchar         *remote_name,
                                    FlatpakDir          *dir,
                                    gboolean             types_filter[],
                                    GPtrArray           *remotes /* (element-type FlatpakRemote) */,
                                    GCancellable        *cancellable,
                                    GError             **error)
{
  g_autofree gchar *collection_id = NULL;
  OstreeCollectionRef ref;
  OstreeCollectionRef ref2;
  const OstreeCollectionRef *refs[3] = { NULL, };
  g_autofree gchar *appstream_ref = NULL;
  g_autofree gchar *appstream2_ref = NULL;

  g_auto(OstreeRepoFinderResultv) results = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(OstreeRepoFinder) finder_mount = NULL, finder_avahi = NULL;
  OstreeRepoFinder *finders[3] = { NULL, };
  gsize i;
  guint finder_index = 0;
  g_autoptr(GMainContextPopDefault) context = NULL;

  if (!types_filter[FLATPAK_REMOTE_TYPE_USB] &&
      !types_filter[FLATPAK_REMOTE_TYPE_LAN])
    return TRUE;

  /* Find the collection ID for @remote_name, or bail if there is none. */
  if (!ostree_repo_get_remote_option (flatpak_dir_get_repo (dir),
                                      remote_name, "collection-id",
                                      NULL, &collection_id, error))
    return FALSE;
  if (collection_id == NULL || *collection_id == '\0')
    return TRUE;

  context = flatpak_main_context_new_default ();

  appstream_ref = g_strdup_printf ("appstream/%s", flatpak_get_arch ());
  ref.collection_id = collection_id;
  ref.ref_name = appstream_ref;
  refs[0] = &ref;
  appstream2_ref = g_strdup_printf ("appstream2/%s", flatpak_get_arch ());
  ref2.collection_id = collection_id;
  ref2.ref_name = appstream2_ref;
  refs[1] = &ref2;

  if (types_filter[FLATPAK_REMOTE_TYPE_USB])
    {
      finder_mount = OSTREE_REPO_FINDER (ostree_repo_finder_mount_new (NULL));
      finders[finder_index++] = finder_mount;
    }

  if (types_filter[FLATPAK_REMOTE_TYPE_LAN])
    {
      g_autoptr(GError) local_error = NULL;
      finder_avahi = OSTREE_REPO_FINDER (ostree_repo_finder_avahi_new (context));
      finders[finder_index++] = finder_avahi;

      /* The Avahi finder may fail to start on, for example, a CI server. */
      ostree_repo_finder_avahi_start (OSTREE_REPO_FINDER_AVAHI (finder_avahi), &local_error);
      if (local_error != NULL)
        {
          if (finder_index == 1)
            return TRUE;
          else
            {
              finders[--finder_index] = NULL;
              g_clear_object (&finder_avahi);
            }
        }
    }

  ostree_repo_find_remotes_async (flatpak_dir_get_repo (dir),
                                  (const OstreeCollectionRef * const *) refs,
                                  NULL,  /* no options */
                                  finders,
                                  NULL,  /* no progress */
                                  cancellable,
                                  async_result_cb,
                                  &result);

  while (result == NULL)
    g_main_context_iteration (context, TRUE);

  results = ostree_repo_find_remotes_finish (flatpak_dir_get_repo (dir), result, error);

  if (types_filter[FLATPAK_REMOTE_TYPE_LAN])
    ostree_repo_finder_avahi_stop (OSTREE_REPO_FINDER_AVAHI (finder_avahi));

  for (i = 0; results != NULL && results[i] != NULL; i++)
    {
      g_ptr_array_add (remotes,
                       flatpak_remote_new_from_ostree (results[i]->remote,
                                                       results[i]->finder,
                                                       dir));
    }

  return TRUE;
}

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

  for (i = 0; i < NUM_FLATPAK_REMOTE_TYPES; ++i)
    {
      /* If NULL or an empty array of types is passed then we include all types */
      types_filter[i] = (num_types == 0) ? TRUE : FALSE;
    }

  for (i = 0; i < num_types; ++i)
    {
      g_return_val_if_fail (types[i] < NUM_FLATPAK_REMOTE_TYPES, NULL);
      types_filter[types[i]] = TRUE;
    }

  for (i = 0; remote_names[i] != NULL; ++i)
    {
      g_autoptr(GError) local_error = NULL;
      if (types_filter[FLATPAK_REMOTE_TYPE_STATIC])
        g_ptr_array_add (remotes, flatpak_remote_new_with_dir (remote_names[i],
                                                               dir_clone));

      /* Add the dynamic mirrors of this remote. */
      if (!list_remotes_for_configured_remote (self, remote_names[i], dir_clone,
                                               types_filter, remotes,
                                               cancellable, &local_error))
        g_debug ("Couldn't find remotes for configured remote %s: %s",
                 remote_names[i], local_error->message);
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
 * Returns: (transfer container) (element-type FlatpakRemote): an GPtrArray of
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
 * the only supported key is "languages", which is a comman-separated
 * list of langue codes like "sv;en;pl", or "" to mean all languages.
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

  if (!flatpak_dir_update_remote_configuration (dir, name, cancellable, error))
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
 * Returns: (transfer full): a #FlatpakRemote instances, or %NULL error
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
  g_autofree char *metadata_contents = NULL;
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
 * Install an application or runtime from an flatpak bundle file.
 * See flatpak-build-bundle(1) for how to create bundles.
 *
 * Returns: (transfer full): The ref for the newly installed app or %NULL on failure
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

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  remote = flatpak_dir_ensure_bundle_remote (dir, file, NULL, &ref, NULL, NULL, NULL, cancellable, error);
  if (remote == NULL)
    return NULL;

  /* Make sure we pick up the new config */
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
  g_autoptr(FlatpakCollectionRef) coll_ref = NULL;
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

  coll_ref = flatpak_collection_ref_new (collection_id, ref);
  return flatpak_remote_ref_new (coll_ref, NULL, remote, NULL);
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
                                   FlatpakProgressCallback progress,
                                   gpointer                progress_data,
                                   GCancellable           *cancellable,
                                   GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autofree char *ref = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
  FlatpakInstalledRef *result = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autoptr(GMainContextPopDefault) main_context = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  ref = flatpak_compose_ref (kind == FLATPAK_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
  if (deploy_dir != NULL)
    {
      g_set_error (error,
                   FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                   "%s branch %s already installed", name, branch ? branch : "master");
      return NULL;
    }

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, cancellable, error);
  if (state == NULL)
    return NULL;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = flatpak_main_context_new_default ();

  if (progress)
    ostree_progress = flatpak_progress_new (progress, progress_data);
  else
    ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

  if (!flatpak_dir_install (dir_clone,
                            (flags & FLATPAK_INSTALL_FLAGS_NO_PULL) != 0,
                            (flags & FLATPAK_INSTALL_FLAGS_NO_DEPLOY) != 0,
                            (flags & FLATPAK_INSTALL_FLAGS_NO_STATIC_DELTAS) != 0,
                            FALSE, state,
                            ref, NULL, (const char **) subpaths,
                            ostree_progress, cancellable, error))
    goto out;

  if ((flags & FLATPAK_INSTALL_FLAGS_NO_TRIGGERS) == 0 &&
      g_str_has_prefix (ref, "app"))
    flatpak_dir_run_triggers (dir_clone, cancellable, NULL);

  /* Note that if the caller sets FLATPAK_INSTALL_FLAGS_NO_DEPLOY we must
   * always return an error, as explained above. Otherwise get_ref will
   * always return an error. */
  if ((flags & FLATPAK_INSTALL_FLAGS_NO_DEPLOY) != 0)
    {
      g_set_error (error,
                   FLATPAK_ERROR, FLATPAK_ERROR_ONLY_PULLED,
                   "As requested, %s was only pulled, but not installed",
                   name);
      goto out;
    }

  result = get_ref (dir, ref, cancellable, error);
  if (result == NULL)
    goto out;

out:
  if (ostree_progress)
    ostree_async_progress_finish (ostree_progress);

  return result;
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
  return flatpak_installation_install_full (self, FLATPAK_INSTALL_FLAGS_NONE,
                                            remote_name, kind, name, arch, branch,
                                            NULL, progress, progress_data,
                                            cancellable, error);
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
 * Update an application or runtime.
 *
 * If the specified package is not installed, then %FLATPAK_ERROR_NOT_INSTALLED
 * will be thrown.
 *
 * If no updates could be found on the remote end and the package is
 * already up to date, then %FLATPAK_ERROR_ALREADY_INSTALLED will be thrown.
 *
 * Returns: (transfer full): The ref for the newly updated app or %NULL on failure
 */
FlatpakInstalledRef *
flatpak_installation_update_full (FlatpakInstallation    *self,
                                  FlatpakUpdateFlags      flags,
                                  FlatpakRefKind          kind,
                                  const char             *name,
                                  const char             *arch,
                                  const char             *branch,
                                  const char * const     *subpaths,
                                  FlatpakProgressCallback progress,
                                  gpointer                progress_data,
                                  GCancellable           *cancellable,
                                  GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autofree char *ref = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
  g_autofree char *remote_name = NULL;
  FlatpakInstalledRef *result = NULL;
  g_autofree char *target_commit = NULL;
  g_auto(OstreeRepoFinderResultv) check_results = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autoptr(GMainContextPopDefault) main_context = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  ref = flatpak_compose_ref (kind == FLATPAK_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
  if (deploy_dir == NULL)
    {
      g_set_error (error,
                   FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, branch ? branch : "master");
      return NULL;
    }

  remote_name = flatpak_dir_get_origin (dir, ref, cancellable, error);
  if (remote_name == NULL)
    return NULL;

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, cancellable, error);
  if (state == NULL)
    return NULL;

  target_commit = flatpak_dir_check_for_update (dir, state, ref, NULL,
                                                (const char **) subpaths,
                                                (flags & FLATPAK_UPDATE_FLAGS_NO_PULL) != 0,
                                                &check_results,
                                                cancellable, error);
  if (target_commit == NULL)
    return NULL;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return NULL;

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = flatpak_main_context_new_default ();

  if (progress)
    ostree_progress = flatpak_progress_new (progress, progress_data);
  else
    ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

  if (!flatpak_dir_update (dir_clone,
                           (flags & FLATPAK_UPDATE_FLAGS_NO_PULL) != 0,
                           (flags & FLATPAK_UPDATE_FLAGS_NO_DEPLOY) != 0,
                           (flags & FLATPAK_UPDATE_FLAGS_NO_STATIC_DELTAS) != 0,
                           FALSE, state,
                           ref, target_commit,
                           (const OstreeRepoFinderResult * const *) check_results,
                           (const char **) subpaths,
                           ostree_progress, cancellable, error))
    goto out;

  if ((flags & FLATPAK_UPDATE_FLAGS_NO_TRIGGERS) == 0 &&
      g_str_has_prefix (ref, "app"))
    flatpak_dir_run_triggers (dir_clone, cancellable, NULL);

  result = get_ref (dir, ref, cancellable, error);
  if (result == NULL)
    goto out;

  /* We don't get prunable objects if not pulling or if NO_PRUNE is passed */
  if (!(flags & FLATPAK_UPDATE_FLAGS_NO_PULL) && !(flags & FLATPAK_UPDATE_FLAGS_NO_PRUNE))
    flatpak_dir_prune (dir_clone, cancellable, NULL);

out:
  if (ostree_progress)
    ostree_async_progress_finish (ostree_progress);

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
 * Update an application or runtime.
 *
 * If the specified package is not installed, then %FLATPAK_ERROR_NOT_INSTALLED
 * will be thrown.
 *
 * If no updates could be found on the remote end and the package is
 * already up to date, then %FLATPAK_ERROR_ALREADY_INSTALLED will be thrown.
 *
 * Returns: (transfer full): The ref for the newly updated app or %NULL on failure
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
  return flatpak_installation_update_full (self, flags, kind, name, arch,
                                           branch, NULL, progress, progress_data,
                                           cancellable, error);
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
 * Uninstall an application or runtime.
 *
 * Returns: %TRUE on success
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
  return flatpak_installation_uninstall_full (self, FLATPAK_UNINSTALL_FLAGS_NONE,
                                              kind, name, arch, branch,
                                              progress, progress_data,
                                              cancellable, error);
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
 * Uninstall an application or runtime.
 *
 * Returns: %TRUE on success
 *
 * Since: 0.11.8
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

  if ((flags & FLATPAK_UNINSTALL_FLAGS_NO_TRIGGERS) == 0 &&
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

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, cancellable, error);
  if (state == NULL)
    return FALSE;

  return flatpak_remote_state_lookup_cache (state, full_ref,
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
  const char *res = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, cancellable, error);
  if (state == NULL)
    return FALSE;

  if (!flatpak_remote_state_lookup_cache (state, full_ref,
                                          NULL, NULL, &res,
                                          error))
    return NULL;

  return g_bytes_new (res, strlen (res));
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
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autoptr(GHashTable) ht = NULL;
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  state = flatpak_dir_get_remote_state (dir, remote_or_uri, cancellable, error);
  if (state == NULL)
    return NULL;

  if (!flatpak_dir_list_remote_refs (dir, state, &ht,
                                     cancellable, error))
    return NULL;

  g_hash_table_iter_init (&iter, ht);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakRemoteRef *ref;
      FlatpakCollectionRef *coll_ref = key;
      const gchar *ref_commit = value;

      ref = flatpak_remote_ref_new (coll_ref, ref_commit, remote_or_uri, state);

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
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GHashTable) ht = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autofree char *ref = NULL;
  g_autoptr(FlatpakCollectionRef) coll_ref = NULL;
  g_autofree gchar *collection_id = NULL;
  const char *checksum;

  if (branch == NULL)
    branch = "master";

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return NULL;

  state = flatpak_dir_get_remote_state (dir, remote_name, cancellable, error);
  if (state == NULL)
    return NULL;

  if (!flatpak_dir_list_remote_refs (dir, state, &ht,
                                     cancellable, error))
    return NULL;

  /* FIXME: Rework to accept the collection ID as an input argument instead */
  if (!ostree_repo_get_remote_option (flatpak_dir_get_repo (dir),
                                      remote_name, "collection-id",
                                      NULL, &collection_id, error))
    return FALSE;

  if (kind == FLATPAK_REF_KIND_APP)
    ref = flatpak_build_app_ref (name,
                                 branch,
                                 arch);
  else
    ref = flatpak_build_runtime_ref (name,
                                     branch,
                                     arch);

  coll_ref = flatpak_collection_ref_new (collection_id, ref);
  checksum = g_hash_table_lookup (ht, coll_ref);

  /* If there was not a match, it may be because the collection ID is
   * not set in the local configuration, or it is wrong, so we resort to
   * trying to match just the ref name */
  if (checksum == NULL)
    {
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, ht);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          FlatpakCollectionRef *current =  (FlatpakCollectionRef *) key;
          if (g_strcmp0 (current->ref_name, ref) == 0)
            {
              checksum = (const gchar *) value;
              break;
            }
        }
    }

  if (checksum != NULL)
    return flatpak_remote_ref_new (coll_ref, checksum, remote_name, state);

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "Reference %s doesn't exist in remote", ref);
  return NULL;
}

/**
 * flatpak_installation_update_appstream_sync:
 * @self: a #FlatpakInstallation
 * @remote_name: the name of the remote
 * @arch: Architecture to update, or %NULL for the local machine arch
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
 * @arch: Architecture to update, or %NULL for the local machine arch
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
                                                 FlatpakProgressCallback progress,
                                                 gpointer                progress_data,
                                                 gboolean               *out_changed,
                                                 GCancellable           *cancellable,
                                                 GError                **error)
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) dir_clone = NULL;
  g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
  gboolean res;
  g_autoptr(GMainContextPopDefault) main_context = NULL;

  dir = flatpak_installation_get_dir (self, error);
  if (dir == NULL)
    return FALSE;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = flatpak_dir_clone (dir);
  if (!flatpak_dir_ensure_repo (dir_clone, cancellable, error))
    return FALSE;

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = flatpak_main_context_new_default ();

  if (progress)
    ostree_progress = flatpak_progress_new (progress, progress_data);
  else
    ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

  res = flatpak_dir_update_appstream (dir_clone,
                                      remote_name,
                                      arch,
                                      out_changed,
                                      ostree_progress,
                                      cancellable,
                                      error);

  if (ostree_progress)
    ostree_async_progress_finish (ostree_progress);

  return res;
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
 * everyone should always be installed. For example,
 * flatpak_related_ref_should_download () returns TRUE if the
 * reference should be installed/updated with the app, and
 * flatpak_related_ref_should_delete () returns TRUE if it
 * should be uninstalled with the main ref.
 *
 * The commit property of each FlatpakRelatedRef is not guaranteed to be
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

  state = flatpak_dir_get_remote_state_optional (dir, remote_name, cancellable, error);
  if (state == NULL)
    return NULL;

  related = flatpak_dir_find_remote_related (dir, state, ref,
                                             cancellable, error);
  if (related == NULL)
    return NULL;

  for (i = 0; i < related->len; i++)
    {
      FlatpakRelated *rel = g_ptr_array_index (related, i);
      FlatpakRelatedRef *ref;

      ref = flatpak_related_ref_new (rel->collection_id, rel->ref, rel->commit,
                                     rel->subpaths, rel->download, rel->delete);

      if (ref)
        g_ptr_array_add (refs, ref);
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
      FlatpakRelatedRef *ref;

      ref = flatpak_related_ref_new (rel->collection_id, rel->ref, rel->commit,
                                     rel->subpaths, rel->download, rel->delete);

      if (ref)
        g_ptr_array_add (refs, ref);
    }

  return g_steal_pointer (&refs);
}

/**
 * flatpak_installation_remove_local_ref_sync
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
 * flatpak_installation_cleanup_local_refs_sync
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
 * flatpak_installation_prune_local_repo
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Remove all orphaned OSTree objects from the underlying OSTree repo in
 * @installation.
 *
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
 * flatpak_installation_run_triggers
 * @self: a #FlatpakInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Run the trigger commands to update the files exported by the apps in
 * @self. Should be used after one or more app install, upgrade or
 * uninstall operations with the NO_TRIGGERS flag set.
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
