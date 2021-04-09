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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"
#include "flatpak-instance.h"
#include "flatpak-instance-private.h"
#include "flatpak-enum-types.h"

#include <glib/gi18n-lib.h>

/**
 * SECTION:flatpak-instance
 * @Title: FlatpakInstance
 * @Short_description: Information about a running sandbox
 *
 * A FlatpakInstance refers to a running sandbox, and contains
 * some basic information about the sandbox setup, such as the
 * application and runtime used inside the sandbox.
 *
 * Importantly, it also gives access to the PID of the main
 * processes in the sandbox.
 *
 * One way to obtain FlatpakInstances is to use flatpak_instance_get_all().
 * Another way is to use flatpak_installation_launch_full().
 *
 * Note that process lifecycle tracking is fundamentally racy.
 * You have to be prepared for the sandbox and the processes
 * represented by a FlatpakInstance to not be around anymore.
 *
 * The FlatpakInstance api was added in Flatpak 1.1.
 */

typedef struct _FlatpakInstancePrivate FlatpakInstancePrivate;

struct _FlatpakInstancePrivate
{
  char     *id;
  char     *dir;

  GKeyFile *info;
  char     *app;
  char     *arch;
  char     *branch;
  char     *commit;
  char     *runtime;
  char     *runtime_commit;

  int       pid;
  int       child_pid;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakInstance, flatpak_instance, G_TYPE_OBJECT)

static void
flatpak_instance_finalize (GObject *object)
{
  FlatpakInstance *self = FLATPAK_INSTANCE (object);
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  g_free (priv->id);
  g_free (priv->dir);
  g_free (priv->app);
  g_free (priv->arch);
  g_free (priv->branch);
  g_free (priv->commit);
  g_free (priv->runtime);
  g_free (priv->runtime_commit);

  if (priv->info)
    g_key_file_unref (priv->info);

  G_OBJECT_CLASS (flatpak_instance_parent_class)->finalize (object);
}

static void
flatpak_instance_class_init (FlatpakInstanceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_instance_finalize;
}

static void
flatpak_instance_init (FlatpakInstance *self)
{
}

/**
 * flatpak_instance_get_id:
 * @self: a #FlatpakInstance
 *
 * Gets the instance ID. The ID is used by Flatpak for bookkeeping
 * purposes and has no further relevance.
 *
 * Returns: the instance ID
 *
 * Since: 1.1
 */
const char *
flatpak_instance_get_id (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->id;
}

/**
 * flatpak_instance_get_app:
 * @self: a #FlatpakInstance
 *
 * Gets the application ID of the application running in the instance.
 *
 * Note that this may return %NULL for sandboxes that don't have an application.
 *
 * Returns: (nullable): the application ID or %NULL
 *
 * Since: 1.1
 */
const char *
flatpak_instance_get_app (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->app;
}

/**
 * flatpak_instance_get_arch:
 * @self: a #FlatpakInstance
 *
 * Gets the architecture of the application running in the instance.
 *
 * Returns: the architecture
 *
 * Since: 1.1
 */
const char *
flatpak_instance_get_arch (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->arch;
}

/**
 * flatpak_instance_get_branch:
 * @self: a #FlatpakInstance
 *
 * Gets the branch of the application running in the instance.
 *
 * Returns: the architecture
 *
 * Since: 1.1
 */
const char *
flatpak_instance_get_branch (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->branch;
}

/**
 * flatpak_instance_get_commit:
 * @self: a #FlatpakInstance
 *
 * Gets the commit of the application running in the instance.
 *
 * Returns: the commit
 *
 * Since: 1.1
 */
const char *
flatpak_instance_get_commit (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->commit;
}

/**
 * flatpak_instance_get_runtime:
 * @self: a #FlatpakInstance
 *
 * Gets the ref of the runtime used in the instance.
 *
 * Returns: the runtime ref
 *
 * Since: 1.1
 */
const char *
flatpak_instance_get_runtime (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->runtime;
}

/**
 * flatpak_instance_get_runtime_commit:
 * @self: a #FlatpakInstance
 *
 * Gets the commit of the runtime used in the instance.
 *
 * Returns: the runtime commit
 *
 * Since: 1.1
 */
const char *
flatpak_instance_get_runtime_commit (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->runtime_commit;
}

/**
 * flatpak_instance_get_pid:
 * @self: a #FlatpakInstance
 *
 * Gets the PID of the outermost process in the sandbox. This is not the
 * application process itself, but a bubblewrap 'babysitter' process.
 *
 * See flatpak_instance_get_child_pid().
 *
 * Returns: the outermost process PID
 *
 * Since: 1.1
 */
int
flatpak_instance_get_pid (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->pid;
}

static int get_child_pid (const char *dir);

/**
 * flatpak_instance_get_child_pid:
 * @self: a #FlatpakInstance
 *
 * Gets the PID of the application process in the sandbox.
 *
 * See flatpak_instance_get_pid().
 *
 * Note that this function may return 0 immediately after launching
 * a sandbox, for a short amount of time.
 *
 * Returns: the application process PID
 *
 * Since: 1.1
 */
int
flatpak_instance_get_child_pid (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  if (priv->child_pid == 0)
    priv->child_pid = get_child_pid (priv->dir);

  return priv->child_pid;
}

/**
 * flatpak_instance_get_info:
 * @self: a #FlatpakInstance
 *
 * Gets a keyfile that holds information about the running sandbox.
 *
 * This file is available as /.flatpak-info inside the sandbox as well.
 *
 * The most important data in the keyfile is available with separate getters,
 * but there may be more information in the keyfile.
 *
 * Returns: the flatpak-info keyfile
 *
 * Since: 1.1
 */
GKeyFile *
flatpak_instance_get_info (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->info;
}

static GKeyFile *
get_instance_info (const char *dir)
{
  g_autofree char *file = NULL;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autoptr(GError) error = NULL;

  file = g_build_filename (dir, "info", NULL);

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file, file, G_KEY_FILE_NONE, &error))
    {
      g_debug ("Failed to load instance info file '%s': %s", file, error->message);
      return NULL;
    }

  return g_steal_pointer (&key_file);
}

static int
get_child_pid (const char *dir)
{
  g_autofree char *file = NULL;
  g_autofree char *contents = NULL;
  gsize length;
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *node;
  JsonObject *obj;

  file = g_build_filename (dir, "bwrapinfo.json", NULL);

  if (!g_file_get_contents (file, &contents, &length, &error))
    {
      g_debug ("Failed to load bwrapinfo.json file '%s': %s", file, error->message);
      return 0;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, length, &error))
    {
      g_debug ("Failed to parse bwrapinfo.json file '%s': %s", file, error->message);
      return 0;
    }

  node = json_parser_get_root (parser);
  if (!node)
    {
      g_debug ("Failed to parse bwrapinfo.json file '%s': %s", file, "empty");
      return 0;
    }

  obj = json_node_get_object (node);

  return json_object_get_int_member (obj, "child-pid");
}

static int
get_pid (const char *dir)
{
  g_autofree char *file = NULL;
  g_autofree char *contents = NULL;
  g_autoptr(GError) error = NULL;

  file = g_build_filename (dir, "pid", NULL);

  if (!g_file_get_contents (file, &contents, NULL, &error))
    {
      g_debug ("Failed to load pid file '%s': %s", file, error->message);
      return 0;
    }

  return (int) g_ascii_strtoll (contents, NULL, 10);
}

FlatpakInstance *
flatpak_instance_new (const char *dir)
{
  FlatpakInstance *self = g_object_new (flatpak_instance_get_type (), NULL);
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  priv->dir = g_strdup (dir);
  priv->id = g_path_get_basename (dir);

  priv->pid = get_pid (priv->dir);
  priv->child_pid = get_child_pid (priv->dir);
  priv->info = get_instance_info (priv->dir);

  if (priv->info)
    {
      if (g_key_file_has_group (priv->info, FLATPAK_METADATA_GROUP_APPLICATION))
        {
          priv->app = g_key_file_get_string (priv->info,
                                             FLATPAK_METADATA_GROUP_APPLICATION, FLATPAK_METADATA_KEY_NAME, NULL);
          priv->runtime = g_key_file_get_string (priv->info,
                                                 FLATPAK_METADATA_GROUP_APPLICATION, FLATPAK_METADATA_KEY_RUNTIME, NULL);
        }
      else
        {
          priv->runtime = g_key_file_get_string (priv->info,
                                                 FLATPAK_METADATA_GROUP_RUNTIME, FLATPAK_METADATA_KEY_RUNTIME, NULL);
        }

      priv->arch = g_key_file_get_string (priv->info,
                                          FLATPAK_METADATA_GROUP_INSTANCE, FLATPAK_METADATA_KEY_ARCH, NULL);
      priv->branch = g_key_file_get_string (priv->info,
                                            FLATPAK_METADATA_GROUP_INSTANCE, FLATPAK_METADATA_KEY_BRANCH, NULL);
      priv->commit = g_key_file_get_string (priv->info,
                                            FLATPAK_METADATA_GROUP_INSTANCE, FLATPAK_METADATA_KEY_APP_COMMIT, NULL);
      priv->runtime_commit = g_key_file_get_string (priv->info,
                                                    FLATPAK_METADATA_GROUP_INSTANCE, FLATPAK_METADATA_KEY_RUNTIME_COMMIT, NULL);
    }

  return self;
}

/*
 * Return the directory in which we create a numbered subdirectory per
 * instance.
 *
 * This directory is not shared with Flatpak apps, and we rely on this
 * for the sandbox boundary.
 *
 * This is currently the same as the
 * flatpak_instance_get_apps_directory(). We can distinguish between
 * instance IDs and app-IDs because instances are integers, and app-IDs
 * always contain at least one dot.
 */
char *
flatpak_instance_get_instances_directory (void)
{
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();

  return g_build_filename (user_runtime_dir, ".flatpak", NULL);
}

/*
 * Return the directory in which we create a subdirectory per
 * concurrently running Flatpak app-ID to store app-specific data that
 * is common to all instances of the same app.
 *
 * This directory is not shared with Flatpak apps, and we rely on this
 * for the sandbox boundary.
 *
 * This is currently the same as the
 * flatpak_instance_get_instances_directory(). We can distinguish between
 * instance IDs and app-IDs because instances are integers, and app-IDs
 * always contain at least one dot.
 */
char *
flatpak_instance_get_apps_directory (void)
{
  return flatpak_instance_get_instances_directory ();
}

/*
 * @app_id: $FLATPAK_ID
 * @lock_fd_out: (out) (not optional): Used to return a lock on the
 *  per-app directories
 * @lock_path_out: (out) (not optional): Used to return the path to the
 *  lock file, suitable for bind-mounting into the container
 *
 * Create a per-app directory and take out a lock on it.
 */
gboolean
flatpak_instance_ensure_per_app_dir (const char *app_id,
                                     int *lock_fd_out,
                                     char **lock_path_out,
                                     GError **error)
{
  glnx_autofd int lock_fd = -1;
  g_autofree char *lock_path = NULL;
  g_autofree char *per_app_parent = NULL;
  g_autofree char *per_app_dir = NULL;
  struct flock non_exclusive_lock =
  {
    .l_type = F_RDLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0
  };

  g_return_val_if_fail (app_id != NULL, FALSE);
  g_return_val_if_fail (lock_fd_out != NULL, FALSE);
  g_return_val_if_fail (*lock_fd_out == -1, FALSE);
  g_return_val_if_fail (lock_path_out != NULL, FALSE);
  g_return_val_if_fail (*lock_path_out == NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  per_app_parent = flatpak_instance_get_apps_directory ();
  per_app_dir = g_build_filename (per_app_parent, app_id, NULL);
  lock_path = g_build_filename (per_app_dir, ".ref", NULL);

  if (g_mkdir_with_parents (per_app_dir, 0700) != 0)
    return glnx_throw_errno_prefix (error,
                                    _("Unable to create directory %s"),
                                    per_app_dir);

  /* Take a file lock inside the shared directory, and hold it during
   * setup and in bwrap. We never delete the directory itself, or the
   * lock file that it contains (that would defeat the locking scheme).
   * Anyone cleaning up other members of per_app_dir must first verify
   * that it contains the lock file .ref, and take out an exclusive lock
   * on it. */
  lock_fd = open (lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

  /* As with the per-instance directories, there's a race here, because
   * we can't atomically open and lock the lockfile. We work around
   * that by only doing GC if the lockfile is "old".
   *
   * If we can't get the lock immediately, that'll be because some other
   * process is trying to carry out garbage-collection, so we wait
   * for it to finish. */
  if (lock_fd < 0 ||
      fcntl (lock_fd, F_SETLKW, &non_exclusive_lock) != 0)
    return glnx_throw_errno_prefix (error,
                                    _("Unable to lock %s"),
                                    lock_path);

  *lock_fd_out = glnx_steal_fd (&lock_fd);
  *lock_path_out = g_steal_pointer (&lock_path);
  return TRUE;
}

/*
 * @app_id: $FLATPAK_ID
 * @per_app_dir_lock_fd: Used to prove that we have already taken out
 *  a per-app non-exclusive lock to stop this directory from being
 *  garbage-collected
 * @shared_tmp: (out) (not optional) (not nullable): Used to return
 *  the path to the shared /tmp
 *
 * Create the per-app /tmp.
 */
gboolean
flatpak_instance_ensure_per_app_tmp (const char *app_id,
                                     int per_app_dir_lock_fd,
                                     char **shared_tmp_out,
                                     GError **error)
{
  g_autofree char *per_app_parent = NULL;
  g_autofree char *shared_tmp = NULL;

  g_return_val_if_fail (app_id != NULL, FALSE);
  g_return_val_if_fail (shared_tmp_out != NULL, FALSE);
  g_return_val_if_fail (*shared_tmp_out == NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* We don't actually do anything with this, we just pass it in here as
   * proof that we already set up the directory that contains the lock
   * file for per-app things, to force us to get the sequence right */
  g_return_val_if_fail (per_app_dir_lock_fd >= 0, FALSE);

  per_app_parent = flatpak_instance_get_apps_directory ();
  shared_tmp = g_build_filename (per_app_parent, app_id, "tmp", NULL);

  if (g_mkdir_with_parents (shared_tmp, 0700) != 0)
    return glnx_throw_errno_prefix (error,
                                    _("Unable to create directory %s"),
                                    shared_tmp);

  *shared_tmp_out = g_steal_pointer (&shared_tmp);
  return TRUE;
}

/*
 * @app_id: $FLATPAK_ID
 * @per_app_dir_lock_fd: Used to prove that we have already taken out
 *  a per-app non-exclusive lock to stop this directory from being
 *  garbage-collected
 * @shared_dir_out: (out) (not optional) (not nullable): Used to return
 *  the path to the shared $XDG_RUNTIME_DIR
 *
 * Create a per-app $XDG_RUNTIME_DIR.
 */
gboolean
flatpak_instance_ensure_per_app_xdg_runtime_dir (const char *app_id,
                                                 int per_app_dir_lock_fd,
                                                 char **shared_dir_out,
                                                 GError **error)
{
  g_autofree char *per_app_parent = NULL;
  g_autofree char *shared_dir = NULL;

  g_return_val_if_fail (app_id != NULL, FALSE);
  g_return_val_if_fail (shared_dir_out != NULL, FALSE);
  g_return_val_if_fail (*shared_dir_out == NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* We don't actually do anything with this, we just pass it in here as
   * proof that we already set up the directory that contains the lock
   * file for per-app things, to force us to get the sequence right */
  g_return_val_if_fail (per_app_dir_lock_fd >= 0, FALSE);

  per_app_parent = flatpak_instance_get_apps_directory ();
  shared_dir = g_build_filename (per_app_parent, app_id, "xdg-run", NULL);

  if (g_mkdir_with_parents (shared_dir, 0700) != 0)
    return glnx_throw_errno_prefix (error,
                                    _("Unable to create directory %s"),
                                    shared_dir);

  *shared_dir_out = g_steal_pointer (&shared_dir);
  return TRUE;
}

/*
 * @host_dir_out: (not optional): used to return the directory on the host
 *  system representing this instance
 * @lock_fd_out: (not optional): used to return a non-exclusive (read) lock
 *  on the lockdirectory on the host-file
 */
char *
flatpak_instance_allocate_id (char **host_dir_out,
                              int *lock_fd_out)
{
  g_autofree char *base_dir = flatpak_instance_get_instances_directory ();
  int count;

  g_return_val_if_fail (host_dir_out != NULL, NULL);
  g_return_val_if_fail (*host_dir_out == NULL, NULL);
  g_return_val_if_fail (lock_fd_out != NULL, NULL);
  g_return_val_if_fail (*lock_fd_out == -1, NULL);

  g_mkdir_with_parents (base_dir, 0755);

  flatpak_instance_iterate_all_and_gc (NULL);

  for (count = 0; count < 1000; count++)
    {
      g_autofree char *instance_id = NULL;
      g_autofree char *instance_dir = NULL;

      instance_id = g_strdup_printf ("%u", g_random_int ());

      instance_dir = g_build_filename (base_dir, instance_id, NULL);

      /* We use an atomic mkdir to ensure the instance id is unique */
      if (mkdir (instance_dir, 0755) == 0)
        {
          g_autofree char *lock_file = g_build_filename (instance_dir, ".ref", NULL);
          glnx_autofd int lock_fd = -1;
          struct flock l = {
            .l_type = F_RDLCK,
            .l_whence = SEEK_SET,
            .l_start = 0,
            .l_len = 0
          };

          /* Then we take a file lock inside the dir, hold that during
           * setup and in bwrap. Anyone trying to clean up unused
           * directories need to first verify that there is a .ref
           * file and take a write lock on .ref to ensure its not in
           * use. */
          lock_fd = open (lock_file, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
          /* There is a tiny race here between the open creating the file and the lock succeeding.
             We work around that by only gc:ing "old" .ref files */
          if (lock_fd != -1 && fcntl (lock_fd, F_SETLK, &l) == 0)
            {
              *lock_fd_out = glnx_steal_fd (&lock_fd);
              g_debug ("Allocated instance id %s", instance_id);
              *host_dir_out = g_steal_pointer (&instance_dir);
              return g_steal_pointer (&instance_id);
            }
        }
    }

  return NULL;
}

FlatpakInstance *
flatpak_instance_new_for_id (const char *id)
{
  g_autofree char *base_dir = flatpak_instance_get_instances_directory ();
  g_autofree char *dir = NULL;

  dir = g_build_filename (base_dir, id, NULL);
  return flatpak_instance_new (dir);
}

/*
 * The @error is not intended to be user-facing, and is there for
 * testing/debugging.
 */
static gboolean
flatpak_instance_gc_per_app_dirs (const char *instance_id,
                                  GError **error)
{
  g_autofree char *per_instance_parent = NULL;
  g_autofree char *per_app_parent = NULL;
  g_autofree char *app_id = NULL;
  g_autofree char *instance_dir = NULL;
  g_autofree char *per_app_dir = NULL;
  glnx_autofd int per_app_dir_fd = -1;
  glnx_autofd int per_app_dir_lock_fd = -1;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GKeyFile) key_file = NULL;
  struct flock exclusive_lock =
  {
    .l_type = F_WRLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
   .l_len = 0,
  };
  struct stat statbuf;

  per_instance_parent = flatpak_instance_get_instances_directory ();
  per_app_parent = flatpak_instance_get_apps_directory ();

  instance_dir = g_build_filename (per_instance_parent, instance_id, NULL);
  key_file = get_instance_info (instance_dir);

  if (key_file == NULL)
    return glnx_throw (error, "Unable to load keyfile %s/info", instance_dir);

  if (g_key_file_has_group (key_file, FLATPAK_METADATA_GROUP_APPLICATION))
    app_id = g_key_file_get_string (key_file,
                                    FLATPAK_METADATA_GROUP_APPLICATION,
                                    FLATPAK_METADATA_KEY_NAME, error);
  else
    app_id = g_key_file_get_string (key_file,
                                    FLATPAK_METADATA_GROUP_RUNTIME,
                                    FLATPAK_METADATA_KEY_RUNTIME, error);

  if (app_id == NULL)
    {
      g_prefix_error (error, "%s/info: ", instance_dir);
      return FALSE;
    }

  /* Take an exclusive lock so we don't race with other instances */

  per_app_dir = g_build_filename (per_app_parent, app_id, NULL);
  per_app_dir_fd = openat (AT_FDCWD, per_app_dir,
                           O_PATH | O_DIRECTORY | O_CLOEXEC);

  if (per_app_dir_fd < 0)
    return glnx_throw_errno_prefix (error, "open %s", per_app_dir);

  per_app_dir_lock_fd = openat (per_app_dir_fd, ".ref",
                                O_RDWR | O_CREAT | O_CLOEXEC, 0600);

  if (per_app_dir_lock_fd < 0)
    return glnx_throw_errno_prefix (error, "open %s/.ref", per_app_dir);

  /* We don't wait for the lock: we're just doing GC opportunistically.
   * If at least one instance is running, then we'll fail to get the
   * exclusive lock. */
  if (fcntl (per_app_dir_lock_fd, F_SETLK, &exclusive_lock) < 0)
    return glnx_throw_errno_prefix (error, "lock %s/.ref", per_app_dir);

  if (fstat (per_app_dir_lock_fd, &statbuf) < 0)
    return glnx_throw_errno_prefix (error, "fstat %s/.ref", per_app_dir);

  /* Only gc if created at least 3 secs ago, to work around the equivalent
   * of the race mentioned in flatpak_instance_allocate_id() */
  if (statbuf.st_mtime + 3 >= time (NULL))
    return glnx_throw (error, "lock file too recent, avoiding race condition");

  g_debug ("Cleaning up per-app-ID state for %s", app_id);

  if (!glnx_shutil_rm_rf_at (per_app_dir_fd, "tmp", NULL, &local_error))
    {
      g_debug ("Unable to clean up %s/tmp: %s", per_app_dir,
               local_error->message);
      g_clear_error (&local_error);
    }

  /* Deliberately don't clean up the .ref lock file or the directory itself.
   * If we did that, we'd defeat our locking scheme, because a concurrent
   * process could open the .ref file just before we unlink it. */
  return TRUE;
}

void
flatpak_instance_iterate_all_and_gc (GPtrArray *out_instances)
{
  g_autofree char *base_dir = flatpak_instance_get_instances_directory ();
  g_auto(GLnxDirFdIterator) iter = { 0 };
  struct dirent *dent;

  /* Clean up unused instances */
  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, base_dir, FALSE, &iter, NULL))
    return;

  while (TRUE)
    {
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iter, &dent, NULL, NULL))
        break;

      if (dent == NULL)
        break;

      if (!flatpak_str_is_integer (dent->d_name))
        continue;

      if (dent->d_type == DT_DIR)
        {
          g_autofree char *ref_file = g_strconcat (dent->d_name, "/.ref", NULL);
          struct stat statbuf;
          struct flock l = {
            .l_type = F_WRLCK,
            .l_whence = SEEK_SET,
            .l_start = 0,
            .l_len = 0
          };
          glnx_autofd int lock_fd = openat (iter.fd, ref_file, O_RDWR | O_CLOEXEC);
          if (lock_fd != -1 &&
              fstat (lock_fd, &statbuf) == 0 &&
              /* Only gc if created at least 3 secs ago, to work around race mentioned in
               * flatpak_instance_allocate_id() */
              statbuf.st_mtime + 3 < time (NULL) &&
              fcntl (lock_fd, F_GETLK, &l) == 0 &&
              l.l_type == F_UNLCK)
            {
              /* The instance is not used, remove it */
              g_debug ("Cleaning up unused container id %s", dent->d_name);
              flatpak_instance_gc_per_app_dirs (dent->d_name, NULL);
              glnx_shutil_rm_rf_at (iter.fd, dent->d_name, NULL, NULL);
              continue;
            }

          if (out_instances != NULL)
            g_ptr_array_add (out_instances, flatpak_instance_new_for_id (dent->d_name));
        }
    }
}

/**
 * flatpak_instance_get_all:
 *
 * Gets FlatpakInstance objects for all running sandboxes in the current session.
 *
 * Returns: (transfer full) (element-type FlatpakInstance): a #GPtrArray of
 *   #FlatpakInstance objects
 *
 * Since: 1.1
 */
GPtrArray *
flatpak_instance_get_all (void)
{
  g_autoptr(GPtrArray) instances = NULL;

  instances = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  flatpak_instance_iterate_all_and_gc (instances);

  return g_steal_pointer (&instances);
}

/**
 * flatpak_instance_is_running:
 * @self: a #FlatpakInstance
 *
 * Finds out if the sandbox represented by @self is still running.
 *
 * Returns: %TRUE if the sandbox is still running
 */
gboolean
flatpak_instance_is_running (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  if (kill (priv->pid, 0) == 0)
    return TRUE;

  return FALSE;
}
