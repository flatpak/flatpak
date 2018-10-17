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
 * Returns: the application ID
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
      priv->app = g_key_file_get_string (priv->info,
          FLATPAK_METADATA_GROUP_APPLICATION, FLATPAK_METADATA_KEY_NAME, NULL);
      priv->runtime = g_key_file_get_string (priv->info,
          FLATPAK_METADATA_GROUP_APPLICATION, FLATPAK_METADATA_KEY_RUNTIME, NULL);

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

static FlatpakInstance *
flatpak_instance_new_for_id (const char *id)
{
  g_autofree char *dir = NULL;

  dir = g_build_filename (g_get_user_runtime_dir (), ".flatpak", id, NULL);
  return flatpak_instance_new (dir);
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
  g_autofree char *base_dir = NULL;
  g_auto(GLnxDirFdIterator) iter = { 0 };
  struct dirent *dent;

  instances = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  base_dir = g_build_filename (g_get_user_runtime_dir (), ".flatpak", NULL);

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, base_dir, FALSE, &iter, NULL))
    return g_steal_pointer (&instances);

  while (TRUE)
    {
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iter, &dent, NULL, NULL))
        break;

      if (dent == NULL)
        break;

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
              /* Only gc if created at least 3 secs ago, to work around race mentioned in flatpak_run_allocate_id() */
              statbuf.st_mtime + 3 < time (NULL) &&
              fcntl (lock_fd, F_GETLK, &l) == 0 &&
              l.l_type == F_UNLCK)
            {
              /* The instance is not used, remove it */
              g_debug ("Cleaning up unused container id %s", dent->d_name);
              glnx_shutil_rm_rf_at (iter.fd, dent->d_name, NULL, NULL);
              continue;
            }

          g_ptr_array_add (instances, flatpak_instance_new_for_id (dent->d_name));
        }
    }

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

