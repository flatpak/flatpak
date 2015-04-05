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

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ftw.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_command;
static char **opt_allow;

static GOptionEntry options[] = {
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, "Command to set", "COMMAND" },
  { "allow", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_allow, "Environment options to set to true", "KEY" },
  { NULL }
};

static gboolean
export_dir (int            source_parent_fd,
            const char    *source_name,
            const char    *source_relpath,
            int            destination_parent_fd,
            const char    *destination_name,
            const char    *required_prefix,
            GCancellable  *cancellable,
            GError       **error)
{
  gboolean ret = FALSE;
  int res;
  g_auto(GLnxDirFdIterator) source_iter = {0};
  glnx_fd_close int destination_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  do
    res = mkdirat (destination_parent_fd, destination_name, 0777);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  if (!gs_file_open_dir_fd_at (destination_parent_fd, destination_name,
                               &destination_dfd,
                               cancellable, error))
    goto out;

  while (TRUE)
    {
      struct stat stbuf;
      g_autofree char *source_printable = NULL;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            continue;
          else
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          g_autofree gchar *child_relpath = g_strconcat (source_relpath, dent->d_name, "/", NULL);

          if (!export_dir (source_iter.fd, dent->d_name, child_relpath, destination_dfd, dent->d_name,
                           required_prefix, cancellable, error))
            goto out;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          source_printable = g_build_filename (source_relpath, dent->d_name, NULL);


          if (!xdg_app_has_name_prefix (dent->d_name, required_prefix))
            {
              g_print ("Not exporting %s, wrong prefix\n", source_printable);
              continue;
            }

          g_print ("Exporting %s\n", source_printable);

          if (!glnx_file_copy_at (source_iter.fd, dent->d_name, &stbuf,
                                  destination_dfd, dent->d_name,
                                  GLNX_FILE_COPY_NOXATTRS,
                                  cancellable,
                                  error))
            goto out;
        }
      else
        {
          source_printable = g_build_filename (source_relpath, dent->d_name, NULL);
          g_print ("Not exporting non-regular file %s\n", source_printable);
        }
    }

  ret = TRUE;
 out:

  return ret;
}

static gboolean
copy_exports (GFile    *source,
              GFile    *destination,
              const char *source_prefix,
              const char *required_prefix,
              GCancellable  *cancellable,
              GError       **error)
{
  gboolean ret = FALSE;

  if (!gs_file_ensure_directory (destination, TRUE, cancellable, error))
    goto out;

  /* The fds are closed by this call */
  if (!export_dir (AT_FDCWD, gs_file_get_path_cached (source), source_prefix,
                   AT_FDCWD, gs_file_get_path_cached (destination),
                   required_prefix, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static gboolean
collect_exports (GFile *base, const char *app_id, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) export = NULL;
  const char *paths[] = {
    "share/applications",                 /* Copy desktop files */
    "share/icons/hicolor",                /* Icons */
    "share/dbus-1/services",              /* D-Bus service files */
    "share/gnome-shell/search-providers", /* Search providers */
    NULL,
  };
  int i;

  files = g_file_get_child (base, "files");
  export = g_file_get_child (base, "export");

  if (!gs_file_ensure_directory (export, TRUE, cancellable, error))
    goto out;

  for (i = 0; paths[i]; i++)
    {
      g_autoptr(GFile) src = NULL;
      src = g_file_resolve_relative_path (files, paths[i]);
      if (g_file_query_exists (src, cancellable))
        {
          g_debug ("Exporting from %s", paths[i]);
          g_autoptr(GFile) dest = NULL;
          g_autoptr(GFile) dest_parent = NULL;
          dest = g_file_resolve_relative_path (export, paths[i]);
          dest_parent = g_file_get_parent (dest);
          g_debug ("Ensuring export/%s parent exists", paths[i]);
          if (!gs_file_ensure_directory (dest_parent, TRUE, cancellable, error))
            goto out;
          g_debug ("Copying from files/%s", paths[i]);
          if (!copy_exports (src, dest, paths[i], app_id, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;

  g_assert_no_error (*error);
out:
  return ret;
}

static gboolean
update_metadata (GFile *base, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  GError *temp_error = NULL;
  const char *environment_keys[] = {
    "x11", "wayland", "ipc", "pulseaudio", "system-dbus", "session-dbus",
    "network", "host-fs", "homedir", "dri", NULL
  };
  const char *key;
  int i;

  metadata = g_file_get_child (base, "metadata");
  if (!g_file_query_exists (metadata, cancellable))
    goto out;

  path = g_file_get_path (metadata);
  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    goto out;

  if (g_key_file_has_key (keyfile, "Application", "command", NULL))
    {
      g_debug ("Command key is present");

      if (opt_command)
        g_key_file_set_string (keyfile, "Application", "command", opt_command);
    }
  else if (opt_command)
    {
      g_debug ("Using explicitly provided command %s", opt_command);

      g_key_file_set_string (keyfile, "Application", "command", opt_command);
    }
  else
    {
      g_autofree char *command = NULL;
      g_autoptr(GFile) bin_dir = NULL;
      g_autoptr(GFileEnumerator) bin_enum = NULL;
      g_autoptr(GFileInfo) child_info = NULL;

      g_debug ("Looking for executables");

      bin_dir = g_file_resolve_relative_path (base, "files/bin");
      if (g_file_query_exists (bin_dir, cancellable))
        {
          bin_enum = g_file_enumerate_children (bin_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                cancellable, error);
          if (!bin_enum)
            goto out;

          while ((child_info = g_file_enumerator_next_file (bin_enum, cancellable, &temp_error)))
            {
              if (command != NULL)
                {
                  g_print ("More than one executable found\n");
                  break;
                }
              command = g_strdup (g_file_info_get_name (child_info));
            }
          if (temp_error != NULL)
            goto out;
        }

      if (command)
        {
          g_print ("Using %s as command\n", command);
          g_key_file_set_string (keyfile, "Application", "command", command);
        }
      else
        {
          g_print ("No executable found\n");
        }
    }

  g_debug ("Setting environment");

  for (i = 0; environment_keys[i]; i++)
    {
      key = environment_keys[i];
      g_key_file_set_boolean (keyfile, "Environment", key, FALSE);
    }

  if (opt_allow)
    {
      for (i = 0; opt_allow[i]; i++)
        {
          key = opt_allow[i];
          if (!g_strv_contains (environment_keys, key))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unknown Environment key %s", key);
              goto out;
            }

          g_key_file_set_boolean (keyfile, "Environment", key, TRUE);
        }
    }

  if (!g_key_file_save_to_file (keyfile, path, error))
    goto out;

  ret = TRUE;

out:
  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  return ret;
}

gboolean
xdg_app_builtin_build_finish (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) files_dir = NULL;
  g_autoptr(GFile) export_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(XdgAppDir) user_dir = NULL;
  g_autoptr(XdgAppDir) system_dir = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *app_id = NULL;
  gsize metadata_size;
  const char *directory;
  g_autoptr(GKeyFile) metakey = NULL;

  context = g_option_context_new ("DIRECTORY - Convert a directory to a bundle");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "DIRECTORY must be specified", error);
      goto out;
    }

  directory = argv[1];

  base = g_file_new_for_commandline_arg (directory);

  files_dir = g_file_get_child (base, "files");
  export_dir = g_file_get_child (base, "export");
  metadata_file = g_file_get_child (base, "metadata");

  if (!g_file_query_exists (files_dir, cancellable) ||
      !g_file_query_exists (metadata_file, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Build directory %s not initialized", directory);
      goto out;
    }

  if (!g_file_load_contents (metadata_file, cancellable, &metadata_contents, &metadata_size, NULL, error))
    goto out;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    goto out;

  app_id = g_key_file_get_string (metakey, "Application", "name", error);
  if (app_id == NULL)
    goto out;

  if (g_file_query_exists (export_dir, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Build directory %s already finalized", directory);
      goto out;
    }

  g_debug ("Collecting exports");
  if (!collect_exports (base, app_id, cancellable, error))
    goto out;

  g_debug ("Updating metadata");
  if (!update_metadata (base, cancellable, error))
    goto out;

  g_print ("Please review the exported files and the metadata\n");

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
