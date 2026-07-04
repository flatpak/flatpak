/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-context-private.h"
#include "flatpak-dir-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static char *opt_command;
static char *opt_require_version;
static char **opt_extra_data;
static char **opt_extensions;
static char **opt_remove_extensions;
static char **opt_metadata;
static gboolean opt_no_exports;
static gboolean opt_no_inherit_permissions;
static int opt_extension_prio = G_MININT;
static char *opt_sdk;
static char *opt_runtime;

static GOptionEntry options[] = {
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, N_("Command to set"), N_("COMMAND") },
  { "require-version", 0, 0, G_OPTION_ARG_STRING, &opt_require_version, N_("Flatpak version to require"), N_("MAJOR.MINOR.MICRO") },
  { "no-exports", 0, 0, G_OPTION_ARG_NONE, &opt_no_exports, N_("Don't process exports"), NULL },
  { "extra-data", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_extra_data, N_("Extra data info") },
  { "extension", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_extensions, N_("Add extension point info"),  N_("NAME=VARIABLE[=VALUE]") },
  { "remove-extension", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_remove_extensions, N_("Remove extension point info"),  N_("NAME") },
  { "extension-priority", 0, 0, G_OPTION_ARG_INT, &opt_extension_prio, N_("Set extension priority (only for extensions)"), N_("VALUE") },
  { "sdk", 0, 0, G_OPTION_ARG_STRING, &opt_sdk, N_("Change the sdk used for the app"),  N_("SDK") },
  { "runtime", 0, 0, G_OPTION_ARG_STRING, &opt_runtime, N_("Change the runtime used for the app"),  N_("RUNTIME") },
  { "metadata", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata, N_("Set generic metadata option"),  N_("GROUP=KEY[=VALUE]") },
  { "no-inherit-permissions", 0, 0, G_OPTION_ARG_NONE, &opt_no_inherit_permissions, N_("Don't inherit permissions from runtime"), NULL },
  { NULL }
};

static gboolean
export_dir (int           source_parent_fd,
            const char   *source_name,
            const char   *source_relpath,
            int           destination_parent_fd,
            const char   *destination_name,
            char        **allowed_prefixes,
            char        **allowed_extensions,
            gboolean      require_exact_match,
            GCancellable *cancellable,
            GError      **error)
{
  int res;
  g_auto(GLnxDirFdIterator) source_iter = {0};
  glnx_autofd int destination_dfd = -1;
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
                           allowed_prefixes, allowed_extensions, require_exact_match,
                           cancellable, error))
            return FALSE;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          g_autofree gchar *name_without_extension = NULL;
          int i;

          source_printable = g_build_filename (source_relpath, dent->d_name, NULL);

          for (i = 0; allowed_extensions[i] != NULL; i++)
            {
              if (g_str_has_suffix (dent->d_name, allowed_extensions[i]))
                break;
            }

          if (allowed_extensions[i] == NULL)
            {
              g_print (_("Not exporting %s, wrong extension\n"), source_printable);
              continue;
            }

          name_without_extension = g_strndup (dent->d_name, strlen (dent->d_name) - strlen (allowed_extensions[i]));

          if (!flatpak_name_matches_one_wildcard_prefix (name_without_extension, (const char * const *) allowed_prefixes, require_exact_match))
            {
              g_print (_("Not exporting %s, non-allowed export filename\n"), source_printable);
              continue;
            }

          g_print (_("Exporting %s\n"), source_printable);

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
          g_info ("Not exporting non-regular file %s", source_printable);
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
              char        **allowed_prefixes,
              char        **allowed_extensions,
              gboolean      require_exact_match,
              GCancellable *cancellable,
              GError      **error)
{
  if (!flatpak_mkdir_p (destination, cancellable, error))
    return FALSE;

  /* The fds are closed by this call */
  if (!export_dir (AT_FDCWD, flatpak_file_get_path_cached (source), source_prefix,
                   AT_FDCWD, flatpak_file_get_path_cached (destination),
                   allowed_prefixes, allowed_extensions, require_exact_match,
                   cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
collect_exports (GFile          *base,
                 const char     *app_id,
                 FlatpakContext *arg_context,
                 GCancellable   *cancellable,
                 GError        **error)
{
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) export = NULL;
  int i;
  const char *paths[] = {
    "share/applications",                 /* Copy desktop files */
    "share/mime/packages",                /* Copy MIME Type files */
    "share/icons",                        /* Icons */
    "share/dbus-1/services",              /* D-Bus service files */
    "share/gnome-shell/search-providers", /* Search providers */
    "share/krunner/dbusplugins",          /* KDE krunner DBus plugins */
    "share/appdata",                      /* Copy appdata/metainfo files (legacy path) */
    "share/metainfo",                     /* Copy appdata/metainfo files */
    NULL,
  };


  files = g_file_get_child (base, "files");

  export = g_file_get_child (base, "export");

  if (!flatpak_mkdir_p (export, cancellable, error))
    return FALSE;

  if (opt_no_exports)
    return TRUE;

  for (i = 0; paths[i]; i++)
    {
      const char * path = paths[i];
      g_autoptr(GFile) src = g_file_resolve_relative_path (files, path);
      g_auto(GStrv) allowed_prefixes = NULL;
      g_auto(GStrv) allowed_extensions = NULL;
      gboolean require_exact_match = FALSE;

      if (!flatpak_context_get_allowed_exports (arg_context, path, app_id,
                                                &allowed_extensions, &allowed_prefixes, &require_exact_match))
        return flatpak_fail (error, "Unexpectedly not allowed to export %s", path);

      if (g_file_query_exists (src, cancellable))
        {
          g_info ("Exporting from %s", path);
          g_autoptr(GFile) dest = NULL;
          g_autoptr(GFile) dest_parent = NULL;

          if (strcmp (path, "share/appdata") == 0)
            dest = g_file_resolve_relative_path (export, "share/metainfo");
          else
            dest = g_file_resolve_relative_path (export, path);

          dest_parent = g_file_get_parent (dest);
          g_info ("Ensuring export/%s parent exists", path);
          if (!flatpak_mkdir_p (dest_parent, cancellable, error))
            return FALSE;
          g_info ("Copying from files/%s", path);
          if (!copy_exports (src,
                             dest,
                             path,
                             allowed_prefixes,
                             allowed_extensions,
                             require_exact_match,
                             cancellable,
                             error))
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
  g_autoptr(FlatpakContext) inherited_context = NULL;
  GError *temp_error = NULL;
  const char *group;
  int i;

  metadata = g_file_get_child (base, "metadata");
  if (!g_file_query_exists (metadata, cancellable))
    goto out;

  if (is_runtime)
    group = FLATPAK_METADATA_GROUP_RUNTIME;
  else
    group = FLATPAK_METADATA_GROUP_APPLICATION;

  path = g_file_get_path (metadata);
  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    goto out;

  if (opt_sdk != NULL || opt_runtime != NULL)
    {
      g_autofree char *old_runtime = g_key_file_get_string (keyfile,
                                                            group,
                                                            FLATPAK_METADATA_KEY_RUNTIME, NULL);
      g_autofree char *old_sdk = g_key_file_get_string (keyfile,
                                                        group,
                                                        FLATPAK_METADATA_KEY_SDK,
                                                        NULL);
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
          g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_SDK, ref);
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
          g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_RUNTIME, ref);
        }
    }

  if (!is_runtime)
    {
      if (g_key_file_has_key (keyfile, group, FLATPAK_METADATA_KEY_COMMAND, NULL))
        {
          g_info ("Command key is present");

          if (opt_command)
            g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_COMMAND, opt_command);
        }
      else if (opt_command)
        {
          g_info ("Using explicitly provided command %s", opt_command);

          g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_COMMAND, opt_command);
        }
      else
        {
          g_autofree char *command = NULL;
          g_autoptr(GFile) bin_dir = NULL;
          g_autoptr(GFileEnumerator) bin_enum = NULL;
          g_autoptr(GFileInfo) child_info = NULL;

          g_info ("Looking for executables");

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
                      g_print (_("More than one executable found\n"));
                      break;
                    }
                  command = g_strdup (g_file_info_get_name (child_info));
                }
              if (temp_error != NULL)
                goto out;
            }

          if (command)
            {
              g_print (_("Using %s as command\n"), command);
              g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_COMMAND, command);
            }
          else
            {
              g_print (_("No executable found\n"));
            }
        }

      /* Inherit permissions from runtime by default */
      if (!opt_no_inherit_permissions)
        {
          g_autofree char *runtime_pref = NULL;
          g_autoptr(GFile) runtime_deploy_dir = NULL;

          runtime_pref = g_key_file_get_string (keyfile, group, FLATPAK_METADATA_KEY_RUNTIME, NULL);
          if (runtime_pref != NULL)
            {
              g_autoptr(FlatpakDecomposed) runtime_ref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, runtime_pref, NULL);
              if (runtime_ref)
                runtime_deploy_dir = flatpak_find_deploy_dir_for_ref (runtime_ref, NULL, cancellable, NULL);
            }

          /* This is optional, because some weird uses of flatpak build-finish (like the test suite)
             may not have the actual runtime installed. Most will though, as otherwise flatpak build
             will not work. */
          if (runtime_deploy_dir != NULL)
            {
              g_autoptr(GFile) runtime_metadata_file = NULL;
              g_autofree char *runtime_metadata_contents = NULL;
              gsize runtime_metadata_size;
              g_autoptr(GKeyFile) runtime_metakey = NULL;

              runtime_metadata_file = g_file_get_child (runtime_deploy_dir, "metadata");
              if (!g_file_load_contents (runtime_metadata_file, cancellable,
                                         &runtime_metadata_contents, &runtime_metadata_size, NULL, error))
                goto out;

              runtime_metakey = g_key_file_new ();
              if (!g_key_file_load_from_data (runtime_metakey, runtime_metadata_contents, runtime_metadata_size, 0, error))
                goto out;

              inherited_context = flatpak_context_new ();
              if (!flatpak_context_load_metadata (inherited_context, runtime_metakey, error))
                goto out;

              /* non-permissions are inherited at runtime, so no need to inherit them */
              flatpak_context_reset_non_permissions (inherited_context);
            }
        }
    }

  if (opt_require_version)
    {
      g_autoptr(GError) local_error = NULL;

      g_key_file_set_string (keyfile, group, "required-flatpak", opt_require_version);
      if (!flatpak_check_required_version ("test", keyfile, &local_error) &&
          g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_DATA))
        {
          flatpak_fail (error, _("Invalid --require-version argument: %s"), opt_require_version);
          goto out;
        }
    }

  app_context = flatpak_context_new ();
  if (inherited_context)
    flatpak_context_merge (app_context, inherited_context);
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

      uri_key = g_strconcat (FLATPAK_METADATA_KEY_EXTRA_DATA_URI, suffix, NULL);
      name_key = g_strconcat (FLATPAK_METADATA_KEY_EXTRA_DATA_NAME, suffix, NULL);
      checksum_key = g_strconcat (FLATPAK_METADATA_KEY_EXTRA_DATA_CHECKSUM,
                                  suffix, NULL);
      size_key = g_strconcat (FLATPAK_METADATA_KEY_EXTRA_DATA_SIZE, suffix, NULL);
      installed_size_key = g_strconcat (FLATPAK_METADATA_KEY_EXTRA_DATA_INSTALLED_SIZE,
                                        suffix, NULL);

      if (strlen (elements[0]) > 0)
        g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_EXTRA_DATA,
                               name_key, elements[0]);
      g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_EXTRA_DATA,
                             checksum_key, elements[1]);
      g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_EXTRA_DATA,
                             size_key, elements[2]);
      if (strlen (elements[3]) > 0)
        g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_EXTRA_DATA,
                               installed_size_key, elements[3]);
      g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_EXTRA_DATA,
                             uri_key, elements[4]);
    }

  for (i = 0; opt_metadata != NULL && opt_metadata[i] != NULL; i++)
    {
      g_auto(GStrv) elements = NULL;
      elements = g_strsplit (opt_metadata[i], "=", 3);
      if (g_strv_length (elements) < 2)
        {
          flatpak_fail (error, _("Too few elements in --metadata argument %s, format should be GROUP=KEY[=VALUE]]"), opt_metadata[i]);
          goto out;
        }

      g_key_file_set_string (keyfile, elements[0], elements[1], elements[2] ? elements[2] : "true");
    }

  for (i = 0; opt_remove_extensions != NULL && opt_remove_extensions[i] != NULL; i++)
    {
      g_autofree char *groupname = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION, opt_remove_extensions[i], NULL);

      g_key_file_remove_group (keyfile, groupname, NULL);
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

      if (!flatpak_is_valid_name (elements[0], -1, error))
        {
          glnx_prefix_error (error, _("Invalid extension name %s"), elements[0]);
          goto out;
        }

      groupname = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION,
                               elements[0], NULL);

      g_key_file_set_string (keyfile, groupname, elements[1], elements[2] ? elements[2] : "true");
    }


  if (opt_extension_prio != G_MININT)
    g_key_file_set_integer (keyfile, FLATPAK_METADATA_GROUP_EXTENSION_OF,
                            FLATPAK_METADATA_KEY_PRIORITY, opt_extension_prio);

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

  id = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_APPLICATION,
                              FLATPAK_METADATA_KEY_NAME, NULL);
  if (id == NULL)
    {
      id = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_RUNTIME,
                                  FLATPAK_METADATA_KEY_NAME, NULL);
      if (id == NULL)
        return flatpak_fail (error, _("No name specified in the metadata"));
      is_runtime = TRUE;
    }

  if (g_file_query_exists (export_dir, cancellable))
    return flatpak_fail (error, _("Build directory %s already finalized"), directory);

  if (!is_runtime)
    {
      g_info ("Collecting exports");
      if (!collect_exports (base, id, arg_context, cancellable, error))
        return FALSE;
    }

  g_info ("Updating metadata");
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
      flatpak_complete_context (completion);

      flatpak_complete_dir (completion);
      break;
    }

  return TRUE;
}
