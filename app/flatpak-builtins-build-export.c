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

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"

static char *opt_subject;
static char *opt_body;
static char *opt_arch;
static gboolean opt_runtime;
static gboolean opt_update_appstream;
static gboolean opt_no_update_summary;
static char **opt_gpg_key_ids;
static char **opt_exclude;
static char **opt_include;
static char *opt_gpg_homedir;
static char *opt_files;
static char *opt_metadata;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, N_("One line subject"), N_("SUBJECT") },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &opt_body, N_("Full description"), N_("BODY") },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Architecture to export for (must be host compatible)"), N_("ARCH") },
  { "runtime", 'r', 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Commit runtime (/usr), not /app"), NULL },
  { "update-appstream", 0, 0, G_OPTION_ARG_NONE, &opt_update_appstream, N_("Update the appstream branch"), NULL },
  { "no-update-summary", 0, 0, G_OPTION_ARG_NONE, &opt_no_update_summary, N_("Don't update the summary"), NULL },
  { "files", 0, 0, G_OPTION_ARG_STRING, &opt_files, N_("Use alternative directory for the files"), N_("SUBDIR") },
  { "metadata", 0, 0, G_OPTION_ARG_STRING, &opt_metadata, N_("Use alternative file for the metadata"), N_("FILE") },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the commit with"), N_("KEY-ID") },
  { "exclude", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_exclude, N_("Files to exclude"), N_("PATTERN") },
  { "include", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_include, N_("Excluded files to include"), N_("PATTERN") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },

  { NULL }
};

static gboolean
metadata_get_arch (GFile *file, char **out_arch, GError **error)
{
  g_autofree char *path = NULL;

  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *runtime = NULL;
  g_auto(GStrv) parts = NULL;

  if (opt_arch != NULL)
    {
      *out_arch = g_strdup (opt_arch);
      return TRUE;
    }

  keyfile = g_key_file_new ();
  path = g_file_get_path (file);
  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;

  runtime = g_key_file_get_string (keyfile,
                                   opt_runtime ? "Runtime" : "Application",
                                   "runtime", NULL);
  if (runtime == NULL)
    {
      *out_arch = g_strdup (flatpak_get_arch ());
      return TRUE;
    }

  parts = g_strsplit (runtime, "/", 0);
  if (g_strv_length (parts) != 3)
    return flatpak_fail (error, "Failed to determine arch from metadata runtime key: %s", runtime);

  *out_arch = g_strdup (parts[1]);

  return TRUE;
}

static gboolean
is_empty_directory (GFile *file, GCancellable *cancellable)
{
  g_autoptr(GFileEnumerator) file_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;

  file_enum = g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                         G_FILE_QUERY_INFO_NONE,
                                         cancellable, NULL);
  if (!file_enum)
    return FALSE;

  child_info = g_file_enumerator_next_file (file_enum, cancellable, NULL);
  if (child_info)
    return FALSE;

  return TRUE;
}

typedef struct
{
  const char **exclude;
  const char **include;
} CommitData;

static gboolean
matches_patterns (const char **patterns, const char *path)
{
  int i;

  if (patterns == NULL)
    return FALSE;

  for (i = 0; patterns[i] != NULL; i++)
    {
      if (flatpak_path_match_prefix (patterns[i], path) != NULL)
        return TRUE;
    }

  return FALSE;
}

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo *repo,
               const char *path,
               GFileInfo  *file_info,
               CommitData *commit_data)
{
  guint mode;

  /* No user info */
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);

  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  /* No setuid */
  mode = mode & ~07000;
  /* All files readable */
  mode = mode | 0444;
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", mode);

  if (matches_patterns (commit_data->exclude, path) &&
      !matches_patterns (commit_data->include, path))
    {
      g_debug ("Excluding %s", path);
      return OSTREE_REPO_COMMIT_FILTER_SKIP;
    }

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

gboolean
add_file_to_mtree (GFile             *file,
                   const char        *name,
                   OstreeRepo        *repo,
                   OstreeMutableTree *mtree,
                   GCancellable      *cancellable,
                   GError           **error)
{
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GInputStream) raw_input = NULL;
  g_autoptr(GInputStream) input = NULL;
  guint64 length;
  g_autofree guchar *child_file_csum = NULL;
  char *tmp_checksum = NULL;

  file_info = g_file_query_info (file,
                                 "standard::size",
                                 0, cancellable, error);
  if (file_info == NULL)
    return FALSE;

  g_file_info_set_name (file_info, name);
  g_file_info_set_file_type (file_info, G_FILE_TYPE_REGULAR);
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", 0100644);

  raw_input = (GInputStream *) g_file_read (file, cancellable, error);
  if (raw_input == NULL)
    return FALSE;

  if (!ostree_raw_file_to_content_stream (raw_input,
                                          file_info, NULL,
                                          &input, &length,
                                          cancellable, error))
    return FALSE;

  if (!ostree_repo_write_content (repo, NULL, input, length,
                                  &child_file_csum, cancellable, error))
    return FALSE;

  tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
  if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum, error))
    return FALSE;

  return TRUE;
}

static gboolean
find_file_in_tree (GFile *base, const char *filename)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;

  enumerator = g_file_enumerate_children (base,
                                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                          G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NONE,
                                          NULL,
                                          NULL);
  if (!enumerator)
    return FALSE;

  do {
    g_autoptr(GFileInfo) info = g_file_enumerator_next_file (enumerator, NULL, NULL);
    GFileType type;
    const char *name;

    if (!info)
      return FALSE;

    type = g_file_info_get_file_type (info);
    name = g_file_info_get_name (info);

    if (type == G_FILE_TYPE_REGULAR && strcmp (name, filename) == 0)
      return TRUE;
    else if (type == G_FILE_TYPE_DIRECTORY)
      {
        g_autoptr(GFile) dir = g_file_get_child (base, name);
        if (find_file_in_tree (dir, filename))
          return TRUE;
      }
  } while (1);

  return FALSE;
}

static GFile *
convert_app_absolute_path (const char *path, GFile *files)
{
  g_autofree char *exec_path = NULL;

  if (g_path_is_absolute (path))
    {
      if (g_str_has_prefix (path, "/app/"))
        exec_path = g_strdup (path + 5);
      else
        exec_path = g_strdup (path);
    }
  else
    exec_path = g_strconcat ("bin/", path, NULL);

  return g_file_resolve_relative_path (files, exec_path);
}

static gboolean
validate_desktop_file (GFile *desktop_file,
                       GFile *export,
                       GFile *files,
                       const char *app_id,
                       char **icon,
                       gboolean *activatable,
                       GError **error)
{
  g_autofree char *path = g_file_get_path (desktop_file);
  g_autoptr(GSubprocess) subprocess = NULL;
  g_autofree char *stdout_buf = NULL;
  g_autofree char *stderr_buf = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autofree char *command = NULL;
  g_auto(GStrv) argv = NULL;
  g_autoptr(GFile) bin_file = NULL;

  if (!g_file_query_exists (desktop_file, NULL))
    return TRUE;

  subprocess = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                 &local_error, "desktop-file-validate", path, NULL);
  if (!subprocess)
    {
      if (!g_error_matches (local_error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        g_print ("WARNING: Error running desktop-file-validate: %s\n", local_error->message);

      g_clear_error (&local_error);
      goto check_refs;
    }

  if (!g_subprocess_communicate_utf8 (subprocess, NULL, NULL, &stdout_buf, &stderr_buf, &local_error))
    {
      g_print ("WARNING: Error reading from desktop-file-validate: %s\n", local_error->message);
      g_clear_error (&local_error);
    }

  if (g_subprocess_get_if_exited (subprocess) &&
      g_subprocess_get_exit_status (subprocess) != 0)
    g_print ("WARNING: Failed to validate desktop file %s: %s\n", path, stdout_buf);

check_refs:
  /* Test that references to other files are valid */

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, error))
    return FALSE;

  command = g_key_file_get_string (key_file,
                                   G_KEY_FILE_DESKTOP_GROUP,
                                   G_KEY_FILE_DESKTOP_KEY_EXEC,
                                   &local_error);
  if (!command)
    {
      g_print ("WARNING: Can't find Exec key in %s: %s\n", path, local_error->message);
      g_clear_error (&local_error);
    }

  argv = g_strsplit (command, " ", 0);

  bin_file = convert_app_absolute_path (argv[0], files);
  if (!g_file_query_exists (bin_file, NULL))
    g_print ("WARNING: Binary not found for Exec line in %s: %s", path, command);

  *icon = g_key_file_get_string (key_file,
                                 G_KEY_FILE_DESKTOP_GROUP,
                                 G_KEY_FILE_DESKTOP_KEY_ICON,
                                 NULL);
  if (*icon && !g_str_has_prefix (*icon, app_id))
    g_print ("WARNING: Icon not matching app id in %s: %s", path, *icon);

  *activatable = g_key_file_get_boolean (key_file,
                                         G_KEY_FILE_DESKTOP_GROUP,
                                         G_KEY_FILE_DESKTOP_KEY_DBUS_ACTIVATABLE,
                                         NULL);

  return TRUE;
}

static gboolean
validate_icon (const char *icon,
               GFile *export,
               const char *app_id,
               GError **error)
{
  g_autoptr(GFile) icondir = NULL;
  g_autofree char *png = NULL;
  g_autofree char *svg  = NULL;

  if (!icon)
    return TRUE;

  icondir = g_file_resolve_relative_path (export, "share/icons/hicolor");
  png = g_strconcat (icon, ".png", NULL);
  svg  = g_strconcat (icon, ".svg", NULL);
  if (!find_file_in_tree (icondir, png) &&
      !find_file_in_tree (icondir, svg))
    g_print ("WARNING: Icon referenced in desktop file but not exported: %s", icon);

  return TRUE;
}

static gboolean
validate_service_file (GFile *service_file,
                       gboolean activatable,
                       GFile *files,
                       const char *app_id,
                       GError **error)
{
  g_autofree char *path = g_file_get_path (service_file);
  g_autoptr(GKeyFile) key_file = NULL;
  g_autofree char *name = NULL;
  g_autofree char *command = NULL;
  g_auto(GStrv) argv = NULL;
  g_autoptr(GFile) bin_file = NULL;

  if (!g_file_query_exists (service_file, NULL))
    {
      if (activatable)
        {
          g_set_error (error,
                       G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Desktop file D-Bus activatable, but service file not exported: %s", path);
          return FALSE;
        }

      return TRUE;
    }

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, error))
    return FALSE;

  name = g_key_file_get_string (key_file, "D-BUS Service", "Name", error);
  if (!name)
    {
      g_prefix_error (error, "Invalid service file %s: ", path);
      return FALSE;
    }

  if (strcmp (name, app_id) != 0)
    {
      g_set_error (error,
                   G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Name in service file %s does not match app id: %s", path, name);
      return FALSE;
    }

  command = g_key_file_get_string (key_file, "D-BUS Service", "Exec", error);
  if (!command)
    {
      g_prefix_error (error, "Invalid service file %s: ", path);
      return FALSE;
    }

  argv = g_strsplit (command, " ", 0);

  bin_file = convert_app_absolute_path (argv[0], files);
  if (!g_file_query_exists (bin_file, NULL))
    g_print ("WARNING: Binary not found for Exec line in %s: %s", path, command);

  return TRUE;
}

static gboolean
validate_exports (GFile *export, GFile *files, const char *app_id, GError **error)
{
  g_autofree char *desktop_path = NULL;
  g_autoptr(GFile) desktop_file = NULL;
  g_autofree char *service_path = NULL;
  g_autoptr(GFile) service_file = NULL;
  g_autofree char *icon = NULL;
  gboolean activatable = FALSE;

  desktop_path = g_strconcat ("share/applications/", app_id, ".desktop", NULL);
  desktop_file = g_file_resolve_relative_path (export, desktop_path);

  if (!validate_desktop_file (desktop_file, export, files, app_id, &icon, &activatable, error))
    return FALSE;

  if (!validate_icon (icon, export, app_id, error))
    return FALSE;

  service_path = g_strconcat ("share/dbus-1/services/", app_id, ".service", NULL);
  service_file = g_file_resolve_relative_path (export, service_path);

  if (!validate_service_file (service_file, activatable, files, app_id, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_build_export (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;

  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) usr = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autoptr(GFile) export = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  const char *directory;
  const char *branch;
  g_autofree char *arch = NULL;
  g_autofree char *full_branch = NULL;
  g_autofree char *app_id = NULL;
  g_autofree char *parent = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *format_size = NULL;
  g_autoptr(OstreeMutableTree) mtree = NULL;
  g_autoptr(OstreeMutableTree) files_mtree = NULL;
  g_autoptr(OstreeMutableTree) export_mtree = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  gsize metadata_size;
  g_autofree char *subject = NULL;
  g_autofree char *body = NULL;
  OstreeRepoTransactionStats stats;
  g_autoptr(OstreeRepoCommitModifier) modifier = NULL;
  CommitData commit_data = {0};

  context = g_option_context_new (_("LOCATION DIRECTORY [BRANCH] - Create a repository from a build directory"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, _("LOCATION and DIRECTORY must be specified"), error);
      goto out;
    }

  location = argv[1];
  directory = argv[2];

  if (argc >= 4)
    branch = argv[3];
  else
    branch = "master";

  if (!flatpak_is_valid_branch (branch))
    {
      flatpak_fail (error, _("'%s' is not a valid branch name"), branch);
      goto out;
    }

  base = g_file_new_for_commandline_arg (directory);
  if (opt_files)
    files = g_file_resolve_relative_path (base, opt_files);
  else
    files = g_file_get_child (base, "files");
  if (opt_files)
    usr = g_file_resolve_relative_path (base, opt_files);
  else
    usr = g_file_get_child (base, "usr");
  if (opt_metadata)
    metadata = g_file_resolve_relative_path (base, opt_metadata);
  else
    metadata = g_file_get_child (base, "metadata");
  export = g_file_get_child (base, "export");

  if (!g_file_query_exists (files, cancellable) ||
      !g_file_query_exists (metadata, cancellable))
    {
      flatpak_fail (error, _("Build directory %s not initialized"), directory);
      goto out;
    }

  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    goto out;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    goto out;

  app_id = g_key_file_get_string (metakey, opt_runtime ? "Runtime" : "Application", "name", error);
  if (app_id == NULL)
    goto out;

  if (!opt_runtime && !g_file_query_exists (export, cancellable))
    {
      flatpak_fail (error, "Build directory %s not finalized", directory);
      goto out;
    }

  if (!validate_exports (export, files, app_id, error))
    goto out;

  if (!metadata_get_arch (metadata, &arch, error))
    goto out;

  if (opt_subject)
    subject = g_strdup (opt_subject);
  else
    subject = g_strconcat ("Export ", app_id, NULL);
  if (opt_body)
    body = g_strdup (opt_body);
  else
    body = g_strconcat ("Name: ", app_id, "\nArch: ", arch, "\nBranch: ", branch, NULL);

  full_branch = g_strconcat (opt_runtime ? "runtime/" : "app/", app_id, "/", arch, "/", branch, NULL);

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (g_file_query_exists (repofile, cancellable) &&
      !is_empty_directory (repofile, cancellable))
    {
      if (!ostree_repo_open (repo, cancellable, error))
        goto out;

      if (!ostree_repo_resolve_rev (repo, full_branch, TRUE, &parent, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_create (repo, OSTREE_REPO_MODE_ARCHIVE_Z2, cancellable, error))
        goto out;
    }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();

  if (!flatpak_mtree_create_root (repo, mtree, cancellable, error))
    goto out;

  if (!ostree_mutable_tree_ensure_dir (mtree, "files", &files_mtree, error))
    goto out;

  modifier = ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS,
                                              (OstreeRepoCommitFilter) commit_filter, &commit_data, NULL);

  if (opt_runtime)
    {
      commit_data.exclude = (const char **) opt_exclude;
      commit_data.include = (const char **) opt_include;
      if (!ostree_repo_write_directory_to_mtree (repo, usr, files_mtree, modifier, cancellable, error))
        goto out;
      commit_data.exclude = NULL;
      commit_data.include = NULL;
    }
  else
    {
      commit_data.exclude = (const char **) opt_exclude;
      commit_data.include = (const char **) opt_include;
      if (!ostree_repo_write_directory_to_mtree (repo, files, files_mtree, modifier, cancellable, error))
        goto out;
      commit_data.exclude = NULL;
      commit_data.include = NULL;

      if (!ostree_mutable_tree_ensure_dir (mtree, "export", &export_mtree, error))
        goto out;

      if (!ostree_repo_write_directory_to_mtree (repo, export, export_mtree, modifier, cancellable, error))
        goto out;
    }

  if (!add_file_to_mtree (metadata, "metadata", repo, mtree, cancellable, error))
    goto out;

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (!ostree_repo_write_commit (repo, parent, subject, body, NULL,
                                 OSTREE_REPO_FILE (root),
                                 &commit_checksum, cancellable, error))
    goto out;

  if (opt_gpg_key_ids)
    {
      char **iter;

      for (iter = opt_gpg_key_ids; iter && *iter; iter++)
        {
          const char *keyid = *iter;

          if (!ostree_repo_sign_commit (repo,
                                        commit_checksum,
                                        keyid,
                                        opt_gpg_homedir,
                                        cancellable,
                                        error))
            goto out;
        }
    }

  ostree_repo_transaction_set_ref (repo, NULL, full_branch, commit_checksum);

  if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
    goto out;

  if (opt_update_appstream &&
      !flatpak_repo_generate_appstream (repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, cancellable, error))
    return FALSE;

  if (!opt_no_update_summary &&
      !flatpak_repo_update (repo,
                            (const char **) opt_gpg_key_ids,
                            opt_gpg_homedir,
                            cancellable,
                            error))
    goto out;

  format_size = g_format_size (stats.content_bytes_written);

  g_print ("Commit: %s\n", commit_checksum);
  g_print ("Metadata Total: %u\n", stats.metadata_objects_total);
  g_print ("Metadata Written: %u\n", stats.metadata_objects_written);
  g_print ("Content Total: %u\n", stats.content_objects_total);
  g_print ("Content Written: %u\n", stats.content_objects_written);
  g_print ("Content Bytes Written: %" G_GUINT64_FORMAT " (%s)\n", stats.content_bytes_written, format_size);

  ret = TRUE;

out:
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);

  return ret;
}

gboolean
flatpak_complete_build_export (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* LOCATION */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;

    case 2: /* DIR */
      flatpak_complete_dir (completion);
      break;
    }

  return TRUE;
}
