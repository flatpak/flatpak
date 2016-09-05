/* builder-source-git.c
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

#include "builder-source-git.h"
#include "builder-utils.h"
#include "flatpak-utils.h"

struct BuilderSourceGit
{
  BuilderSource parent;

  char         *url;
  char         *path;
  char         *branch;
};

typedef struct
{
  BuilderSourceClass parent_class;
} BuilderSourceGitClass;

G_DEFINE_TYPE (BuilderSourceGit, builder_source_git, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_URL,
  PROP_PATH,
  PROP_BRANCH,
  LAST_PROP
};

static gboolean git_mirror_repo (const char     *repo_url,
                                 gboolean        update,
                                 const char     *ref,
                                 BuilderContext *context,
                                 GError        **error);


static void
builder_source_git_finalize (GObject *object)
{
  BuilderSourceGit *self = (BuilderSourceGit *) object;

  g_free (self->url);
  g_free (self->path);
  g_free (self->branch);

  G_OBJECT_CLASS (builder_source_git_parent_class)->finalize (object);
}

static void
builder_source_git_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_value_set_string (value, self->url);
      break;

    case PROP_PATH:
      g_value_set_string (value, self->path);
      break;

    case PROP_BRANCH:
      g_value_set_string (value, self->branch);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_git_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_free (self->url);
      self->url = g_value_dup_string (value);
      break;

    case PROP_PATH:
      g_free (self->path);
      self->path = g_value_dup_string (value);
      break;

    case PROP_BRANCH:
      g_free (self->branch);
      self->branch = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

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

static const char *
get_branch (BuilderSourceGit *self)
{
  if (self->branch)
    return self->branch;
  else
    return "master";
}

static char *
get_url_or_path (BuilderSourceGit *self,
                 BuilderContext   *context,
                 GError          **error)
{
  g_autoptr(GFile) repo = NULL;

  if (self->url == NULL && self->path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No URL or path specified");
      return NULL;
    }

  if (self->url)
    {
      g_autofree char *scheme = NULL;
      scheme = g_uri_parse_scheme (self->url);
      if (scheme == NULL)
        {
          repo = g_file_resolve_relative_path (builder_context_get_base_dir (context),
                                               self->url);
          return g_file_get_uri (repo);
        }

      return g_strdup (self->url);
    }

  repo = g_file_resolve_relative_path (builder_context_get_base_dir (context),
                                       self->path);
  return g_file_get_path (repo);
}

static char *
git_get_current_commit (GFile          *repo_dir,
                        const char     *branch,
                        BuilderContext *context,
                        GError        **error)
{
  char *output = NULL;

  if (!git (repo_dir, &output, error,
            "rev-parse", branch, NULL))
    return NULL;

  /* Trim trailing whitespace */
  g_strchomp (output);

  return output;
}

char *
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
                       gboolean        update,
                       GFile          *mirror_dir,
                       const char     *revision,
                       BuilderContext *context,
                       GError        **error)
{
  g_autofree char *mirror_dir_path = NULL;

  g_autoptr(GKeyFile) key_file = g_key_file_new ();
  g_autofree gchar *submodule_data = NULL;
  g_autofree gchar **submodules = NULL;
  g_autofree gchar *gitmodules = g_strconcat (revision, ":.gitmodules", NULL);
  gsize num_submodules;

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

          if (g_strcmp0 (words[1], "commit") != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Not a gitlink tree: %s", path);
              return FALSE;
            }

          if (!git_mirror_repo (absolute_url, update, words[2], context, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
git_mirror_repo (const char     *repo_location,
                 gboolean        update,
                 const char     *ref,
                 BuilderContext *context,
                 GError        **error)
{
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *current_commit = NULL;

  mirror_dir = git_get_mirror_dir (repo_location, context);

  if (!g_file_query_exists (mirror_dir, NULL))
    {
      g_autofree char *filename = g_file_get_basename (mirror_dir);
      g_autoptr(GFile) parent = g_file_get_parent (mirror_dir);
      g_autofree char *filename_tmp = g_strconcat (filename, ".clone_tmp", NULL);
      g_autoptr(GFile) mirror_dir_tmp = g_file_get_child (parent, filename_tmp);

      g_print ("Cloning git repo %s\n", repo_location);

      if (!git (parent, NULL, error,
                "clone", "--mirror", repo_location,  filename_tmp, NULL) ||
          !g_file_move (mirror_dir_tmp, mirror_dir, 0, NULL, NULL, NULL, error))
        return FALSE;
    }
  else if (update)
    {
      g_print ("Fetching git repo %s\n", repo_location);
      if (!git (mirror_dir, NULL, error,
                "fetch", "-p", NULL))
        return FALSE;
    }

  current_commit = git_get_current_commit (mirror_dir, ref, context, error);
  if (current_commit == NULL)
    return FALSE;

  if (!git_mirror_submodules (repo_location, update, mirror_dir, current_commit, context, error))
    return FALSE;

  return TRUE;
}

static gboolean
builder_source_git_download (BuilderSource  *source,
                             gboolean        update_vcs,
                             BuilderContext *context,
                             GError        **error)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);
  g_autofree char *url = NULL;
  g_autofree char *location = NULL;

  location = get_url_or_path (self, context, error);
  if (location == NULL)
    return FALSE;

  if (!git_mirror_repo (location,
                        update_vcs,
                        get_branch (self),
                        context,
                        error))
    return FALSE;

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
  g_autofree gchar *submodule_data = NULL;
  g_autofree gchar **submodules = NULL;
  g_autofree gchar *gitmodules = g_strconcat (revision, ":.gitmodules", NULL);
  gsize num_submodules;

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

          if (g_strcmp0 (words[1], "commit") != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Not a gitlink tree: %s", path);
              return FALSE;
            }

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

static gboolean
builder_source_git_extract (BuilderSource  *source,
                            GFile          *dest,
                            BuilderOptions *build_options,
                            BuilderContext *context,
                            GError        **error)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);

  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *mirror_dir_path = NULL;
  g_autofree char *dest_path = NULL;
  g_autofree char *location = NULL;

  location = get_url_or_path (self, context, error);
  if (location == NULL)
    return FALSE;

  mirror_dir = git_get_mirror_dir (location, context);

  mirror_dir_path = g_file_get_path (mirror_dir);
  dest_path = g_file_get_path (dest);

  if (!git (NULL, NULL, error,
            "clone", mirror_dir_path, dest_path, NULL))
    return FALSE;

  if (!git (dest, NULL, error,
            "checkout", get_branch (self), NULL))
    return FALSE;

  if (!git_extract_submodule (location, dest, get_branch (self), context, error))
    return FALSE;

  if (!git (dest, NULL, error,
            "config", "--local", "remote.origin.url",
            location, NULL))
    return FALSE;

  return TRUE;
}

static void
builder_source_git_checksum (BuilderSource  *source,
                             BuilderCache   *cache,
                             BuilderContext *context)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);

  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *current_commit = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *location = NULL;

  builder_cache_checksum_str (cache, self->url);
  builder_cache_checksum_str (cache, self->path);
  builder_cache_checksum_str (cache, self->branch);

  location = get_url_or_path (self, context, &error);
  if (location != NULL)
    {
      mirror_dir = git_get_mirror_dir (location, context);

      current_commit = git_get_current_commit (mirror_dir, get_branch (self), context, &error);
      if (current_commit)
        builder_cache_checksum_str (cache, current_commit);
      else if (error)
        g_warning ("Failed to get current git checksum: %s", error->message);
    }
  else
    {
      g_warning ("No url or path");
    }
}

static gboolean
builder_source_git_update (BuilderSource  *source,
                           BuilderContext *context,
                           GError        **error)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);

  g_autoptr(GFile) mirror_dir = NULL;
  char *current_commit;
  g_autofree char *location = NULL;

  location = get_url_or_path (self, context, error);
  if (location == NULL)
    return FALSE;

  mirror_dir = git_get_mirror_dir (location, context);

  current_commit = git_get_current_commit (mirror_dir, get_branch (self), context, NULL);
  if (current_commit)
    {
      g_free (self->branch);
      self->branch = current_commit;
    }

  return TRUE;
}

static void
builder_source_git_class_init (BuilderSourceGitClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_git_finalize;
  object_class->get_property = builder_source_git_get_property;
  object_class->set_property = builder_source_git_set_property;

  source_class->download = builder_source_git_download;
  source_class->extract = builder_source_git_extract;
  source_class->update = builder_source_git_update;
  source_class->checksum = builder_source_git_checksum;

  g_object_class_install_property (object_class,
                                   PROP_URL,
                                   g_param_spec_string ("url",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BRANCH,
                                   g_param_spec_string ("branch",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_source_git_init (BuilderSourceGit *self)
{
}
