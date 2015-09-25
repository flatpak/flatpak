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

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;
static char *opt_file;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to make current for", "ARCH" },
  { "file", 0, 0, G_OPTION_ARG_STRING, &opt_file, "Write to file instead of stdout", "PATH" },
  { NULL }
};

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>

typedef struct archive write_archive_t;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(write_archive_t, archive_write_free);

typedef struct archive_entry archive_entry_t;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(archive_entry_t, archive_entry_free);

static gboolean
dump_data (GFile *file,
           struct archive *archive,
           GCancellable               *cancellable,
           GError                    **error)
{
  char buffer[32*1024];
  g_autoptr(GFileInputStream) in = NULL;
  gssize in_buffer;

  in = g_file_read (file, cancellable, error);
  if (in == NULL)
    return FALSE;

  while (TRUE)
    {
      in_buffer = g_input_stream_read (G_INPUT_STREAM (in), buffer, sizeof (buffer), cancellable, error);
      if (in_buffer == -1)
        return FALSE;

      if (in_buffer == 0)
        break;

      if (archive_write_data (archive, buffer, in_buffer) < ARCHIVE_OK)
        return xdg_app_fail (error, "Can't write tar data");
    }

  return TRUE;
}

static gboolean
dump_files (GFile *dir,
            struct archive *archive,
            GCancellable *cancellable,
            char *parent,
            GError **error)
{
  g_autoptr(GFileEnumerator) fe = NULL;
  gboolean ret = TRUE;
  GFileType type;

  fe = g_file_enumerate_children (dir,
                                  "standard::name,standard::type,standard::is-symlink,standard::symlink-target,unix::mode,time::*",
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (fe == NULL)
    return FALSE;


  while (TRUE)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (fe, cancellable, error);
      g_autofree char *path = NULL;
      g_autoptr(GFile) child = NULL;
      guint32 mode;
      g_autoptr(archive_entry_t) entry = archive_entry_new2 (archive);

      if (!info)
        {
          if (error && *error != NULL)
            ret = FALSE;
          break;
        }

      type = g_file_info_get_file_type (info);
      mode = g_file_info_get_attribute_uint32 (info, "unix::mode");
      path = g_build_filename (parent, g_file_info_get_name (info), NULL);
      child = g_file_enumerator_get_child (fe, info);

      archive_entry_set_pathname (entry, path);
      archive_entry_set_uid(entry, 0);
      archive_entry_set_gid(entry, 0);
      archive_entry_set_perm(entry, mode & 0777);
      archive_entry_set_mtime(entry, 0, 0);

      switch (type)
        {
        case G_FILE_TYPE_SYMBOLIC_LINK:
          archive_entry_set_filetype (entry, AE_IFLNK);
          archive_entry_set_symlink (entry, g_file_info_get_symlink_target (info));
          break;

        case G_FILE_TYPE_REGULAR:
          archive_entry_set_filetype (entry, AE_IFREG);
          archive_entry_set_size(entry, g_file_info_get_size (info));
          break;

        case G_FILE_TYPE_DIRECTORY:
          archive_entry_set_filetype (entry, AE_IFDIR);
          break;

        default:
          g_error ("Unhandled type %d\n", type);
          break;
        }

      if (archive_write_header (archive, entry) < ARCHIVE_OK)
        return xdg_app_fail (error, "Can't write tar header");

      if (type == G_FILE_TYPE_REGULAR)
        {
          if (!dump_data (child, archive, cancellable, error))
            return FALSE;
        }

      if (archive_write_finish_entry (archive) < ARCHIVE_OK)
        return xdg_app_fail (error, "Can't finish tar entry");

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!dump_files (child, archive, cancellable, path, error))
            return FALSE;
        }
    }

  return ret;
}

static const char *extra_dirs[] = {
  "app",
  "dev",
  "home",
  "proc",
  "run",
  "run/host",
  "run/dbus",
  "run/media",
  "run/user",
  "sys",
  "usr",
  "tmp",
  "var",
};

static struct {const char *path; const char *target; } extra_symlinks[] = {
  {"bin", "usr/bin" },
  {"sbin", "usr/sbin" },
  {"etc", "usr/etc" },
  {"lib", "usr/lib" },
  {"lib32", "usr/lib32" },
  {"lib64", "usr/lib64" },
  {"var/run", "/run" },
  {"var/tmp", "/tmp" },
};

static gboolean
dump_runtime (GFile *root, GCancellable *cancellable, GError **error)
{
  int i;

  g_autoptr(write_archive_t) archive = NULL;
  g_autoptr(GFile) files = g_file_get_child (root, "files");

  archive = archive_write_new ();
  if (archive == NULL)
    return xdg_app_fail (error, "Can't allocate archive");

  if (archive_write_set_format_gnutar (archive) < ARCHIVE_OK)
    return xdg_app_fail (error, "Can't set tar format");

  if (archive_write_open_FILE (archive, stdout) < ARCHIVE_OK)
    return xdg_app_fail (error, "can't open stdout");

  for (i = 0; i < G_N_ELEMENTS(extra_dirs); i++)
    {
      g_autoptr(archive_entry_t) entry = archive_entry_new2 (archive);

      archive_entry_set_pathname (entry, extra_dirs[i]);
      archive_entry_set_uid(entry, 0);
      archive_entry_set_gid(entry, 0);
      archive_entry_set_perm(entry, 0755);
      archive_entry_set_mtime(entry, 0, 0);
      archive_entry_set_filetype (entry, AE_IFDIR);

      if (archive_write_header (archive, entry) < ARCHIVE_OK)
        return xdg_app_fail (error, "Can't write tar header");
    }

  for (i = 0; i < G_N_ELEMENTS(extra_symlinks); i++)
    {
      g_autoptr(archive_entry_t) entry = NULL;
      g_autoptr(GFile) dest = g_file_resolve_relative_path (files, extra_symlinks[i].target);

      if (g_str_has_prefix (extra_symlinks[i].target, "usr/"))
        {
          g_autoptr(GFile) dest = g_file_resolve_relative_path (files, extra_symlinks[i].target + 4);

          if (!g_file_query_exists (dest, cancellable))
            continue;
        }

      entry = archive_entry_new2 (archive);

      archive_entry_set_pathname (entry, extra_symlinks[i].path);
      archive_entry_set_uid(entry, 0);
      archive_entry_set_gid(entry, 0);
      archive_entry_set_perm(entry, 0755);
      archive_entry_set_mtime(entry, 0, 0);
      archive_entry_set_filetype (entry, AE_IFLNK);
      archive_entry_set_symlink (entry, extra_symlinks[i].target);

      if (archive_write_header (archive, entry) < ARCHIVE_OK)
        return xdg_app_fail (error, "Can't write tar header");
    }

  if (!dump_files (files, archive, cancellable, "usr", error))
    return FALSE;

  if (archive_write_close (archive) < ARCHIVE_OK)
    return xdg_app_fail (error, "can't close archive");

  return TRUE;
}
#endif

gboolean
xdg_app_builtin_dump_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  const char *runtime;
  const char *branch = "master";
  g_autofree char *ref = NULL;
  OstreeRepo *repo;
  g_autofree char *commit = NULL;
  g_autoptr(GFile) root = NULL;

  context = g_option_context_new ("RUNTIME BRANCH - Make branch of application current");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 3)
    return usage_error (context, "RUNTIME and BRANCH must be specified", error);

  runtime  = argv[1];
  branch = argv[2];

  if (!xdg_app_is_valid_name (runtime))
    return xdg_app_fail (error, "'%s' is not a valid name", runtime);

  if (!xdg_app_is_valid_branch (branch))
    return xdg_app_fail (error, "'%s' is not a valid branch name", branch);

  ref = xdg_app_build_runtime_ref (runtime, branch, opt_arch);

  repo = xdg_app_dir_get_repo (dir);

  if (!ostree_repo_read_commit (repo, ref,
                                &root, &commit, cancellable, error))
    return FALSE;

#ifdef HAVE_LIBARCHIVE
  return dump_runtime (root, cancellable, error);
#else
  return xdg_app_fail (error, "Build without libarchive");
#endif
}
