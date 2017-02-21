/*
 * Copyright © 2014 Red Hat, Inc
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

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ftw.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-run.h"

static char *opt_command;
static char *opt_require_version;
static char **opt_extra_data;
static char **opt_extensions;
static gboolean opt_no_exports;
static char *opt_sdk;
static char *opt_runtime;

static GOptionEntry options[] = {
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, N_("Command to set"), N_("COMMAND") },
  { "require-version", 0, 0, G_OPTION_ARG_STRING, &opt_require_version, N_("Flatpak version to require"), N_("MAJOR.MINOR.MICRO") },
  { "no-exports", 0, 0, G_OPTION_ARG_NONE, &opt_no_exports, N_("Don't process exports"), NULL },
  { "extra-data", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_extra_data, N_("Extra data info") },
  { "extension", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_extensions, N_("Add extension point info"),  N_("NAME=VARIABLE[=VALUE]") },
  { "sdk", 0, 0, G_OPTION_ARG_STRING, &opt_sdk, N_("Change the sdk used for the app"),  N_("SDK") },
  { "runtime", 0, 0, G_OPTION_ARG_STRING, &opt_runtime, N_("Change the runtime used for the app"),  N_("RUNTIME") },
  { NULL }
};

static gboolean
export_dir (int           source_parent_fd,
            const char   *source_name,
            const char   *source_relpath,
            int           destination_parent_fd,
            const char   *destination_name,
            const char   *required_prefix,
            GCancellable *cancellable,
            GError      **error)
{
  int res;

  g_auto(GLnxDirFdIterator) source_iter = {0};
  glnx_fd_close int destination_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    return FALSE;

  do
    res = mkdirat (destination_parent_fd, destination_name, 0755);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  if (!glnx_opendirat (destination_parent_fd, destination_name, TRUE,
                       &destination_dfd, error))
    return FALSE;

  while (TRUE)
    {
      struct stat stbuf;
      g_autofree char *source_printable = NULL;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            {
              continue;
            }
          else
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }

      /* Don't export any hidden files or backups */
      if (g_str_has_prefix (dent->d_name, ".") ||
          g_str_has_suffix (dent->d_name, "~"))
        continue;

      if (S_ISDIR (stbuf.st_mode))
        {
          g_autofree gchar *child_relpath = g_build_filename (source_relpath, dent->d_name, NULL);

          if (!export_dir (source_iter.fd, dent->d_name, child_relpath, destination_dfd, dent->d_name,
                           required_prefix, cancellable, error))
            return FALSE;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          source_printable = g_build_filename (source_relpath, dent->d_name, NULL);


          if (!flatpak_has_name_prefix (dent->d_name, required_prefix))
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
            return FALSE;
        }
      else
        {
          source_printable = g_build_filename (source_relpath, dent->d_name, NULL);
          g_debug ("Not exporting non-regular file %s\n", source_printable);
        }
    }

  /* Try to remove the directory, as we don't want to export empty directories.
   * However, don't fail if the unlink fails due to the directory not being empty */
  do
    res = unlinkat (destination_parent_fd, destination_name, AT_REMOVEDIR);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != ENOTEMPTY)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
copy_exports (GFile        *source,
              GFile        *destination,
              const char   *source_prefix,
              const char   *required_prefix,
              GCancellable *cancellable,
              GError      **error)
{
  if (!flatpak_mkdir_p (destination, cancellable, error))
    return FALSE;

  /* The fds are closed by this call */
  if (!export_dir (AT_FDCWD, flatpak_file_get_path_cached (source), source_prefix,
                   AT_FDCWD, flatpak_file_get_path_cached (destination),
                   required_prefix, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
collect_exports (GFile *base, const char *app_id, GCancellable *cancellable, GError **error)
{
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) export = NULL;
  const char *paths[] = {
    "share/applications",                 /* Copy desktop files */
    "share/mime/packages",                /* Copy MIME Type files */
    "share/icons",                        /* Icons */
    "share/dbus-1/services",              /* D-Bus service files */
    "share/gnome-shell/search-providers", /* Search providers */
    NULL,
  };
  int i;

  files = g_file_get_child (base, "files");

  export = g_file_get_child (base, "export");

  if (!flatpak_mkdir_p (export, cancellable, error))
    return FALSE;

  if (opt_no_exports)
    return TRUE;

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
          if (!flatpak_mkdir_p (dest_parent, cancellable, error))
            return FALSE;
          g_debug ("Copying from files/%s", paths[i]);
          if (!copy_exports (src, dest, paths[i], app_id, cancellable, error))
            return FALSE;
        }
    }

  g_assert_no_error (*error);
  return TRUE;
}

static gboolean
update_metadata (GFile *base, FlatpakContext *arg_context, gboolean is_runtime, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) metadata = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  GError *temp_error = NULL;
  const char *group;
  int i;

  metadata = g_file_get_child (base, "metadata");
  if (!g_file_query_exists (metadata, cancellable))
    goto out;

  if (is_runtime)
    group = "Runtime";
  else
    group = "Application";

  path = g_file_get_path (metadata);
  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    goto out;

  if (opt_sdk != NULL || opt_runtime != NULL)
    {
      g_autofree char *old_runtime = g_key_file_get_string (keyfile, group, "runtime", NULL);
      g_autofree char *old_sdk = g_key_file_get_string (keyfile, group, "sdk", NULL);
      const char *old_sdk_arch = NULL;
      const char *old_runtime_arch = NULL;
      const char *old_sdk_branch = NULL;
      const char *old_runtime_branch = NULL;
      g_auto(GStrv) old_sdk_parts = NULL;
      g_auto(GStrv) old_runtime_parts = NULL;

      if (old_sdk)
        old_sdk_parts = g_strsplit (old_sdk, "/", 3);

      if (old_runtime)
        old_runtime_parts = g_strsplit (old_runtime, "/", 3);

      if (old_sdk_parts != NULL)
        {
          old_sdk_arch = old_sdk_parts[1];
          old_sdk_branch = old_sdk_parts[2];
        }
      if (old_runtime_parts != NULL)
        {
          old_runtime_arch = old_runtime_parts[1];
          old_runtime_branch = old_runtime_parts[2];

          /* Use the runtime as fallback if sdk is not specified */
          if (old_sdk_parts == NULL)
            {
              old_sdk_arch = old_runtime_parts[1];
              old_sdk_branch = old_runtime_parts[2];
            }
        }

      if (opt_sdk)
        {
          g_autofree char *id = NULL;
          g_autofree char *arch = NULL;
          g_autofree char *branch = NULL;
          g_autofree char *ref = NULL;
          FlatpakKinds kinds;

          if (!flatpak_split_partial_ref_arg (opt_sdk, FLATPAK_KINDS_RUNTIME,
                                              old_sdk_arch, old_sdk_branch ? old_sdk_branch : "master",
                                              &kinds, &id, &arch, &branch, error))
            return FALSE;

          ref = flatpak_build_untyped_ref (id, branch, arch);
          g_key_file_set_string (keyfile, group, "sdk", ref);
        }

      if (opt_runtime)
        {
          g_autofree char *id = NULL;
          g_autofree char *arch = NULL;
          g_autofree char *branch = NULL;
          g_autofree char *ref = NULL;
          FlatpakKinds kinds;

          if (!flatpak_split_partial_ref_arg (opt_runtime, FLATPAK_KINDS_RUNTIME,
                                              old_runtime_arch, old_runtime_branch ? old_runtime_branch : "master",
                                              &kinds, &id, &arch, &branch, error))
            return FALSE;

          ref = flatpak_build_untyped_ref (id, branch, arch);
          g_key_file_set_string (keyfile, group, "runtime", ref);
        }
    }

  if (!is_runtime)
    {
      if (g_key_file_has_key (keyfile, group, "command", NULL))
        {
          g_debug ("Command key is present");

          if (opt_command)
            g_key_file_set_string (keyfile, group, "command", opt_command);
        }
      else if (opt_command)
        {
          g_debug ("Using explicitly provided command %s", opt_command);

          g_key_file_set_string (keyfile, group, "command", opt_command);
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
              g_key_file_set_string (keyfile, group, "command", command);
            }
          else
            {
              g_print ("No executable found\n");
            }
        }
    }

  if (opt_require_version)
    g_key_file_set_string (keyfile, group, "required-flatpak", opt_require_version);

  app_context = flatpak_context_new ();
  if (!flatpak_context_load_metadata (app_context, keyfile, error))
    goto out;
  flatpak_context_merge (app_context, arg_context);
  flatpak_context_save_metadata (app_context, FALSE, keyfile);

  for (i = 0; opt_extra_data != NULL && opt_extra_data[i] != NULL; i++)
    {
      char *extra_data = opt_extra_data[i];
      g_auto(GStrv) elements = NULL;
      g_autofree char *suffix = NULL;
      g_autofree char *uri_key = NULL;
      g_autofree char *name_key = NULL;
      g_autofree char *size_key = NULL;
      g_autofree char *installed_size_key = NULL;
      g_autofree char *checksum_key = NULL;

      if (i == 0)
        suffix = g_strdup ("");
      else
        suffix = g_strdup_printf ("%d", i);

      elements = g_strsplit (extra_data, ":", 5);
      if (g_strv_length (elements) != 5)
        {
          flatpak_fail (error, _("Too few elements in --extra-data argument %s"), extra_data);
          goto out;
        }

      uri_key = g_strconcat ("uri", suffix, NULL);
      name_key = g_strconcat ("name", suffix, NULL);
      checksum_key = g_strconcat ("checksum", suffix, NULL);
      size_key = g_strconcat ("size", suffix, NULL);
      installed_size_key = g_strconcat ("installed-size", suffix, NULL);

      if (strlen (elements[0]) > 0)
        g_key_file_set_string (keyfile, "Extra Data", name_key, elements[0]);
      g_key_file_set_string (keyfile, "Extra Data", checksum_key, elements[1]);
      g_key_file_set_string (keyfile, "Extra Data", size_key, elements[2]);
      if (strlen (elements[3]) > 0)
        g_key_file_set_string (keyfile, "Extra Data", installed_size_key, elements[3]);
      g_key_file_set_string (keyfile, "Extra Data", uri_key, elements[4]);
    }

  for (i = 0; opt_extensions != NULL && opt_extensions[i] != NULL; i++)
    {
      g_auto(GStrv) elements = NULL;
      g_autofree char *groupname = NULL;

      elements = g_strsplit (opt_extensions[i], "=", 3);
      if (g_strv_length (elements) < 2)
        {
          flatpak_fail (error, _("Too few elements in --extension argument %s, format should be NAME=VAR[=VALUE]"), opt_extensions[i]);
          goto out;
        }

      groupname = g_strdup_printf ("Extension %s", elements[0]);

      g_key_file_set_string (keyfile, groupname, elements[1], elements[2] ? elements[2] : "true");
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
flatpak_builtin_build_finish (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) files_dir = NULL;
  g_autoptr(GFile) export_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *id = NULL;
  gboolean is_runtime = FALSE;
  gsize metadata_size;
  const char *directory;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(FlatpakContext) arg_context = NULL;

  context = g_option_context_new (_("DIRECTORY - Finalize a build directory"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("DIRECTORY must be specified"), error);

  directory = argv[1];

  base = g_file_new_for_commandline_arg (directory);

  files_dir = g_file_get_child (base, "files");
  export_dir = g_file_get_child (base, "export");
  metadata_file = g_file_get_child (base, "metadata");

  if (!g_file_query_exists (files_dir, cancellable) ||
      !g_file_query_exists (metadata_file, cancellable))
    return flatpak_fail (error, _("Build directory %s not initialized"), directory);

  if (!g_file_load_contents (metadata_file, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return FALSE;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return FALSE;

  id = g_key_file_get_string (metakey, "Application", "name", NULL);
  if (id == NULL)
    {
      id = g_key_file_get_string (metakey, "Runtime", "name", NULL);
      if (id == NULL)
        return flatpak_fail (error, _("No name specified in the metadata"));
      is_runtime = TRUE;
    }

  if (g_file_query_exists (export_dir, cancellable))
    return flatpak_fail (error, _("Build directory %s already finalized"), directory);

  if (!is_runtime)
    {
      g_debug ("Collecting exports");
      if (!collect_exports (base, id, cancellable, error))
        return FALSE;
    }

  g_debug ("Updating metadata");
  if (!update_metadata (base, arg_context, is_runtime, cancellable, error))
    return FALSE;

  g_print (_("Please review the exported files and the metadata\n"));

  return TRUE;
}

gboolean
flatpak_complete_build_finish (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakContext) arg_context = NULL;

  context = g_option_context_new ("");

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* DIR */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_context_complete (arg_context, completion);

      flatpak_complete_dir (completion);
      break;
    }

  return TRUE;
}
