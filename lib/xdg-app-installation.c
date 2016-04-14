/*
 * Copyright © 2015 Red Hat, Inc
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

#include "config.h"

#include <string.h>

#include "libgsystem.h"
#include "xdg-app-utils.h"
#include "xdg-app-installation.h"
#include "xdg-app-installed-ref-private.h"
#include "xdg-app-remote-private.h"
#include "xdg-app-remote-ref-private.h"
#include "xdg-app-enum-types.h"
#include "xdg-app-dir.h"
#include "xdg-app-run.h"
#include "xdg-app-error.h"

/**
 * SECTION:xdg-app-installation
 * @Title: XdgAppInstallation
 * @Short_description: Installation information
 *
 * XdgAppInstallation is the toplevel object that software installers
 * should use to operate on an xdg-apps.
 *
 * An XdgAppInstallation object provides information about an installation
 * location for xdg-app applications. Typical installation locations are either
 * system-wide (in /var/lib/xdg-app) or per-user (in ~/.local/share/xdg-app).
 *
 * XdgAppInstallation can list configured remotes as well as installed application
 * and runtime references (in short: refs). It can also run, install, update and
 * uninstall applications and runtimes.
 */

typedef struct _XdgAppInstallationPrivate XdgAppInstallationPrivate;

struct _XdgAppInstallationPrivate
{
  XdgAppDir *dir;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppInstallation, xdg_app_installation, G_TYPE_OBJECT)

enum {
  PROP_0,
};

static void
xdg_app_installation_finalize (GObject *object)
{
  XdgAppInstallation *self = XDG_APP_INSTALLATION (object);
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  g_object_unref (priv->dir);

  G_OBJECT_CLASS (xdg_app_installation_parent_class)->finalize (object);
}

static void
xdg_app_installation_set_property (GObject         *object,
                                   guint            prop_id,
                                   const GValue    *value,
                                   GParamSpec      *pspec)
{

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_installation_get_property (GObject         *object,
                                   guint            prop_id,
                                   GValue          *value,
                                   GParamSpec      *pspec)
{

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_installation_class_init (XdgAppInstallationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_installation_get_property;
  object_class->set_property = xdg_app_installation_set_property;
  object_class->finalize = xdg_app_installation_finalize;

}

static void
xdg_app_installation_init (XdgAppInstallation *self)
{
}

static XdgAppInstallation *
xdg_app_installation_new_for_dir (XdgAppDir *dir,
                                  GCancellable  *cancellable,
                                  GError **error)
{
  XdgAppInstallation *self;
  XdgAppInstallationPrivate *priv;

  if (!xdg_app_dir_ensure_repo (dir, NULL, error))
    {
      g_object_unref (dir);
      return NULL;
    }

  self = g_object_new (XDG_APP_TYPE_INSTALLATION, NULL);
  priv = xdg_app_installation_get_instance_private (self);

  priv->dir = dir;

  return self;
}

/**
 * xdg_app_installation_new_system:
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #XdgAppInstallation for the system-wide installation.
 *
 * Returns: (transfer full): a new #XdgAppInstallation
 */
XdgAppInstallation *
xdg_app_installation_new_system (GCancellable *cancellable,
                                 GError **error)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_get_system (), cancellable, error);
}

/**
 * xdg_app_installation_new_user:
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #XdgAppInstallation for the per-user installation.
 *
 * Returns: (transfer full): a new #XdgAppInstallation
 */
XdgAppInstallation *
xdg_app_installation_new_user (GCancellable *cancellable,
                               GError **error)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_get_user (), cancellable, error);
}

/**
 * xdg_app_installation_new_for_path:
 * @path: a #GFile
 * @user: whether this is a user-specific location
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Creates a new #XdgAppInstallation for the installation at the given @path.
 *
 * Returns: (transfer full): a new #XdgAppInstallation
 */
XdgAppInstallation *
xdg_app_installation_new_for_path (GFile *path, gboolean user,
                                   GCancellable *cancellable,
                                   GError **error)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_new (path, user), cancellable, error);
}

/**
 * xdg_app_installation_get_is_user:
 * @self: a #XdgAppInstallation
 *
 * Returns whether the installation is for a user-specific location.
 *
 * Returns: %TRUE if @self is a per-user installation
 */
gboolean
xdg_app_installation_get_is_user (XdgAppInstallation *self)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  return xdg_app_dir_is_user (priv->dir);
}

/**
 * xdg_app_installation_get_path:
 * @self: a #XdgAppInstallation
 *
 * Returns the installation location for @self.
 *
 * Returns: (transfer full): an #GFile
 */
GFile *
xdg_app_installation_get_path (XdgAppInstallation *self)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  return g_object_ref (xdg_app_dir_get_path (priv->dir));
}

/**
 * xdg_app_installation_launch:
 * @self: a #XdgAppInstallation
 * @name: name of the app to launch
 * @arch: (nullable): which architecture to launch (default: current architecture)
 * @branch: (nullable): which branch of the application (default: "master")
 * @commit: (nullable): the commit of @branch to launch
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Launch an installed application.
 *
 * You can use xdg_app_installation_get_installed_ref() or
 * xdg_app_installation_get_current_installed_app() to find out what builds
 * are available, in order to get a value for @commit.
 *
 * Returns: %TRUE, unless an error occurred
 */
gboolean
xdg_app_installation_launch (XdgAppInstallation  *self,
                             const char          *name,
                             const char          *arch,
                             const char          *branch,
                             const char          *commit,
                             GCancellable        *cancellable,
                             GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *app_ref = NULL;
  g_autoptr(XdgAppDeploy) app_deploy = NULL;

  app_ref =
    xdg_app_build_app_ref (name, branch, arch);

  app_deploy =
    xdg_app_dir_load_deployed (priv->dir, app_ref,
                               commit,
                               cancellable, error);
  if (app_deploy == NULL)
    return FALSE;

  return xdg_app_run_app (app_ref,
                          app_deploy,
                          NULL, NULL,
                          NULL,
                          XDG_APP_RUN_FLAG_BACKGROUND,
                          NULL,
                          NULL, 0,
                          cancellable, error);
}


static XdgAppInstalledRef *
get_ref (XdgAppInstallation *self,
         const char *full_ref,
         GCancellable *cancellable)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) parts = NULL;
  g_autofree char *origin = NULL;
  g_autofree char *commit = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) deploy_subdir = NULL;
  g_autofree char *deploy_path = NULL;
  g_autofree char *latest_commit = NULL;
  g_auto(GStrv) subpaths = NULL;
  gboolean is_current = FALSE;
  guint64 installed_size = 0;

  parts = g_strsplit (full_ref, "/", -1);

  origin = xdg_app_dir_get_origin (priv->dir, full_ref, NULL, NULL);
  commit = xdg_app_dir_read_active (priv->dir, full_ref, cancellable);
  subpaths = xdg_app_dir_get_subpaths (priv->dir, full_ref, cancellable, NULL);

  deploy_dir = xdg_app_dir_get_deploy_dir  (priv->dir, full_ref);
  if (deploy_dir && commit)
    {
      deploy_subdir = g_file_get_child (deploy_dir, commit);
      deploy_path = g_file_get_path (deploy_subdir);
    }

  if (strcmp (parts[0], "app") == 0)
    {
      g_autofree char *current =
        xdg_app_dir_current_ref (priv->dir, parts[1], cancellable);
      if (current && strcmp (full_ref, current) == 0)
        is_current = TRUE;
    }

  latest_commit = xdg_app_dir_read_latest (priv->dir, origin, full_ref, NULL, NULL);

  if (!xdg_app_dir_get_installed_size (priv->dir,
                                       commit, &installed_size,
                                       cancellable,
                                       NULL))
    installed_size = 0;

  return xdg_app_installed_ref_new (full_ref,
                                    commit,
                                    latest_commit,
                                    origin, subpaths,
                                    deploy_path,
                                    installed_size,
                                    is_current);
}

/**
 * xdg_app_installation_get_installed_ref:
 * @self: a #XdgAppInstallation
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
 * Returns: (transfer full): an #XdgAppInstalledRef, or %NULL if an error occurred
 */
XdgAppInstalledRef *
xdg_app_installation_get_installed_ref (XdgAppInstallation *self,
                                        XdgAppRefKind kind,
                                        const char *name,
                                        const char *arch,
                                        const char *branch,
                                        GCancellable *cancellable,
                                        GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *ref = NULL;

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  if (kind == XDG_APP_REF_KIND_APP)
    ref = xdg_app_build_app_ref (name, branch, arch);
  else
    ref = xdg_app_build_runtime_ref (name, branch, arch);


  deploy = xdg_app_dir_get_if_deployed (priv->dir,
                                        ref, NULL, cancellable);
  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Ref %s no installed", ref);
      return NULL;
    }

  return get_ref (self, ref, cancellable);
}

/**
 * xdg_app_installation_get_current_installed_app:
 * @self: a #XdgAppInstallation
 * @name: the name of the app
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Get the last build of reference @name that was installed with
 * xdg_app_installation_install(), or %NULL if the reference has
 * never been installed locally.
 *
 * Returns: (transfer full): an #XdgAppInstalledRef
 */
XdgAppInstalledRef *
xdg_app_installation_get_current_installed_app (XdgAppInstallation *self,
                                                const char *name,
                                                GCancellable *cancellable,
                                                GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *current =
    xdg_app_dir_current_ref (priv->dir, name, cancellable);

  if (current)
    deploy = xdg_app_dir_get_if_deployed (priv->dir,
                                          current, NULL, cancellable);

  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "App %s no installed", name);
      return NULL;
    }

  return get_ref (self, current, cancellable);
}

/**
 * xdg_app_installation_list_installed_refs:
 * @self: a #XdgAppInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references.
 *
 * Returns: (transfer container) (element-type XdgAppInstalledRef): an GPtrArray of
 *   #XdgAppInstalledRef instances
 */
GPtrArray *
xdg_app_installation_list_installed_refs (XdgAppInstallation *self,
                                          GCancellable *cancellable,
                                          GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) raw_refs_app = NULL;
  g_auto(GStrv) raw_refs_runtime = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  if (!xdg_app_dir_list_refs (priv->dir,
                              "app",
                              &raw_refs_app,
                              cancellable, error))
    return NULL;

  for (i = 0; raw_refs_app[i] != NULL; i++)
    g_ptr_array_add (refs,
                     get_ref (self, raw_refs_app[i], cancellable));

  if (!xdg_app_dir_list_refs (priv->dir,
                              "runtime",
                              &raw_refs_runtime,
                              cancellable, error))
    return NULL;

  for (i = 0; raw_refs_runtime[i] != NULL; i++)
    g_ptr_array_add (refs,
                     get_ref (self, raw_refs_runtime[i], cancellable));
  
  return g_steal_pointer (&refs);
}

/**
 * xdg_app_installation_list_installed_refs_by_kind:
 * @self: a #XdgAppInstallation
 * @kind: the kind of installation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references of a specific kind.
 *
 * Returns: (transfer container) (element-type XdgAppInstalledRef): an GPtrArray of
 *   #XdgAppInstalledRef instances
 */
GPtrArray *
xdg_app_installation_list_installed_refs_by_kind (XdgAppInstallation *self,
                                                  XdgAppRefKind kind,
                                                  GCancellable *cancellable,
                                                  GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) raw_refs = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  if (!xdg_app_dir_list_refs (priv->dir,
                              kind == XDG_APP_REF_KIND_APP ? "app" : "runtime",
&raw_refs,
                              cancellable, error))
    return NULL;

  for (i = 0; raw_refs[i] != NULL; i++)
    g_ptr_array_add (refs,
                     get_ref (self, raw_refs[i], cancellable));

  return g_steal_pointer (&refs);
}

/**
 * xdg_app_installation_list_installed_refs_for_update:
 * @self: a #XdgAppInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references that has a remote update that is not
 * locally available. However, even though an app is not returned by this
 * it can have local updates available that has not been deployed. Look
 * at commit vs latest_commit on installed apps for this.
 *
 * Returns: (transfer container) (element-type XdgAppInstalledRef): an GPtrArray of
 *   #XdgAppInstalledRef instances
 */
GPtrArray *
xdg_app_installation_list_installed_refs_for_update (XdgAppInstallation *self,
                                                     GCancellable *cancellable,
                                                     GError **error)
{
  g_autoptr(GPtrArray) updates = NULL;
  g_autoptr(GPtrArray) installed = NULL;
  g_autoptr(GPtrArray) remotes = NULL;
  g_autoptr(GHashTable) ht = NULL;
  int i, j;

  ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  remotes = xdg_app_installation_list_remotes (self, cancellable, error);
  if (remotes == NULL)
    return NULL;

  for (i = 0; i < remotes->len; i++)
    {
      XdgAppRemote *remote = g_ptr_array_index (remotes, i);
      g_autoptr(GPtrArray) refs = NULL;
      g_autoptr(GError) local_error = NULL;

      /* We ignore errors here. we don't want one remote to fail us */
      refs = xdg_app_installation_list_remote_refs_sync (self,
                                                         xdg_app_remote_get_name (remote),
                                                         cancellable, &local_error);
      if (refs != NULL)
        {
          for (j = 0; j < refs->len; j++)
            {
              XdgAppRemoteRef *remote_ref = g_ptr_array_index (refs, j);
              g_autofree char *full_ref = xdg_app_ref_format_ref (XDG_APP_REF (remote_ref));
              g_autofree char *key = g_strdup_printf ("%s:%s", xdg_app_remote_get_name (remote),
                                                      full_ref);

              g_hash_table_insert (ht, g_steal_pointer (&key),
                                   g_strdup (xdg_app_ref_get_commit (XDG_APP_REF (remote_ref))));
            }
        }
      else
        {
          g_debug ("Update: Failed to read remote %s: %s\n",
                   xdg_app_remote_get_name (remote),
                   local_error->message);
        }
    }

  installed = xdg_app_installation_list_installed_refs (self, cancellable, error);
  if (installed == NULL)
    return NULL;

  updates = g_ptr_array_new_with_free_func (g_object_unref);

  for (i = 0; i < installed->len; i++)
    {
      XdgAppInstalledRef *installed_ref = g_ptr_array_index (installed, i);
      g_autofree char *full_ref = xdg_app_ref_format_ref (XDG_APP_REF (installed_ref));
      g_autofree char *key = g_strdup_printf ("%s:%s", xdg_app_installed_ref_get_origin (installed_ref),
                                              full_ref);
      const char *remote_ref = g_hash_table_lookup (ht, key);

      if (remote_ref != NULL &&
          g_strcmp0 (remote_ref,
                     xdg_app_installed_ref_get_latest_commit (installed_ref)) != 0)
        g_ptr_array_add (updates, g_object_ref (installed_ref));
    }

  return g_steal_pointer (&updates);
}


/**
 * xdg_app_installation_list_remotes:
 * @self: a #XdgAppInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the remotes, in priority (highest first) order. For same priority,
 * an earlier added remote comes before a later added one.
 *
 * Returns: (transfer container) (element-type XdgAppRemote): an GPtrArray of
 *   #XdgAppRemote instances
 */
GPtrArray *
xdg_app_installation_list_remotes (XdgAppInstallation  *self,
                                   GCancellable        *cancellable,
                                   GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) remote_names = NULL;
  g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  remote_names = xdg_app_dir_list_remotes (priv->dir, cancellable, error);
  if (remote_names == NULL)
    return NULL;

  for (i = 0; remote_names[i] != NULL; i++)
    g_ptr_array_add (remotes,
                     xdg_app_remote_new (priv->dir, remote_names[i]));

  return g_steal_pointer (&remotes);
}

/**
 * xdg_app_installation_get_remote_by_name:
 * @self: a #XdgAppInstallation
 * @name: a remote name
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Looks up a remote by name.
 *
 * Returns: (transfer full): a #XdgAppRemote instances, or %NULL error
 */
XdgAppRemote *
xdg_app_installation_get_remote_by_name (XdgAppInstallation *self,
                                         const gchar         *name,
                                         GCancellable        *cancellable,
                                         GError              **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) remote_names = NULL;
  int i;

  remote_names = xdg_app_dir_list_remotes (priv->dir, cancellable, error);
  if (remote_names == NULL)
    return NULL;

  for (i = 0; remote_names[i] != NULL; i++)
    {
      if (strcmp (remote_names[i], name) == 0)
        return xdg_app_remote_new (priv->dir, remote_names[i]);
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "No remote named '%s'", name);
  return NULL;
}

/**
 * xdg_app_installation_load_app_overrides:
 * @self: a #XdgAppInstallation
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
xdg_app_installation_load_app_overrides  (XdgAppInstallation *self,
                                          const char *app_id,
                                          GCancellable *cancellable,
                                          GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size;

  metadata_contents = xdg_app_dir_load_override (priv->dir, app_id, &metadata_size, error);
  if (metadata_contents == NULL)
    return NULL;

  return metadata_contents;
}

static void
progress_cb (OstreeAsyncProgress *progress, gpointer user_data)
{
  XdgAppProgressCallback progress_cb = g_object_get_data (G_OBJECT (progress), "callback");
  guint last_progress = GPOINTER_TO_UINT(g_object_get_data (G_OBJECT (progress), "last_progress"));
  GString *buf;
  g_autofree char *status = NULL;
  guint outstanding_fetches;
  guint outstanding_metadata_fetches;
  guint outstanding_writes;
  guint n_scanned_metadata;
  guint fetched_delta_parts;
  guint total_delta_parts;
  guint64 bytes_transferred;
  guint64 total_delta_part_size;
  guint fetched;
  guint metadata_fetched;
  guint requested;
  guint64 ellapsed_time;
  guint new_progress = 0;
  gboolean estimating = FALSE;

  buf = g_string_new ("");

  status = ostree_async_progress_get_status (progress);
  outstanding_fetches = ostree_async_progress_get_uint (progress, "outstanding-fetches");
  outstanding_metadata_fetches = ostree_async_progress_get_uint (progress, "outstanding-metadata-fetches");
  outstanding_writes = ostree_async_progress_get_uint (progress, "outstanding-writes");
  n_scanned_metadata = ostree_async_progress_get_uint (progress, "scanned-metadata");
  fetched_delta_parts = ostree_async_progress_get_uint (progress, "fetched-delta-parts");
  total_delta_parts = ostree_async_progress_get_uint (progress, "total-delta-parts");
  total_delta_part_size = ostree_async_progress_get_uint64 (progress, "total-delta-part-size");
  bytes_transferred = ostree_async_progress_get_uint64 (progress, "bytes-transferred");
  fetched = ostree_async_progress_get_uint (progress, "fetched");
  metadata_fetched = ostree_async_progress_get_uint (progress, "metadata-fetched");
  requested = ostree_async_progress_get_uint (progress, "requested");
  ellapsed_time = (g_get_monotonic_time () - ostree_async_progress_get_uint64 (progress, "start-time")) / G_USEC_PER_SEC;

  if (status)
    {
      g_string_append (buf, status);
    }
  else if (outstanding_fetches)
    {
      guint64 bytes_sec = bytes_transferred / ellapsed_time;
      g_autofree char *formatted_bytes_transferred =
        g_format_size_full (bytes_transferred, 0);
      g_autofree char *formatted_bytes_sec = NULL;

      if (!bytes_sec) // Ignore first second
        formatted_bytes_sec = g_strdup ("-");
      else
        {
          formatted_bytes_sec = g_format_size (bytes_sec);
        }

      if (total_delta_parts > 0)
        {
          g_autofree char *formatted_total =
            g_format_size (total_delta_part_size);
          g_string_append_printf (buf, "Receiving delta parts: %u/%u %s/s %s/%s",
                                  fetched_delta_parts, total_delta_parts,
                                  formatted_bytes_sec, formatted_bytes_transferred,
                                  formatted_total);

          new_progress = (100 * bytes_transferred) / total_delta_part_size;
        }
      else if (outstanding_metadata_fetches)
        {
          /* At this point we don't really know how much data there is, so we have to make a guess.
           * Since its really hard to figure out early how much data there is we report 1% until
           * all objects are scanned. */

          new_progress = 1;
          estimating = TRUE;

          g_string_append_printf (buf, "Receiving metadata objects: %u/(estimating) %s/s %s",
                                  metadata_fetched, formatted_bytes_sec, formatted_bytes_transferred);
        }
      else
        {
          new_progress = (100 * fetched) / requested;
          g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                                  (guint)((((double)fetched) / requested) * 100),
                                  fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
        }
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  if (new_progress < last_progress)
    new_progress = last_progress;
  g_object_set_data (G_OBJECT (progress), "last_progress", GUINT_TO_POINTER(new_progress));

  progress_cb (buf->str, new_progress, estimating, user_data);

  g_string_free (buf, TRUE);
}

/**
 * xdg_app_installation_install_bundle:
 * @self: a #XdgAppInstallation
 * @file: a #GFile that is an xdg-app bundle
 * @progress: (scope call): progress callback
 * @progress_data: user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Install an application or runtime from an xdg-app bundle file.
 * See xdg-app-build-bundle(1) for how to create brundles.
 *
 * Returns: (transfer full): The ref for the newly installed app or %NULL on failure
 */
XdgAppInstalledRef *
xdg_app_installation_install_bundle (XdgAppInstallation  *self,
                                     GFile               *file,
                                     XdgAppProgressCallback  progress,
                                     gpointer             progress_data,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *ref = NULL;
  gboolean created_deploy_base = FALSE;
  gboolean added_remote = FALSE;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(XdgAppDir) dir_clone = NULL;
  XdgAppInstalledRef *result = NULL;
  g_autoptr(GError) local_error = NULL;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *origin = NULL;
  g_auto(GStrv) parts = NULL;
  g_autofree char *basename = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *to_checksum = NULL;
  g_autofree char *remote = NULL;

  metadata = xdg_app_bundle_load (file, &to_checksum,
                                  &ref,
                                  &origin,
                                  NULL,
                                  &gpg_data,
                                  error);
  if (metadata == NULL)
    return FALSE;

  parts = xdg_app_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  deploy_base = xdg_app_dir_get_deploy_dir (priv->dir, ref);

  if (g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_ALREADY_INSTALLED,
                   "%s branch %s already installed", parts[1], parts[3]);
      return NULL;
    }

  /* Add a remote for later updates */
  basename = g_file_get_basename (file);
  remote = xdg_app_dir_create_origin_remote (priv->dir,
                                             origin,
                                             parts[1],
                                             basename,
                                             gpg_data,
                                             cancellable,
                                             error);
  if (remote == NULL)
    return FALSE;

  /* From here we need to goto out on error, to clean up */
  added_remote = TRUE;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = xdg_app_dir_clone (priv->dir);

  if (!xdg_app_dir_pull_from_bundle (dir_clone,
                                     file,
                                     remote,
                                     ref,
                                     gpg_data != NULL,
                                     cancellable,
                                     error))
    goto out;

  if (!xdg_app_dir_lock (dir_clone, &lock,
                         cancellable, error))
    goto out;

  if (!g_file_make_directory_with_parents (deploy_base, cancellable, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        g_set_error (error,
                     XDG_APP_ERROR, XDG_APP_ERROR_ALREADY_INSTALLED,
                     "%s branch %s already installed", parts[1], parts[3]);
      else
        g_propagate_error (error, g_steal_pointer (&local_error));
      goto out;
    }

  created_deploy_base = TRUE;

  if (!xdg_app_dir_set_origin (dir_clone, ref, remote, cancellable, error))
    goto out;

  if (!xdg_app_dir_deploy (dir_clone, ref, NULL, cancellable, error))
    goto out;

  if (strcmp (parts[0], "app") == 0)
    {
      if (!xdg_app_dir_make_current_ref (dir_clone, ref, cancellable, error))
        goto out;

      if (!xdg_app_dir_update_exports (dir_clone, parts[1], cancellable, error))
        goto out;
    }

  result = get_ref (self, ref, cancellable);

  glnx_release_lock_file (&lock);

  xdg_app_dir_cleanup_removed (dir_clone, cancellable, NULL);

  if (!xdg_app_dir_mark_changed (dir_clone, error))
    goto out;

 out:
  if (created_deploy_base && result == NULL)
    gs_shutil_rm_rf (deploy_base, cancellable, NULL);

  if (added_remote && result == NULL)
    ostree_repo_remote_delete (xdg_app_dir_get_repo (priv->dir), remote, NULL, NULL);

  return result;
}

/**
 * xdg_app_installation_install:
 * @self: a #XdgAppInstallation
 * @remote_name: name of the remote to use
 * @kind: what this ref contains (an #XdgAppRefKind)
 * @name: name of the app/runtime to fetch
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 * @branch: (nullable): which branch to fetch (default: 'master')
 * @progress: (scope call): progress callback
 * @progress_data: user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Install a new application or runtime.
 *
 * Returns: (transfer full): The ref for the newly installed app or %NULL on failure
 */
XdgAppInstalledRef *
xdg_app_installation_install (XdgAppInstallation  *self,
                              const char          *remote_name,
                              XdgAppRefKind        kind,
                              const char          *name,
                              const char          *arch,
                              const char          *branch,
                              XdgAppProgressCallback progress,
                              gpointer             progress_data,
                              GCancellable        *cancellable,
                              GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *ref = NULL;
  gboolean created_deploy_base = FALSE;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(XdgAppDir) dir_clone = NULL;
  g_autoptr(GMainContext) main_context = NULL;
  g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
  XdgAppInstalledRef *result = NULL;
  g_autoptr(GError) local_error = NULL;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  ref = xdg_app_compose_ref (kind == XDG_APP_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (priv->dir, ref);
  if (g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_ALREADY_INSTALLED,
                   "%s branch %s already installed", name, branch ? branch : "master");
      goto out;
    }

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = xdg_app_dir_clone (priv->dir);

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = g_main_context_new ();
  g_main_context_push_thread_default (main_context);

  if (progress)
    {
      ostree_progress = ostree_async_progress_new_and_connect (progress_cb, progress_data);
      g_object_set_data (G_OBJECT (ostree_progress), "callback", progress);
      g_object_set_data (G_OBJECT (ostree_progress), "last_progress", GUINT_TO_POINTER(0));
    }

  if (!xdg_app_dir_pull (dir_clone, remote_name, ref, NULL,
                         ostree_progress, cancellable, error))
    goto out;

  if (!xdg_app_dir_lock (dir_clone, &lock,
                         cancellable, error))
    goto out;

  if (!g_file_make_directory_with_parents (deploy_base, cancellable, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        g_set_error (error,
                     XDG_APP_ERROR, XDG_APP_ERROR_ALREADY_INSTALLED,
                     "%s branch %s already installed", name, branch ? branch : "master");
      else
        g_propagate_error (error, g_steal_pointer (&local_error));
      goto out;
    }
  created_deploy_base = TRUE;

  if (!xdg_app_dir_set_origin (dir_clone, ref, remote_name, cancellable, error))
    goto out;

  if (!xdg_app_dir_deploy (dir_clone, ref, NULL, cancellable, error))
    goto out;

  if (kind == XDG_APP_REF_KIND_APP)
    {
      if (!xdg_app_dir_make_current_ref (dir_clone, ref, cancellable, error))
        goto out;

      if (!xdg_app_dir_update_exports (dir_clone, name, cancellable, error))
        goto out;
    }

  result = get_ref (self, ref, cancellable);

  glnx_release_lock_file (&lock);

  xdg_app_dir_cleanup_removed (dir_clone, cancellable, NULL);

  if (!xdg_app_dir_mark_changed (dir_clone, error))
    goto out;

 out:
  if (main_context)
    g_main_context_pop_thread_default (main_context);

  if (created_deploy_base && result == NULL)
    gs_shutil_rm_rf (deploy_base, cancellable, NULL);

  if (ostree_progress)
    ostree_async_progress_finish (ostree_progress);

  return result;
}

/**
 * xdg_app_installation_update:
 * @self: a #XdgAppInstallation
 * @flags: an #XdgAppUpdateFlags variable
 * @kind: whether this is an app or runtime
 * @name: name of the app or runtime to update
 * @arch: (nullable): architecture of the app or runtime to update (default: current architecture)
 * @branch: (nullable): name of the branch of the app or runtime to update (default: master)
 * @progress: (scope call): the callback
 * @progress_data: user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Update an application or runtime.
 *
 * Returns: (transfer full): The ref for the newly updated app (or the same if no update) or %NULL on failure
 */
XdgAppInstalledRef *
xdg_app_installation_update (XdgAppInstallation  *self,
                             XdgAppUpdateFlags    flags,
                             XdgAppRefKind        kind,
                             const char          *name,
                             const char          *arch,
                             const char          *branch,
                             XdgAppProgressCallback progress,
                             gpointer             progress_data,
                             GCancellable        *cancellable,
                             GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *ref = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(XdgAppDir) dir_clone = NULL;
  g_autoptr(GMainContext) main_context = NULL;
  g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
  g_autofree char *remote_name = NULL;
  XdgAppInstalledRef *result = NULL;
  gboolean was_updated = FALSE;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;
  g_auto(GStrv) subpaths = NULL;

  ref = xdg_app_compose_ref (kind == XDG_APP_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (priv->dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, branch ? branch : "master");
      return NULL;
    }

  remote_name = xdg_app_dir_get_origin (priv->dir, ref, cancellable, error);
  if (remote_name == NULL)
    return NULL;

  subpaths = xdg_app_dir_get_subpaths (priv->dir, ref, cancellable, error);
  if (subpaths == NULL)
    return FALSE;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = xdg_app_dir_clone (priv->dir);

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = g_main_context_new ();
  g_main_context_push_thread_default (main_context);

  if (progress)
    {
      ostree_progress = ostree_async_progress_new_and_connect (progress_cb, progress_data);
      g_object_set_data (G_OBJECT (ostree_progress), "callback", progress);
      g_object_set_data (G_OBJECT (ostree_progress), "last_progress", GUINT_TO_POINTER(0));
    }

  if ((flags & XDG_APP_UPDATE_FLAGS_NO_PULL) == 0)
    {
      if (!xdg_app_dir_pull (dir_clone, remote_name, ref, subpaths,
                             ostree_progress, cancellable, error))
        goto out;
    }

  if ((flags & XDG_APP_UPDATE_FLAGS_NO_DEPLOY) == 0)
    {
      if (!xdg_app_dir_lock (dir_clone, &lock,
                             cancellable, error))
        goto out;

      if (!xdg_app_dir_deploy_update (dir_clone, ref, NULL, &was_updated, cancellable, error))
        return FALSE;

      if (was_updated && kind == XDG_APP_REF_KIND_APP)
        {
          if (!xdg_app_dir_update_exports (dir_clone, name, cancellable, error))
            goto out;
        }
    }

  result = get_ref (self, ref, cancellable);

  glnx_release_lock_file (&lock);

  if (was_updated)
    {
      if (!xdg_app_dir_prune (dir_clone, cancellable, error))
        goto out;

      if (!xdg_app_dir_mark_changed (dir_clone, error))
        goto out;
    }

  xdg_app_dir_cleanup_removed (dir_clone, cancellable, NULL);

 out:
  if (main_context)
    g_main_context_pop_thread_default (main_context);

  if (ostree_progress)
    ostree_async_progress_finish (ostree_progress);

  return result;
}

/**
 * xdg_app_installation_uninstall:
 * @self: a #XdgAppInstallation
 * @kind: what this ref contains (an #XdgAppRefKind)
 * @name: name of the app or runtime to uninstall
 * @arch: architecture of the app or runtime to uninstall
 * @branch: name of the branch of the app or runtime to uninstall
 * @progress: (scope call): the callback
 * @progress_data: user data passed to @progress
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Uninstall an application or runtime.
 *
 * Returns: %TRUE on success
 */
XDG_APP_EXTERN gboolean
xdg_app_installation_uninstall (XdgAppInstallation  *self,
                                XdgAppRefKind        kind,
                                const char          *name,
                                const char          *arch,
                                const char          *branch,
                                XdgAppProgressCallback  progress,
                                gpointer             progress_data,
                                GCancellable        *cancellable,
                                GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *ref = NULL;
  g_autofree char *remote_name = NULL;
  g_autofree char *current_ref = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(XdgAppDir) dir_clone = NULL;
  gboolean was_deployed = FALSE;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  ref = xdg_app_compose_ref (kind == XDG_APP_REF_KIND_APP, name, branch, arch, error);
  if (ref == NULL)
    return FALSE;

  /* prune, etc are not threadsafe, so we work on a copy */
  dir_clone = xdg_app_dir_clone (priv->dir);

  if (!xdg_app_dir_lock (dir_clone, &lock,
                         cancellable, error))
    return FALSE;

  deploy_base = xdg_app_dir_get_deploy_dir (priv->dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, branch ? branch : "master");
      return FALSE;
    }

  remote_name = xdg_app_dir_get_origin (priv->dir, ref, cancellable, error);
  if (remote_name == NULL)
    return FALSE;

  g_debug ("dropping active ref");
  if (!xdg_app_dir_set_active (dir_clone, ref, NULL, cancellable, error))
    return FALSE;

  if (kind == XDG_APP_REF_KIND_APP)
    {
      current_ref = xdg_app_dir_current_ref (dir_clone, name, cancellable);
      if (current_ref != NULL && strcmp (ref, current_ref) == 0)
        {
          g_debug ("dropping current ref");
          if (!xdg_app_dir_drop_current_ref (dir_clone, name, cancellable, error))
            return FALSE;
        }
    }

  if (!xdg_app_dir_undeploy_all (dir_clone, ref, FALSE, &was_deployed, cancellable, error))
    return FALSE;

  if (!xdg_app_dir_remove_ref (dir_clone, remote_name, ref, cancellable, error))
    return FALSE;

  glnx_release_lock_file (&lock);

  if (!xdg_app_dir_prune (dir_clone, cancellable, error))
    return FALSE;

  xdg_app_dir_cleanup_removed (dir_clone, cancellable, NULL);

  if (kind == XDG_APP_REF_KIND_APP)
    {
      if (!xdg_app_dir_update_exports (dir_clone, name, cancellable, error))
        return FALSE;
    }

  if (!xdg_app_dir_mark_changed (dir_clone, error))
    return FALSE;

  if (!was_deployed)
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, branch ? branch : "master");
      return FALSE;
    }

  return TRUE;
}

/**
 * xdg_app_installation_fetch_remote_size_sync:
 * @self: a #XdgAppInstallation
 * @remote_name: the name of the remote
 * @commit: the commit
 * @download_size: (out): return location for the download size
 * @installed_size: (out): return location for the installed size
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Gets information about the amount of data that needs to be transferred
 * to pull a commit from a remote repository, and about the amount of
 * local disk space that is required to check out this commit.
 *
 * This is deprectated, use xdg_app_installation_fetch_remote_size_sync2 instead.
 *
 * Returns: %TRUE, unless an error occurred
 */
gboolean
xdg_app_installation_fetch_remote_size_sync (XdgAppInstallation  *self,
                                             const char          *remote_name,
                                             const char          *commit,
                                             guint64             *download_size,
                                             guint64             *installed_size,
                                             GCancellable        *cancellable,
                                             GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  return xdg_app_dir_fetch_sizes (priv->dir, remote_name, commit,
                                  download_size,
                                  NULL,
                                  NULL,
                                  installed_size,
                                  cancellable,
                                  error);
}

/**
 * xdg_app_installation_fetch_remote_size_sync2:
 * @self: a #XdgAppInstallation
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
 * Note that if there are locally available data that are in the ref, which is commong
 * for instance if you're doing an update then the real download size may be smaller
 * than what is returned here.
 *
 * Returns: %TRUE, unless an error occurred
 */
gboolean
xdg_app_installation_fetch_remote_size_sync2 (XdgAppInstallation  *self,
                                              const char          *remote_name,
                                              XdgAppRef           *ref,
                                              guint64             *download_size,
                                              guint64             *installed_size,
                                              GCancellable        *cancellable,
                                              GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *full_ref = xdg_app_ref_format_ref (ref);

  return xdg_app_dir_fetch_ref_cache (priv->dir, remote_name, full_ref,
                                      download_size, installed_size,
                                      NULL,
                                      cancellable,
                                      error);
}

/**
 * xdg_app_installation_fetch_remote_metadata_sync:
 * @self: a #XdgAppInstallation
 * @remote_name: the name of the remote
 * @commit: the commit
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Obtains the metadata file from a commit.
 *
 * This is deprecated, use xdg_app_installation_fetch_remote_metadata_sync2
 *
 * Returns: (transfer full): a #GBytes containing the xdg-app metadata file,
 *   or %NULL if an error occurred
 */
GBytes *
xdg_app_installation_fetch_remote_metadata_sync (XdgAppInstallation *self,
                                                 const char *remote_name,
                                                 const char *commit,
                                                 GCancellable *cancellable,
                                                 GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GBytes) bytes = NULL;

  bytes = xdg_app_dir_fetch_metadata (priv->dir,
                                      remote_name,
                                      commit,
                                      cancellable,
                                      error);
  if (bytes == NULL)
    return NULL;

  return g_steal_pointer (&bytes);
}

/**
 * xdg_app_installation_fetch_remote_metadata_sync2:
 * @self: a #XdgAppInstallation
 * @remote_name: the name of the remote
 * @ref: the ref
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Obtains the metadata file from a commit.
 *
 * Returns: (transfer full): a #GBytes containing the xdg-app metadata file,
 *   or %NULL if an error occurred
 */
GBytes *
xdg_app_installation_fetch_remote_metadata_sync2 (XdgAppInstallation *self,
                                                  const char *remote_name,
                                                  XdgAppRef *ref,
                                                  GCancellable *cancellable,
                                                  GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autofree char *full_ref = xdg_app_ref_format_ref (ref);
  char *res = NULL;

  if (!xdg_app_dir_fetch_ref_cache (priv->dir, remote_name, full_ref,
                                    NULL, NULL,
                                    &res,
                                    cancellable, error))
    return NULL;

  return g_bytes_new_take (res, strlen (res));
}

/**
 * xdg_app_installation_list_remote_refs_sync:
 * @self: a #XdgAppInstallation
 * @remote_name: the name of the remote
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists all the applications and runtimes in a remote.
 *
 * Returns: (transfer container) (element-type XdgAppRemoteRef): an GPtrArray of
 *   #XdgAppRemoteRef instances
 */
GPtrArray *
xdg_app_installation_list_remote_refs_sync (XdgAppInstallation *self,
                                            const char     *remote_name,
                                            GCancellable *cancellable,
                                            GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GHashTable) ht = NULL;
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  if (!xdg_app_dir_list_remote_refs (priv->dir,
                                     remote_name,
                                     &ht,
                                     cancellable,
                                     error))
    return NULL;

  g_hash_table_iter_init (&iter, ht);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *refspec = key;
      const char *checksum = value;
      XdgAppRemoteRef *ref;

      ref = xdg_app_remote_ref_new (refspec, checksum, remote_name);

      if (ref)
        g_ptr_array_add (refs, ref);
    }

  return g_steal_pointer (&refs);
}

/**
 * xdg_app_installation_fetch_remote_ref_sync:
 * @self: a #XdgAppInstallation
 * @remote_name: the name of the remote
 * @kind: what this ref contains (an #XdgAppRefKind)
 * @name: name of the app/runtime to fetch
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 * @branch: (nullable): which branch to fetch (default: 'master')
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Gets the current remote branch of a ref in the remote.
 *
 * Returns: (transfer full): a #XdgAppRemoteRef instance, or %NULL
 */
XdgAppRemoteRef *
xdg_app_installation_fetch_remote_ref_sync (XdgAppInstallation *self,
                                            const char   *remote_name,
                                            XdgAppRefKind kind,
                                            const char   *name,
                                            const char   *arch,
                                            const char   *branch,
                                            GCancellable *cancellable,
                                            GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GHashTable) ht = NULL;
  g_autofree char *ref = NULL;
  const char *checksum;

  if (branch == NULL)
    branch = "master";

  if (!xdg_app_dir_list_remote_refs (priv->dir,
                                     remote_name,
                                     &ht,
                                     cancellable,
                                     error))
    return NULL;

  if (kind == XDG_APP_REF_KIND_APP)
    ref = xdg_app_build_app_ref (name,
                                 branch,
                                 arch);
  else
    ref = xdg_app_build_runtime_ref (name,
                                     branch,
                                     arch);

  checksum = g_hash_table_lookup (ht, ref);

  if (checksum != NULL)
    return xdg_app_remote_ref_new (ref, checksum, remote_name);

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "Reference %s doesn't exist in remote\n", ref);
  return NULL;
}

static void
no_progress_cb (OstreeAsyncProgress *progress, gpointer user_data)
{
}

/**
 * xdg_app_installation_update_appstream_sync:
 * @self: a #XdgAppInstallation
 * @remote_name: the name of the remote
 * @arch: Architecture to update, or %NULL for the local machine arch
 * @out_changed: (nullable): Set to %TRUE if the contents of the appstream changed, %FALSE if nothing changed
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Updates the local copy of appstream for @remote_name for the specified @arch.
 *
 * Returns: %TRUE on success, or %FALSE on error
 */
gboolean
xdg_app_installation_update_appstream_sync (XdgAppInstallation  *self,
                                          const char          *remote_name,
                                          const char          *arch,
                                          gboolean            *out_changed,
                                          GCancellable        *cancellable,
                                          GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(XdgAppDir) dir_clone = NULL;
  g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
  g_autoptr(GMainContext) main_context = NULL;
  gboolean res;

  /* Pull, prune, etc are not threadsafe, so we work on a copy */
  dir_clone = xdg_app_dir_clone (priv->dir);

  if (main_context)
    g_main_context_pop_thread_default (main_context);

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = g_main_context_new ();
  g_main_context_push_thread_default (main_context);

  ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

  res = xdg_app_dir_update_appstream (dir_clone,
                                      remote_name,
                                      arch,
                                      out_changed,
                                      ostree_progress,
                                      cancellable,
                                      error);

  g_main_context_pop_thread_default (main_context);

  if (ostree_progress)
    ostree_async_progress_finish (ostree_progress);

  return res;
}

/**
 * xdg_app_installation_create_monitor:
 * @self: a #XdgAppInstallation
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
xdg_app_installation_create_monitor (XdgAppInstallation  *self,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GFile) path = NULL;

  path = xdg_app_dir_get_changed_path (priv->dir);

  return g_file_monitor_file (path, G_FILE_MONITOR_NONE,
                              cancellable, error);
}
