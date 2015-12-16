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

#include "libgsystem.h"
#include "xdg-app-utils.h"
#include "xdg-app-installation.h"
#include "xdg-app-installed-ref-private.h"
#include "xdg-app-remote-private.h"
#include "xdg-app-enum-types.h"
#include "xdg-app-dir.h"
#include "xdg-app-error.h"

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
xdg_app_installation_new_for_dir (XdgAppDir *dir)
{
  XdgAppInstallation *self = g_object_new (XDG_APP_TYPE_INSTALLATION, NULL);
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  priv->dir = dir;
  return self;
}

XdgAppInstallation *
xdg_app_installation_new_system (void)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_get_system ());
}

XdgAppInstallation *
xdg_app_installation_new_user (void)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_get_user ());
}

XdgAppInstallation *
xdg_app_installation_new_for_path (GFile *path, gboolean user)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_new (path, user));
}

gboolean
xdg_app_installation_get_is_user (XdgAppInstallation *self)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  return xdg_app_dir_is_user (priv->dir);
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
  gboolean is_current = FALSE;

  parts = g_strsplit (full_ref, "/", -1);

  origin = xdg_app_dir_get_origin (priv->dir, full_ref, NULL, NULL);
  commit = xdg_app_dir_read_active (priv->dir, full_ref, cancellable);
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

  return xdg_app_installed_ref_new (full_ref,
                                    commit,
                                    origin,
                                    deploy_path,
                                    priv->dir,
                                    is_current);
}

/**
 * xdg_app_installation_get_installed_ref:
 * @self: a #XdgAppInstallation
 * @kind: ...
 * @name: ...
 * @arch: ...
 * @version: ...
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * ...
 *
 * Returns: (transfer full): ...
 */
XdgAppInstalledRef *
xdg_app_installation_get_installed_ref (XdgAppInstallation *self,
                                        XdgAppRefKind kind,
                                        const char *name,
                                        const char *arch,
                                        const char *version,
                                        GCancellable *cancellable,
                                        GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *ref = NULL;

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  if (kind == XDG_APP_REF_KIND_APP)
    ref = xdg_app_build_app_ref (name, version, arch);
  else
    ref = xdg_app_build_runtime_ref (name, version, arch);


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
 * ...
 *
 * Returns: (transfer full): ...
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
 * @kind: the kind of installation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the installed references.
 *
 * Returns: (transfer full) (array zero-terminated=1): an array of
 *   #XdgAppInstalledRef instances
 */
XdgAppInstalledRef **
xdg_app_installation_list_installed_refs (XdgAppInstallation *self,
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

  g_ptr_array_add (refs, NULL);
  return (XdgAppInstalledRef **)g_ptr_array_free (g_steal_pointer (&refs), FALSE);
}

/**
 * xdg_app_installation_list_remotes:
 * @self: a #XdgAppInstallation
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Lists the remotes.
 *
 * Returns: (transfer full) (array zero-terminated=1): an array of
 *   #XdgAppRemote instances
 */
XdgAppRemote **
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

  g_ptr_array_add (remotes, NULL);
  return (XdgAppRemote **)g_ptr_array_free (g_steal_pointer (&remotes), FALSE);
}

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
 * xdg_app_installation_install:
 * @self: a #XdgAppInstallation
 * @progress: (scope call): the callback
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Install a new ref.
 *
 * Returns: (transfer full): The ref for the newly installed app or %null on failure
 */
XdgAppInstalledRef *
xdg_app_installation_install (XdgAppInstallation  *self,
                              const char          *remote_name,
                              XdgAppRefKind        kind,
                              const char          *name,
                              const char          *arch,
                              const char          *version,
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

  ref = xdg_app_compose_ref (kind == XDG_APP_REF_KIND_APP, name, version, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (priv->dir, ref);
  if (g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_ALREADY_INSTALLED,
                   "%s branch %s already installed", name, version ? version : "master");
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

  if (!xdg_app_dir_pull (dir_clone, remote_name, ref,
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
                     "%s branch %s already installed", name, version ? version : "master");
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

 out:
  if (main_context)
    g_main_context_pop_thread_default (main_context);

  if (created_deploy_base && result == NULL)
    gs_shutil_rm_rf (deploy_base, cancellable, NULL);

  return result;
}

/**
 * xdg_app_installation_update:
 * @self: a #XdgAppInstallation
 * @progress: (scope call): the callback
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Update a ref.
 *
 * Returns: (transfer full): The ref for the newly updated app (or the same if no update) or %null on failure
 */
XdgAppInstalledRef *
xdg_app_installation_update (XdgAppInstallation  *self,
                             XdgAppRefKind        kind,
                             const char          *name,
                             const char          *arch,
                             const char          *version,
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
  gboolean was_updated;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  ref = xdg_app_compose_ref (kind == XDG_APP_REF_KIND_APP, name, version, arch, error);
  if (ref == NULL)
    return NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (priv->dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, version ? version : "master");
      return NULL;
    }

  remote_name = xdg_app_dir_get_origin (priv->dir, ref, cancellable, error);
  if (remote_name == NULL)
    return NULL;

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

  if (!xdg_app_dir_pull (dir_clone, remote_name, ref,
                         ostree_progress, cancellable, error))
    goto out;

  if (!xdg_app_dir_lock (dir_clone, &lock,
                         cancellable, error))
    goto out;

  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, version ? version : "master");
      return NULL;
    }

  if (!xdg_app_dir_deploy_update (dir_clone, ref, NULL, &was_updated, cancellable, error))
    return FALSE;

  if (was_updated && kind == XDG_APP_REF_KIND_APP)
    {
      if (!xdg_app_dir_update_exports (dir_clone, name, cancellable, error))
        goto out;
    }

  result = get_ref (self, ref, cancellable);

  glnx_release_lock_file (&lock);

  if (was_updated)
    {
      if (!xdg_app_dir_prune (dir_clone, cancellable, error))
        goto out;
    }

  xdg_app_dir_cleanup_removed (dir_clone, cancellable, NULL);

 out:
  if (main_context)
    g_main_context_pop_thread_default (main_context);

  return result;
}

/**
 * xdg_app_installation_uninstall:
 * @self: a #XdgAppInstallation
 * @progress: (scope call): the callback
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Update a ref.
 *
 * Returns: %true on success
 */
XDG_APP_EXTERN gboolean
xdg_app_installation_uninstall (XdgAppInstallation  *self,
                                XdgAppRefKind        kind,
                                const char          *name,
                                const char          *arch,
                                const char          *version,
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

  ref = xdg_app_compose_ref (kind == XDG_APP_REF_KIND_APP, name, version, arch, error);
  if (ref == NULL)
    return FALSE;

  if (!xdg_app_dir_lock (dir_clone, &lock,
                         cancellable, error))
    return FALSE;

  deploy_base = xdg_app_dir_get_deploy_dir (priv->dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, version ? version : "master");
      return FALSE;
    }

  remote_name = xdg_app_dir_get_origin (priv->dir, ref, cancellable, error);
  if (remote_name == NULL)
    return FALSE;

  /* prune, etc are not threadsafe, so we work on a copy */
  dir_clone = xdg_app_dir_clone (priv->dir);

  if (kind == XDG_APP_REF_KIND_APP)
    {
      g_debug ("dropping active ref");
      if (!xdg_app_dir_set_active (dir_clone, ref, NULL, cancellable, error))
        return FALSE;

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

  if (!was_deployed)
    {
      g_set_error (error,
                   XDG_APP_ERROR, XDG_APP_ERROR_NOT_INSTALLED,
                   "%s branch %s is not installed", name, version ? version : "master");
      return FALSE;
    }

  return TRUE;
}
