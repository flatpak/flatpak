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
#include "flatpak-instance-private.h"
#include "flatpak-enum-types.h"


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

const char *
flatpak_instance_get_id (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->id;
}

const char *
flatpak_instance_get_app (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->app;
}

const char *
flatpak_instance_get_arch (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->arch;
}

const char *
flatpak_instance_get_branch (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->branch;
}

const char *
flatpak_instance_get_commit (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->commit;
}

const char *
flatpak_instance_get_runtime (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->runtime;
}

const char *
flatpak_instance_get_runtime_commit (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->runtime_commit;
}

int
flatpak_instance_get_pid (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->pid;
}

int
flatpak_instance_get_child_pid (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  return priv->child_pid;
}

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

static FlatpakInstance *
flatpak_instance_new (const char *id)
{
  FlatpakInstance *self = g_object_new (flatpak_instance_get_type (), NULL);
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  priv->id = g_strdup (id);

  priv->dir = g_build_filename (g_get_user_runtime_dir (), ".flatpak", id, NULL);

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

          g_ptr_array_add (instances, flatpak_instance_new (dent->d_name));
        }
    }

  return g_steal_pointer (&instances);
}

gboolean
flatpak_instance_is_running (FlatpakInstance *self)
{
  FlatpakInstancePrivate *priv = flatpak_instance_get_instance_private (self);

  if (priv->dir)
    {
      g_autofree char *ref_file = g_strconcat (priv->dir, "/.ref", NULL);
      struct stat statbuf;
      struct flock l = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0
      };
      glnx_autofd int lock_fd = open (ref_file, O_RDWR | O_CLOEXEC);
      if (lock_fd != -1 &&
          fstat (lock_fd, &statbuf) == 0 &&
          /* Only gc if created at least 3 secs ago, to work around race mentioned in flatpak_run_allocate_id() */
          statbuf.st_mtime + 3 < time (NULL) &&
          fcntl (lock_fd, F_GETLK, &l) == 0 &&
          l.l_type == F_UNLCK)
        {
          /* The instance is not used, remove it */
          g_debug ("Cleaning up unused container id %s", priv->id);
          glnx_shutil_rm_rf_at (-1, priv->dir, NULL, NULL);

          g_clear_pointer (&priv->dir, g_free);
        }
    }

  return priv->dir != NULL;
}

