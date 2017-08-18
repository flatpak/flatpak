/* builder-git.c
 *
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statfs.h>

#include "builder-utils.h"

#include "builder-git.h"
#include "builder-utils.h"
#include "flatpak-utils.h"

static gboolean
git (GFile   *dir,
     char   **output,
     GError **error,
     ...)
{
  gboolean res;
  va_list ap;

  va_start (ap, error);
  res = flatpak_spawn (dir, output, error, "git", ap);
  va_end (ap);

  return res;
}

static GFile *
git_get_mirror_dir (const char     *url_or_path,
                    BuilderContext *context)
{
  g_autoptr(GFile) git_dir = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *git_dir_path = NULL;

  git_dir = g_file_get_child (builder_context_get_state_dir (context),
                              "git");

  git_dir_path = g_file_get_path (git_dir);
  g_mkdir_with_parents (git_dir_path, 0755);

  /* Technically a path isn't a uri but if it's absolute it should still be unique. */
  filename = builder_uri_to_filename (url_or_path);
  return g_file_get_child (git_dir, filename);
}

static char *
git_get_current_commit (GFile          *repo_dir,
                        const char     *branch,
                        gboolean        ensure_commit,
                        BuilderContext *context,
                        GError        **error)
{
  char *output = NULL;
  g_autofree char *arg = NULL;

  if (ensure_commit)
    arg = g_strconcat (branch, "^{commit}", NULL);
  else
    arg = g_strdup (branch);

  if (!git (repo_dir, &output, error,
            "rev-parse", arg, NULL))
    return NULL;

  /* Trim trailing whitespace */
  g_strchomp (output);

  return output;
}

char *
builder_git_get_current_commit (const char     *repo_location,
                                const char     *branch,
                                gboolean        ensure_commit,
                                BuilderContext *context,
                                GError        **error)
{
  g_autoptr(GFile) mirror_dir = NULL;

  mirror_dir = git_get_mirror_dir (repo_location, context);
  return git_get_current_commit (mirror_dir, branch, ensure_commit, context, error);
}

static char *
make_absolute (const char *orig_parent, const char *orig_relpath, GError **error)
{
  g_autofree char *parent = g_strdup (orig_parent);
  const char *relpath = orig_relpath;
  char *start;
  char *parent_path;

  if (!g_str_has_prefix (relpath, "../"))
    return g_strdup (orig_relpath);

  if (parent[strlen (parent) - 1] == '/')
    parent[strlen (parent) - 1] = 0;

  if ((start = strstr (parent, "://")))
    start = start + 3;
  else
    start = parent;

  parent_path = strchr (start, '/');
  if (parent_path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid uri or path %s", orig_parent);
      return NULL;
    }

  while (g_str_has_prefix (relpath, "../"))
    {
      char *last_slash = strrchr (parent_path, '/');
      if (last_slash == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid relative path %s for uri or path %s", orig_relpath, orig_parent);
          return NULL;
        }
      relpath += 3;
      *last_slash = 0;
    }

  return g_strconcat (parent, "/", relpath, NULL);
}

static gboolean
git_mirror_submodules (const char     *repo_location,
                       const char     *destination_path,
                       gboolean        update,
                       GFile          *mirror_dir,
                       gboolean        disable_fsck,
                       const char     *revision,
                       BuilderContext *context,
                       GError        **error)
{
  g_autoptr(GKeyFile) key_file = g_key_file_new ();
  g_autofree gchar *rev_parse_output = NULL;
  g_autofree gchar *submodule_data = NULL;
  g_autofree gchar **submodules = NULL;
  g_autofree gchar *gitmodules = g_strconcat (revision, ":.gitmodules", NULL);
  gsize num_submodules;

  if (!git (mirror_dir, &rev_parse_output, NULL, "rev-parse", "--verify", "--quiet", gitmodules, NULL))
    return TRUE;

  if (git (mirror_dir, &submodule_data, NULL, "show", gitmodules, NULL))
    {
      if (!g_key_file_load_from_data (key_file, submodule_data, -1,
                                      G_KEY_FILE_NONE, error))
        return FALSE;

      submodules = g_key_file_get_groups (key_file, &num_submodules);

      int i;
      for (i = 0; i < num_submodules; i++)
        {
          g_autofree gchar *submodule = NULL;
          g_autofree gchar *path = NULL;
          g_autofree gchar *relative_url = NULL;
          g_autofree gchar *absolute_url = NULL;
          g_autofree gchar *ls_tree = NULL;
          g_auto(GStrv) lines = NULL;
          g_auto(GStrv) words = NULL;

          submodule = submodules[i];

          if (!g_str_has_prefix (submodule, "submodule \""))
            continue;

          path = g_key_file_get_string (key_file, submodule, "path", error);
          if (path == NULL)
            return FALSE;

          relative_url = g_key_file_get_string (key_file, submodule, "url", error);
          /* Remove any trailing whitespace */
          g_strchomp (relative_url);
          absolute_url = make_absolute (repo_location, relative_url, error);
          if (absolute_url == NULL)
            return FALSE;

          if (!git (mirror_dir, &ls_tree, error, "ls-tree", revision, path, NULL))
            return FALSE;

          lines = g_strsplit (g_strstrip (ls_tree), "\n", 0);
          if (g_strv_length (lines) != 1)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Not a gitlink tree: %s", path);
              return FALSE;
            }

          words = g_strsplit_set (lines[0], " \t", 4);

          if (g_strcmp0 (words[0], "160000") != 0)
            continue;

          if (!builder_git_mirror_repo (absolute_url, destination_path, update, TRUE, disable_fsck, words[2], context, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
builder_git_mirror_repo (const char     *repo_location,
                         const char     *destination_path,
                         gboolean        update,
                         gboolean        mirror_submodules,
                         gboolean        disable_fsck,
                         const char     *ref,
                         BuilderContext *context,
                         GError        **error)
{
  g_autoptr(GFile) cache_mirror_dir = NULL;
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *current_commit = NULL;

  cache_mirror_dir = git_get_mirror_dir (repo_location, context);

  if (destination_path != NULL)
    {
      g_autofree char *file_name = g_file_get_basename (cache_mirror_dir);
      g_autofree char *destination_file_path = g_build_filename (destination_path,
                                                                 file_name,
                                                                 NULL);
      mirror_dir = g_file_new_for_path (destination_file_path);
    }
  else
    mirror_dir = g_object_ref (cache_mirror_dir);

  if (!g_file_query_exists (mirror_dir, NULL))
    {
      g_autofree char *filename = g_file_get_basename (mirror_dir);
      g_autoptr(GFile) parent = g_file_get_parent (mirror_dir);
      g_autofree char *mirror_path = g_file_get_path (mirror_dir);
      g_autofree char *path_tmp = g_strconcat (mirror_path, ".clone_XXXXXX", NULL);
      g_autofree char *filename_tmp = NULL;
      g_autoptr(GFile) mirror_dir_tmp = NULL;
      g_autoptr(GFile) cached_git_dir = NULL;
      gboolean res;
      g_autoptr(GPtrArray) args = g_ptr_array_new ();

      if (g_mkdtemp_full (path_tmp, 0755) == NULL)
        return flatpak_fail (error, "Can't create temporary directory");

      mirror_dir_tmp = g_file_new_for_path (path_tmp);
      filename_tmp = g_file_get_basename (mirror_dir_tmp);

      g_ptr_array_add (args, "git");
      g_ptr_array_add (args, "clone");

      if (!disable_fsck)
        {
          g_ptr_array_add (args, "-c");
          g_ptr_array_add (args, "transfer.fsckObjects=1");
        }

      g_ptr_array_add (args, "--mirror");

      /* If we're doing a regular download, look for cache sources */
      if (destination_path == NULL)
        cached_git_dir = builder_context_find_in_sources_dirs (context, "git", filename, NULL);
      else
        cached_git_dir = g_object_ref (cache_mirror_dir);

      g_print ("Cloning git repo %s\n", repo_location);

      if (cached_git_dir && update)
        {
          g_ptr_array_add (args, "--reference");
          g_ptr_array_add (args, (char *)flatpak_file_get_path_cached (cached_git_dir));
        }

      /* Non-updating use of caches we just pull from the cache to avoid network i/o */
      if (cached_git_dir && !update)
        g_ptr_array_add (args, (char *)flatpak_file_get_path_cached (cached_git_dir));
      else
        g_ptr_array_add (args, (char *)repo_location);

      g_ptr_array_add (args, filename_tmp);
      g_ptr_array_add (args, NULL);

      res = flatpak_spawnv (parent, NULL, 0, error,
                            (const gchar * const *) args->pdata);

      if (cached_git_dir && !update &&
          !git (mirror_dir_tmp, NULL, error,
                "config", "--local", "remote.origin.url",
                repo_location, NULL))
        return FALSE;

      /* Ensure we copy the files from the cache, to be safe if the extra source changes */
      if (cached_git_dir && update)
        {
          g_autoptr(GFile) alternates = g_file_resolve_relative_path (mirror_dir_tmp, "objects/info/alternates");

          if (!git (mirror_dir_tmp, NULL, error,
                    "repack", "-a", "-d", NULL))
            return FALSE;

          g_file_delete (alternates, NULL, NULL);
        }

      if (!res || !g_file_move (mirror_dir_tmp, mirror_dir, 0, NULL, NULL, NULL, error))
        return FALSE;
    }
  else if (update)
    {
      g_print ("Fetching git repo %s\n", repo_location);
      if (!git (mirror_dir, NULL, error,
                "fetch", "-p", NULL))
        return FALSE;
    }

  if (mirror_submodules)
    {
      current_commit = git_get_current_commit (mirror_dir, ref, FALSE, context, error);
      if (current_commit == NULL)
        return FALSE;

      if (!git_mirror_submodules (repo_location, destination_path, update,
                                  mirror_dir, disable_fsck, current_commit, context, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
git_extract_submodule (const char     *repo_location,
                       GFile          *checkout_dir,
                       const char     *revision,
                       BuilderContext *context,
                       GError        **error)
{
  g_autoptr(GKeyFile) key_file = g_key_file_new ();
  g_autofree gchar *rev_parse_output = NULL;
  g_autofree gchar *submodule_data = NULL;
  g_autofree gchar **submodules = NULL;
  g_autofree gchar *gitmodules = g_strconcat (revision, ":.gitmodules", NULL);
  gsize num_submodules;

  if (!git (checkout_dir, &rev_parse_output, NULL, "rev-parse", "--verify", "--quiet", gitmodules, NULL))
    return TRUE;

  if (git (checkout_dir, &submodule_data, NULL, "show", gitmodules, NULL))
    {
      if (!g_key_file_load_from_data (key_file, submodule_data, -1,
                                      G_KEY_FILE_NONE, error))
        return FALSE;

      submodules = g_key_file_get_groups (key_file, &num_submodules);

      int i;
      for (i = 0; i < num_submodules; i++)
        {
          g_autofree gchar *submodule = NULL;
          g_autofree gchar *name = NULL;
          g_autofree gchar *update_method = NULL;
          g_autofree gchar *path = NULL;
          g_autofree gchar *relative_url = NULL;
          g_autofree gchar *absolute_url = NULL;
          g_autofree gchar *ls_tree = NULL;
          g_auto(GStrv) lines = NULL;
          g_auto(GStrv) words = NULL;
          g_autoptr(GFile) mirror_dir = NULL;
          g_autoptr(GFile) child_dir = NULL;
          g_autofree gchar *mirror_dir_as_url = NULL;
          g_autofree gchar *option = NULL;
          gsize len;

          submodule = submodules[i];
          len = strlen (submodule);

          if (!g_str_has_prefix (submodule, "submodule \""))
            continue;

          name = g_strndup (submodule + 11, len - 12);

          /* Skip any submodules that are disabled (have the update method set to "none")
             Only check if the command succeeds. If it fails, the update method is not set. */
          update_method = g_key_file_get_string (key_file, submodule, "update", NULL);
          if (g_strcmp0 (update_method, "none") == 0)
            continue;

          path = g_key_file_get_string (key_file, submodule, "path", error);
          if (path == NULL)
            return FALSE;

          relative_url = g_key_file_get_string (key_file, submodule, "url", error);
          absolute_url = make_absolute (repo_location, relative_url, error);
          if (absolute_url == NULL)
            return FALSE;

          if (!git (checkout_dir, &ls_tree, error, "ls-tree", revision, path, NULL))
            return FALSE;

          lines = g_strsplit (g_strstrip (ls_tree), "\n", 0);
          if (g_strv_length (lines) != 1)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Not a gitlink tree: %s", path);
              return FALSE;
            }

          words = g_strsplit_set (lines[0], " \t", 4);

          if (g_strcmp0 (words[0], "160000") != 0)
            continue;

          mirror_dir = git_get_mirror_dir (absolute_url, context);
          mirror_dir_as_url = g_file_get_uri (mirror_dir);
          option = g_strdup_printf ("submodule.%s.url", name);

          if (!git (checkout_dir, NULL, error,
                    "config", option, mirror_dir_as_url, NULL))
            return FALSE;

          if (!git (checkout_dir, NULL, error,
                    "submodule", "update", "--init", path, NULL))
            return FALSE;

          child_dir = g_file_resolve_relative_path (checkout_dir, path);

          if (!git_extract_submodule (absolute_url, child_dir, words[2], context, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
builder_git_checkout_dir (const char     *repo_location,
                          const char     *branch,
                          const char     *dir,
                          GFile          *dest,
                          BuilderContext *context,
                          GError        **error)
{
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *mirror_dir_path = NULL;
  g_autofree char *dest_path = NULL;

  mirror_dir = git_get_mirror_dir (repo_location, context);

  mirror_dir_path = g_file_get_path (mirror_dir);
  dest_path = g_file_get_path (dest);

  if (!git (NULL, NULL, error,
            "clone", "-n", mirror_dir_path, dest_path, NULL))
    return FALSE;

  if (!git (dest, NULL, error,
            "checkout", branch, "--", dir ? dir : ".", NULL))
    return FALSE;

  return TRUE;
}

gboolean
builder_git_checkout (const char     *repo_location,
                      const char     *branch,
                      GFile          *dest,
                      BuilderContext *context,
                      GError        **error)
{
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *mirror_dir_path = NULL;
  g_autofree char *dest_path = NULL;

  mirror_dir = git_get_mirror_dir (repo_location, context);

  mirror_dir_path = g_file_get_path (mirror_dir);
  dest_path = g_file_get_path (dest);

  if (!git (NULL, NULL, error,
            "clone", mirror_dir_path, dest_path, NULL))
    return FALSE;

  if (!git (dest, NULL, error,
            "checkout", branch, NULL))
    return FALSE;

  if (!git_extract_submodule (repo_location, dest, branch, context, error))
    return FALSE;

  if (!git (dest, NULL, error,
            "config", "--local", "remote.origin.url",
            repo_location, NULL))
    return FALSE;

  return TRUE;
}
