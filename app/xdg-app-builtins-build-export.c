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

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject", "SUBJECT" },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &opt_body, "Full description", "BODY" },

  { NULL }
};

static gboolean
metadata_get_arch (GFile *file, char **out_arch, GError **error)
{
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *runtime = NULL;
  g_auto(GStrv) parts = NULL;

  keyfile = g_key_file_new ();
  path = g_file_get_path (file);
  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;

  runtime = g_key_file_get_string (keyfile, "Application", "runtime", error);
  if (*error)
    return FALSE;

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

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo *repo,
               const char *path,
               GFileInfo *file_info,
               gpointer user_data)
{
  if (g_str_equal (path, "/") ||
      g_str_equal (path, "/metadata") ||
      g_str_has_prefix (path, "/files") ||
      g_str_has_prefix (path, "/export"))
    {
      g_debug ("commit filter, allow: %s", path);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      g_debug ("commit filter, skip: %s", path);
      return OSTREE_REPO_COMMIT_FILTER_SKIP;
    }
}

gboolean
xdg_app_builtin_build_export (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autoptr(GFile) export = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(GFile) arg = NULL;
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
  g_autoptr(OstreeMutableTree) mtree = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  gsize metadata_size;
  g_autofree char *subject = NULL;
  g_autofree char *body = NULL;
  OstreeRepoTransactionStats stats;
  OstreeRepoCommitModifier *modifier = NULL;

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
  files = g_file_get_child (base, "files");
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

  app_id = g_key_file_get_string (metakey, "Application", "name", error);
  if (app_id == NULL)
    goto out;

  if (!g_file_query_exists (export, cancellable))
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

  full_branch = g_strconcat ("app/", app_id, "/", arch, "/", branch, NULL);

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
  arg = g_file_new_for_commandline_arg (directory);

  modifier = ostree_repo_commit_modifier_new (0, commit_filter, NULL, NULL);
  if (!ostree_repo_write_directory_to_mtree (repo, arg, mtree, modifier, cancellable, error))
    goto out;

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (!ostree_repo_write_commit (repo, parent, subject, body, NULL,
                                 OSTREE_REPO_FILE (root),
                                 &commit_checksum, cancellable, error))
    goto out;

  ostree_repo_transaction_set_ref (repo, NULL, full_branch, commit_checksum);

  if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
    goto out;

  g_print ("Commit: %s\n", commit_checksum);
  g_print ("Metadata Total: %u\n", stats.metadata_objects_total);
  g_print ("Metadata Written: %u\n", stats.metadata_objects_written);
  g_print ("Content Total: %u\n", stats.content_objects_total);
  g_print ("Content Written: %u\n", stats.content_objects_written);
  g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats.content_bytes_written);

  ret = TRUE;

 out:
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);

  return ret;
}
