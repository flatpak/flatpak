/* builder-cache.c
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

#include <gio/gio.h>
#include <ostree.h>
#include "libglnx/libglnx.h"
#include "libgsystem.h"

#include "xdg-app-utils.h"
#include "builder-utils.h"
#include "builder-cache.h"

struct BuilderCache {
  GObject parent;
  GChecksum *checksum;
  GFile *cache_dir;
  GFile *app_dir;
  char *branch;
  char *last_parent;
  OstreeRepo *repo;
  gboolean disabled;
};

typedef struct {
  GObjectClass parent_class;
} BuilderCacheClass;

G_DEFINE_TYPE (BuilderCache, builder_cache, G_TYPE_OBJECT);

enum {
  PROP_0,
  PROP_CACHE_DIR,
  PROP_APP_DIR,
  PROP_BRANCH,
  LAST_PROP
};

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

static void
builder_cache_finalize (GObject *object)
{
  BuilderCache *self = (BuilderCache *)object;

  g_clear_object (&self->cache_dir);
  g_clear_object (&self->app_dir);
  g_checksum_free (self->checksum);
  g_free (self->branch);
  g_free (self->last_parent);

  G_OBJECT_CLASS (builder_cache_parent_class)->finalize (object);
}

static void
builder_cache_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  BuilderCache *self = BUILDER_CACHE (object);

  switch (prop_id)
    {
    case PROP_CACHE_DIR:
      g_value_set_object (value, self->cache_dir);
      break;

    case PROP_APP_DIR:
      g_value_set_object (value, self->app_dir);
      break;

    case PROP_BRANCH:
      g_value_set_string (value, self->branch);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_cache_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  BuilderCache *self = BUILDER_CACHE (object);

  switch (prop_id)
    {
    case PROP_BRANCH:
      g_free (self->branch);
      self->branch = g_value_dup_string (value);
      break;

    case PROP_CACHE_DIR:
      g_set_object (&self->cache_dir, g_value_get_object (value));
      break;

    case PROP_APP_DIR:
      g_set_object (&self->app_dir, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_cache_class_init (BuilderCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_cache_finalize;
  object_class->get_property = builder_cache_get_property;
  object_class->set_property = builder_cache_set_property;

  g_object_class_install_property (object_class,
                                   PROP_CACHE_DIR,
                                   g_param_spec_object ("cache-dir",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE|G_PARAM_CONSTRUCT));
  g_object_class_install_property (object_class,
                                   PROP_APP_DIR,
                                   g_param_spec_object ("app-dir",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE|G_PARAM_CONSTRUCT));
  g_object_class_install_property (object_class,
                                   PROP_BRANCH,
                                   g_param_spec_string ("branch",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_cache_init (BuilderCache *self)
{
  self->checksum = g_checksum_new (G_CHECKSUM_SHA256);
}

BuilderCache *
builder_cache_new (GFile *cache_dir,
                   GFile *app_dir,
                   const char *branch)
{
  return g_object_new (BUILDER_TYPE_CACHE,
                       "cache-dir", cache_dir,
                       "app-dir", app_dir,
                       "branch", branch,
                       NULL);
}

GChecksum *
builder_cache_get_checksum (BuilderCache *self)
{
  return self->checksum;
}

gboolean
builder_cache_open (BuilderCache *self,
                    GError **error)
{
  self->repo = ostree_repo_new (self->cache_dir);

  /* We don't need fsync on checkouts as they are transient, and we
     rely on the syncfs() in the transaction commit for commits. */
  ostree_repo_set_disable_fsync (self->repo, TRUE);

  if (!g_file_query_exists (self->cache_dir, NULL))
    {
      if (!ostree_repo_create (self->repo, OSTREE_REPO_MODE_BARE_USER, NULL, error))
        return FALSE;
    }

  if (!ostree_repo_open (self->repo, NULL, error))
    return FALSE;

  return TRUE;
}

static char *
builder_cache_get_current (BuilderCache *self)
{
  g_autoptr(GChecksum) copy = g_checksum_copy (self->checksum);

  return g_strdup (g_checksum_get_string (copy));
}

static gboolean
builder_cache_checkout (BuilderCache *self, const char *commit)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFileInfo) file_info = NULL;

  if (!ostree_repo_read_commit (self->repo, commit, &root, NULL, NULL, NULL))
    return FALSE;

  file_info = g_file_query_info (root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, NULL);
  if (file_info == NULL)
    return FALSE;

  /* We check out without user mode, not necessarily because we care
     about uids not owned by the user (they are all from the build,
     so should be creatable by the user, but because we want to
     force the checkout to not use hardlinks. Hard links into the
     cache are not safe, as the build could mutate these. */
  if (!ostree_repo_checkout_tree (self->repo,
                                  OSTREE_REPO_CHECKOUT_MODE_NONE,
                                  OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
                                  self->app_dir,
                                  OSTREE_REPO_FILE (root), file_info,
                                  NULL, NULL))
    return FALSE;

  return TRUE;
}

void
builder_cache_ensure_checkout (BuilderCache *self)
{
  if (self->disabled)
    return;

  if (self->last_parent)
    {
      g_print ("Everything cached, checking out from cache\n");

      if (!builder_cache_checkout (self, self->last_parent))
        g_error ("Failed to check out cache");
    }
}

gboolean
builder_cache_lookup (BuilderCache *self)
{
  g_autofree char *current = NULL;
  g_autofree char *commit = NULL;

  if (self->disabled)
    return FALSE;

  if (!ostree_repo_resolve_rev (self->repo, self->branch, TRUE, &commit, NULL))
    goto checkout;

  current = builder_cache_get_current (self);

  while (commit != NULL)
    {
      g_autoptr(GVariant) variant = NULL;
      const gchar *subject;

      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT, commit,
                                     &variant, NULL))
        goto checkout;

      g_variant_get (variant, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                     &subject, NULL, NULL, NULL, NULL);

      if (strcmp (subject, current) == 0)
        {
          g_free (self->last_parent);
          self->last_parent = g_steal_pointer (&commit);

          return TRUE;
        }

      g_free (commit);
      commit = ostree_commit_get_parent (variant);
    }

 checkout:
  if (self->last_parent)
    {
      g_print ("Cache miss, checking out last cache hit\n");

      if (!builder_cache_checkout (self, self->last_parent))
        g_error ("Failed to check out cache");
    }

  self->disabled = TRUE; /* Don't use cache any more after first miss */

  return FALSE;
}

gboolean
builder_cache_commit (BuilderCache  *self,
                      const char *body,
                      GError       **error)
{
  g_autofree char *current = NULL;
  OstreeRepoCommitModifier *modifier = NULL;
  g_autoptr(OstreeMutableTree) mtree = NULL;
  g_autoptr(GFile) root = NULL;
  g_autofree char *commit_checksum = NULL;
  gboolean res = FALSE;

  if (!ostree_repo_prepare_transaction (self->repo, NULL, NULL, error))
    return FALSE;

  mtree = ostree_mutable_tree_new ();

  modifier = ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS,
                                              NULL, NULL, NULL);
  if (!ostree_repo_write_directory_to_mtree (self->repo, self->app_dir,
                                             mtree, modifier, NULL, error))
    goto out;

  if (!ostree_repo_write_mtree (self->repo, mtree, &root, NULL, error))
    goto out;

  current = builder_cache_get_current (self);

  if (!ostree_repo_write_commit (self->repo, self->last_parent, current, body, NULL,
                                 OSTREE_REPO_FILE (root),
                                 &commit_checksum, NULL, error))
    goto out;

  ostree_repo_transaction_set_ref (self->repo, NULL, self->branch, commit_checksum);

  if (!ostree_repo_commit_transaction (self->repo, NULL, NULL, error))
    goto out;

  g_free (self->last_parent);
  self->last_parent = g_steal_pointer (&commit_checksum);

  res = TRUE;

 out:
  if (!res)
    {
      if (!ostree_repo_abort_transaction (self->repo, NULL, NULL))
        g_warning ("failed to abort transaction");
    }
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);

  return res;
}

GPtrArray *
builder_cache_get_changes (BuilderCache  *self,
                           GError       **error)
{
  g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GPtrArray) modified = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_diff_item_unref);
  g_autoptr(GPtrArray) removed = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GPtrArray) added_paths = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GFile) current_root = NULL;
  g_autoptr(GFile) current_files = NULL;
  g_autoptr(GFile) parent_root = NULL;
  g_autoptr(GFile) parent_files = NULL;
  g_autoptr(GVariant) variant = NULL;
  g_autofree char *parent_commit = NULL;
  int i;

  if (!ostree_repo_read_commit (self->repo, self->last_parent, &current_root, NULL, NULL, error))
    return NULL;

  current_files = g_file_get_child (current_root, "files");

  if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_COMMIT, self->last_parent,
                                 &variant, NULL))
    return NULL;

  parent_commit = ostree_commit_get_parent (variant);
  if (parent_commit != NULL)
    {
      if (!ostree_repo_read_commit (self->repo, parent_commit, &parent_root, NULL, NULL, error))
        return FALSE;
      parent_files = g_file_get_child (parent_root, "files");
    }

  if (!ostree_diff_dirs (OSTREE_DIFF_FLAGS_NONE,
                         parent_files,
                         current_files,
                         modified,
                         removed,
                         added,
                         NULL, error))
    return NULL;

  for (i = 0; i < added->len; i++)
    {
      char *path = g_file_get_relative_path (current_files, g_ptr_array_index (added, i));
      g_ptr_array_add (added_paths, path);
    }

  return g_steal_pointer (&added_paths);
}

void
builder_cache_disable_lookups (BuilderCache  *self)
{
  self->disabled = TRUE;
}

gboolean
builder_gc (BuilderCache  *self,
            GError       **error)
{
  gint objects_total;
  gint objects_pruned;
  guint64 pruned_object_size_total;

  return ostree_repo_prune (self->repo,
                            OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, -1,
                            &objects_total,
                            &objects_pruned,
                            &pruned_object_size_total,
                            NULL, error);
}

void
builder_cache_checksum_str (BuilderCache  *self,
                            const char    *str)
{
  /* We include the terminating zero so that we make
   * a difference between NULL and "". */

  if (str)
    g_checksum_update (self->checksum, (const guchar *)str, strlen (str) + 1);
  else
    /* Always add something so we can't be fooled by a sequence like
       NULL, "a" turning into "a", NULL. */
    g_checksum_update (self->checksum, (const guchar *)"\1", 1);
}

void
builder_cache_checksum_strv (BuilderCache  *self,
                             char         **strv)
{
  int i;

  if (strv)
    {
      g_checksum_update (self->checksum, (const guchar *)"\1", 1);
      for (i = 0; strv[i] != NULL; i++)
        builder_cache_checksum_str (self, strv[i]);
    }
  else
    g_checksum_update (self->checksum, (const guchar *)"\2", 1);
}

void
builder_cache_checksum_boolean (BuilderCache  *self,
                                gboolean       val)
{
  if (val)
    g_checksum_update (self->checksum, (const guchar *)"\1", 1);
  else
    g_checksum_update (self->checksum, (const guchar *)"\0", 1);
}

void
builder_cache_checksum_uint32  (BuilderCache  *self,
                                guint32        val)
{
  guchar v[4];
  v[0] = (val >> 0) & 0xff;
  v[1] = (val >> 8) & 0xff;
  v[2] = (val >> 16) & 0xff;
  v[3] = (val >> 24) & 0xff;
  g_checksum_update (self->checksum, v, 4);
}

void
builder_cache_checksum_data (BuilderCache  *self,
                             guint8        *data,
                             gsize          len)
{
  g_checksum_update (self->checksum, data, len);
}
