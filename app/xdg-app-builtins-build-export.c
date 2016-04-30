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

static char *opt_subject;
static char *opt_body;
static char *opt_arch;
static gboolean opt_runtime;
static gboolean opt_update_appstream;
static char **opt_gpg_key_ids;
static char **opt_exclude;
static char **opt_include;
static char *opt_gpg_homedir;
static char *opt_files;
static char *opt_metadata;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject", "SUBJECT" },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &opt_body, "Full description", "BODY" },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Architecture to export for (must be host compatible)", "ARCH" },
  { "runtime", 'r', 0, G_OPTION_ARG_NONE, &opt_runtime, "Commit runtime (/usr), not /app" },
  { "update-appstream", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Update the appstream branch" },
  { "files", 0, 0, G_OPTION_ARG_STRING, &opt_files, "Use alternative directory for the files", "SUBDIR"},
  { "metadata", 0, 0, G_OPTION_ARG_STRING, &opt_metadata, "Use alternative file for the metadata", "FILE"},
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, "GPG Key ID to sign the commit with", "KEY-ID"},
  { "exclude", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_exclude, "Files to exclude", "PATTERN"},
  { "include", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_include, "Excluded files to include", "PATTERN"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},

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
      *out_arch = g_strdup (xdg_app_get_arch ());
      return TRUE;
    }

  parts = g_strsplit (runtime, "/", 0);
  if (g_strv_length (parts) != 3)
    return xdg_app_fail (error, "Failed to determine arch from metadata runtime key: %s", runtime);

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

typedef struct {
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
      if (xdg_app_path_match_prefix (patterns[i], path) != NULL)
        return TRUE;
    }

  return FALSE;
}

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo *repo,
               const char *path,
               GFileInfo *file_info,
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
add_file_to_mtree (GFile *file,
                   const char *name,
                   OstreeRepo *repo,
                   OstreeMutableTree *mtree,
                   GCancellable *cancellable,
                   GError **error)
{
  g_autoptr (GFileInfo) file_info = NULL;
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

  raw_input = (GInputStream*)g_file_read (file, cancellable, error);
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

gboolean
xdg_app_builtin_build_export (int argc, char **argv, GCancellable *cancellable, GError **error)
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

  context = g_option_context_new ("LOCATION DIRECTORY [BRANCH] - Create a repository from a build directory");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, "LOCATION and DIRECTORY must be specified", error);
      goto out;
    }

  location = argv[1];
  directory = argv[2];

  if (argc >= 4)
    branch = argv[3];
  else
    branch = "master";

  if (!xdg_app_is_valid_branch (branch))
    {
      xdg_app_fail (error, "'%s' is not a valid branch name", branch);
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
      xdg_app_fail (error, "Build directory %s not initialized", directory);
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
      xdg_app_fail (error, "Build directory %s not finalized", directory);
      goto out;
    }

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

  if (!xdg_app_mtree_create_root (repo, mtree, cancellable, error))
    goto out;

  if (!ostree_mutable_tree_ensure_dir (mtree, "files", &files_mtree, error))
    goto out;

  modifier = ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS,
                                              (OstreeRepoCommitFilter)commit_filter, &commit_data, NULL);

  if (opt_runtime)
    {
      commit_data.exclude = (const char **)opt_exclude;
      commit_data.include = (const char **)opt_include;
      if (!ostree_repo_write_directory_to_mtree (repo, usr, files_mtree, modifier, cancellable, error))
        goto out;
      commit_data.exclude = NULL;
      commit_data.include = NULL;
    }
  else
    {
      commit_data.exclude = (const char **)opt_exclude;
      commit_data.include = (const char **)opt_include;
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

  if (opt_update_appstream)
    {
      g_autoptr(GError) my_error = NULL;

      if (!xdg_app_repo_generate_appstream (repo, (const char **)opt_gpg_key_ids, opt_gpg_homedir, cancellable, &my_error))
        {
          if (g_error_matches (my_error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
            g_print ("WARNING: Can't find appstream-builder, unable to update appstream branch\n");
          else
            {
              g_propagate_error (error, g_steal_pointer (&my_error));
              return FALSE;
            }
        }
    }

  if (!xdg_app_repo_update (repo,
                            (const char **)opt_gpg_key_ids,
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
