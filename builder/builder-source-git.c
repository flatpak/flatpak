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
#include "libgsystem.h"

#include "builder-source-git.h"
#include "builder-utils.h"
#include "flatpak-utils.h"

struct BuilderSourceGit
{
  BuilderSource parent;

  char         *url;
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
git_get_mirror_dir (const char     *url,
                    BuilderContext *context)
{
  g_autoptr(GFile) git_dir = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *git_dir_path = NULL;

  git_dir = g_file_get_child (builder_context_get_state_dir (context),
                              "git");

  git_dir_path = g_file_get_path (git_dir);
  g_mkdir_with_parents (git_dir_path, 0755);

  filename = builder_uri_to_filename (url);
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
get_url (BuilderSourceGit *self,
         BuilderContext   *context,
         GError          **error)
{
  g_autofree char *scheme = NULL;

  if (self->url == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "URL not specified");
      return NULL;
    }

  scheme = g_uri_parse_scheme (self->url);
  if (scheme == NULL)
    {
      g_autoptr(GFile) repo =
        g_file_resolve_relative_path (builder_context_get_base_dir (context),
                                      self->url);
      return g_file_get_uri (repo);
    }

  return g_strdup (self->url);
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
make_absolute_url (const char *orig_parent, const char *orig_relpath, GError **error)
{
  g_autofree char *parent = g_strdup (orig_parent);
  const char *relpath = orig_relpath;
  char *method;
  char *parent_path;

  if (!g_str_has_prefix (relpath, "../"))
    return g_strdup (orig_relpath);

  if (parent[strlen (parent) - 1] == '/')
    parent[strlen (parent) - 1] = 0;

  method = strstr (parent, "://");
  if (method == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid uri %s", orig_parent);
      return NULL;
    }

  parent_path = strchr (method + 3, '/');
  if (parent_path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid uri %s", orig_parent);
      return NULL;
    }

  while (g_str_has_prefix (relpath, "../"))
    {
      char *last_slash = strrchr (parent_path, '/');
      if (last_slash == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid relative path %s for uri %s", orig_relpath, orig_parent);
          return NULL;
        }
      relpath += 3;
      *last_slash = 0;
    }

  return g_strconcat (parent, "/", relpath, NULL);
}

static gboolean
git_mirror_submodules (const char     *repo_url,
                       gboolean        update,
                       GFile          *mirror_dir,
                       const char     *revision,
                       BuilderContext *context,
                       GError        **error)
{
  g_autofree char *mirror_dir_path = NULL;

  g_autoptr(GFile) checkout_dir_template = NULL;
  g_autoptr(GFile) checkout_dir = NULL;
  g_autofree char *checkout_dir_path = NULL;
  g_autofree char *submodule_status = NULL;

  mirror_dir_path = g_file_get_path (mirror_dir);

  checkout_dir_template = g_file_get_child (builder_context_get_state_dir (context),
                                            "tmp-checkout-XXXXXX");
  checkout_dir_path = g_file_get_path (checkout_dir_template);

  if (g_mkdtemp (checkout_dir_path) == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't create temporary checkout directory");
      return FALSE;
    }

  checkout_dir = g_file_new_for_path (checkout_dir_path);

  if (!git (NULL, NULL, error,
            "clone", "-q", "--no-checkout", mirror_dir_path, checkout_dir_path, NULL))
    return FALSE;

  if (!git (checkout_dir, NULL, error, "checkout", "-q", "-f", revision, NULL))
    return FALSE;

  if (!git (checkout_dir, &submodule_status, error,
            "submodule", "status", NULL))
    return FALSE;

  if (submodule_status)
    {
      int i;
      g_auto(GStrv) lines = g_strsplit (submodule_status, "\n", -1);
      for (i = 0; lines[i] != NULL; i++)
        {
          g_autofree char *url = NULL;
          g_autofree char *option = NULL;
          g_autofree char *old = NULL;
          g_auto(GStrv) words = NULL;
          if (*lines[i] == 0)
            continue;
          words = g_strsplit (lines[i] + 1, " ", 3);

          option = g_strdup_printf ("submodule.%s.url", words[1]);
          if (!git (checkout_dir, &url, error,
                    "config", "-f", ".gitmodules", option, NULL))
            return FALSE;
          /* Trim trailing whitespace */
          g_strchomp (url);

          old = url;
          url = make_absolute_url (repo_url, old, error);
          if (url == NULL)
            return FALSE;

          if (!git_mirror_repo (url, update, words[0], context, error))
            return FALSE;
        }
    }

  if (!gs_shutil_rm_rf (checkout_dir, NULL, error))
    return FALSE;

  return TRUE;
}

static gboolean
git_mirror_repo (const char     *repo_url,
                 gboolean        update,
                 const char     *ref,
                 BuilderContext *context,
                 GError        **error)
{
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *current_commit = NULL;

  mirror_dir = git_get_mirror_dir (repo_url, context);

  if (!g_file_query_exists (mirror_dir, NULL))
    {
      g_autofree char *filename = g_file_get_basename (mirror_dir);
      g_autoptr(GFile) parent = g_file_get_parent (mirror_dir);
      g_autofree char *filename_tmp = g_strconcat (filename, ".clone_tmp", NULL);
      g_autoptr(GFile) mirror_dir_tmp = g_file_get_child (parent, filename_tmp);

      g_print ("Cloning git repo %s\n", repo_url);

      if (!git (parent, NULL, error,
                "clone", "--mirror", repo_url,  filename_tmp, NULL) ||
          !g_file_move (mirror_dir_tmp, mirror_dir, 0, NULL, NULL, NULL, error))
        return FALSE;
    }
  else if (update)
    {
      g_print ("Fetching git repo %s\n", repo_url);
      if (!git (mirror_dir, NULL, error,
                "fetch", "-p", NULL))
        return FALSE;
    }

  current_commit = git_get_current_commit (mirror_dir, ref, context, error);
  if (current_commit == NULL)
    return FALSE;

  if (!git_mirror_submodules (repo_url, update, mirror_dir, current_commit, context, error))
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

  url = get_url (self, context, error);
  if (url == NULL)
    return FALSE;

  if (!git_mirror_repo (url,
                        update_vcs,
                        get_branch (self),
                        context,
                        error))
    return FALSE;

  return TRUE;
}

static gboolean
git_extract_submodule (const char     *repo_url,
                       GFile          *checkout_dir,
                       BuilderContext *context,
                       GError        **error)
{
  g_autofree char *submodule_status = NULL;

  if (!git (checkout_dir, &submodule_status, error,
            "submodule", "status", NULL))
    return FALSE;

  if (submodule_status)
    {
      int i;
      g_auto(GStrv) lines = g_strsplit (submodule_status, "\n", -1);
      for (i = 0; lines[i] != NULL; i++)
        {
          g_autoptr(GFile) mirror_dir = NULL;
          g_autoptr(GFile) child_dir = NULL;
          g_autofree char *child_url = NULL;
          g_autofree char *option = NULL;
          g_autofree char *update_method = NULL;
          g_autofree char *child_relative_url = NULL;
          g_autofree char *mirror_dir_as_url = NULL;
          g_auto(GStrv) words = NULL;
          if (*lines[i] == 0)
            continue;
          words = g_strsplit (lines[i] + 1, " ", 3);

          /* Skip any submodules that are disabled (have the update method set to "none")
             Only check if the command succeeds. If it fails, the update method is not set. */
          option = g_strdup_printf ("submodule.%s.update", words[1]);
          if (git (checkout_dir, &update_method, NULL,
                   "config", "-f", ".gitmodules", option, NULL))
            {
              /* Trim trailing whitespace */
              g_strchomp (update_method);

              if (g_strcmp0 (update_method, "none") == 0)
                continue;
            }

          option = g_strdup_printf ("submodule.%s.url", words[1]);
          if (!git (checkout_dir, &child_relative_url, error,
                    "config", "-f", ".gitmodules", option, NULL))
            return FALSE;

          /* Trim trailing whitespace */
          g_strchomp (child_relative_url);

          g_print ("processing submodule %s\n", words[1]);

          child_url = make_absolute_url (repo_url, child_relative_url, error);
          if (child_url == NULL)
            return FALSE;

          mirror_dir = git_get_mirror_dir (child_url, context);

          mirror_dir_as_url = g_file_get_uri (mirror_dir);

          if (!git (checkout_dir, NULL, error,
                    "config", option, mirror_dir_as_url, NULL))
            return FALSE;

          if (!git (checkout_dir, NULL, error,
                    "submodule", "update", "--init", words[1], NULL))
            return FALSE;

          child_dir = g_file_resolve_relative_path (checkout_dir, words[1]);

          if (!git_extract_submodule (child_url, child_dir, context, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
builder_source_git_extract (BuilderSource  *source,
                            GFile          *dest,
                            BuilderContext *context,
                            GError        **error)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);

  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *mirror_dir_path = NULL;
  g_autofree char *dest_path = NULL;
  g_autofree char *url = NULL;

  url = get_url (self, context, error);
  if (url == NULL)
    return FALSE;

  mirror_dir = git_get_mirror_dir (url, context);

  mirror_dir_path = g_file_get_path (mirror_dir);
  dest_path = g_file_get_path (dest);

  if (!git (NULL, NULL, error,
            "clone", mirror_dir_path, dest_path, NULL))
    return FALSE;

  if (!git (dest, NULL, error,
            "checkout", get_branch (self), NULL))
    return FALSE;

  if (!git_extract_submodule (url, dest, context, error))
    return FALSE;

  if (!git (dest, NULL, error,
            "config", "--local", "remote.origin.url", url, NULL))
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
  g_autofree char *url = NULL;

  builder_cache_checksum_str (cache, self->url);
  builder_cache_checksum_str (cache, self->branch);

  url = get_url (self, context, &error);
  if (url != NULL)
    {
      mirror_dir = git_get_mirror_dir (url, context);

      current_commit = git_get_current_commit (mirror_dir, get_branch (self), context, &error);
      if (current_commit)
        builder_cache_checksum_str (cache, current_commit);
      else if (error)
        g_warning ("Failed to get current git checksum: %s", error->message);
    }
  else
    {
      g_warning ("No url");
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
  g_autofree char *url = NULL;

  url = get_url (self, context, error);
  if (url == NULL)
    return FALSE;

  mirror_dir = git_get_mirror_dir (url, context);

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
