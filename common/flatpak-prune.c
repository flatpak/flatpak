/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2021 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "flatpak-error.h"
#include "flatpak-prune-private.h"
#include "flatpak-variant-private.h"
#include "flatpak-variant-impl-private.h"
#include "libglnx.h"
#include "valgrind-private.h"

/* This is a custom implementation of ostree-prune that caches the
 * traversal for better performance on larger repos. It also merges the list-object
 * and prune operation to avoid allocating a lot of memory for the list of all
 * objects in the repo.
 *
 * Locking strategy:
 *
 * Ostree supports three kinds of approaches to handling parallel access to
 * the repo.
 *
 * EXCLUSIVE LOCK:
 *  All global operations that modify the repo state take an exclusive lock on the
 *  repo which means no other repo-modifying operation is allowed in parallel. This
 *  is currently only done for pruning and summary generation. Prune for instance
 *  is global; it traverses from a set of root commits and assumes that everything
 *  that isn't reachable can be deleted, which is not compatible with adding a
 *  new commit that doesn't have a root commit yet.
 *  NOTE: Whenever objects are deleted we always hold an exclusive lock.
 *
 * SHARED LOCKS:
 *  Operations that do local modifications take a shared lock. This means we can
 *  have multiple such operations in parallel with each other, but not in parallel
 *  with an exclusive lock. The typical operation that does this is the commit.
 *  During a commit we don't add to the transaction objects that already exist
 *  in the repo, so we rely on them not disappearing because then when we finally
 *  move the new objects into the repo that would produce a repo that has a broken
 *  object reference. There is nothing that prohibits two parallel commits to the
 *  same branch, and doing that could cause one of the commits to be lost in the
 *  branch history. However, the repo as a whole will always end up valid.
 *
 * NOTHING:
 *  Operations that are purely read-only and can either succeed or
 *  not as a whole do nothing to protect against parallelism. Typical examples
 *  are checkouts or pulls from a remote client. If such an operation is started
 *  nothing protects the repo from removing (by e.g. prune) objects from the repo
 *  that will be necessary to complete the operation. However, such an issue will
 *  be detected by the operation.
 *
 * Given the above the standard approach for locking during prune should be to take
 * an exclusive lock during the entire operation. However, the initial scan of the
 * reachable objects of a repo can take a very long time, and blocking any new
 * commits during this is not a great idea. So, to avoid this the prune operation
 * does two scans of the reachable commits. One with a shared lock and then again
 * with an exclusive lock. The second scan will be faster because it can ignore
 * all the commits we scanned with the shared lock held, meaning we spend less
 * time with an exclusive lock (during which no new commits can be added to the repo).
 *
 * Upgrading the shared lock to an exclusive lock is deadlock prune, as two prune
 * operations could be holding the shared lock and both blocking forever to get the
 * exclusive lock, so we release the lock between the phases. This means there is
 * a small chance that some objects were deleted between the two phases. However, that
 * will only cause the prune operation to over-estimate what objects are reachable, so
 * it can never cause it to delete reachable objects.
 */

static gboolean
ot_dfd_iter_init_allow_noent (int dfd,
                              const char *path,
                              GLnxDirFdIterator *dfd_iter,
                              gboolean *out_exists,
                              GError **error)
{
  glnx_autofd int fd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (fd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "opendirat");
      *out_exists = FALSE;
      return TRUE;
    }
  if (!glnx_dirfd_iterator_init_take_fd (&fd, dfd_iter, error))
    return FALSE;
  *out_exists = TRUE;
  return TRUE;
}

/* Object name helpers */

static guint
_ostree_object_name_hash (gconstpointer a)
{
  VarObjectNameRef ref = var_object_name_from_gvariant ((GVariant *)a);

  return g_str_hash (var_object_name_get_checksum (ref)) + (guint)var_object_name_get_objtype (ref);
}

static gboolean
_ostree_object_name_equal (gconstpointer a,
                           gconstpointer b)
{
  VarObjectNameRef ref_a = var_object_name_from_gvariant ((GVariant *)a);
  VarObjectNameRef ref_b = var_object_name_from_gvariant ((GVariant *)a);

  return
    g_str_equal (var_object_name_get_checksum (ref_a), var_object_name_get_checksum (ref_b)) &&
    var_object_name_get_objtype (ref_a) == var_object_name_get_objtype (ref_b);
}

static GHashTable *
reachable_commits_new (void)
{
  return g_hash_table_new_full (_ostree_object_name_hash, _ostree_object_name_equal,
                                NULL, (GDestroyNotify)g_variant_unref);
}

/* Wrapper to handle flock vs OFD locking based on GLnxLockFile */
static gboolean
do_repo_lock (int fd,
              int flags)
{
  int res;

#ifdef F_OFD_SETLK
  struct flock fl = {
    .l_type = (flags & ~LOCK_NB) == LOCK_EX ? F_WRLCK : F_RDLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0,
  };

  res = TEMP_FAILURE_RETRY (fcntl (fd, (flags & LOCK_NB) ? F_OFD_SETLK : F_OFD_SETLKW, &fl));
#else
  res = -1;
  errno = EINVAL;
#endif

  /* Fallback to flock when OFD locks not available */
  if (res < 0)
    {
      if (errno == EINVAL)
        res = TEMP_FAILURE_RETRY (flock (fd, flags));
      if (res < 0)
        return FALSE;
    }

  return TRUE;
}

static gboolean
get_repo_lock (OstreeRepo          *repo,
               int                  flags,
               int                 *out_lock_fd,
               GCancellable        *cancellable,
               GError             **error)
{
  glnx_autofd int lock_fd = -1;

  /* This re-implements a simpler (non-stacking) version of the ostree repo lock, as
     the API for that is not yet available. When it is (see https://github.com/ostreedev/ostree/pull/2341)
     this should be removed.
     Note: This also doesn't respect the locking config options, it always locks and it always blocks.
  */

  lock_fd = TEMP_FAILURE_RETRY (openat (ostree_repo_get_dfd (repo), ".lock",
                                        O_CREAT | O_RDWR | O_CLOEXEC, 0660));
  if (lock_fd < 0)
    return glnx_throw_errno_prefix (error,
                                    "Opening lock file %s/.lock failed",
                                    flatpak_file_get_path_cached (ostree_repo_get_path (repo)));

  if (!do_repo_lock (lock_fd, flags))
    return glnx_throw_errno_prefix (error, "Locking repo failed (%s)", (flags & LOCK_EX) != 0 ? "exclusive" : "shared");

  *out_lock_fd = g_steal_fd (&lock_fd);
  return TRUE;
}

#define _LOOSE_PATH_MAX (256)

static inline void
get_extra_commitmeta_path (const char *commit,
                           char *path_buf,
                           gsize path_buf_len)
{
  snprintf (path_buf, path_buf_len,
            "objects/%c%c/%s.commitmeta2",
            commit[0], commit[1], commit + 2);
}

static gboolean
load_extra_commitmeta (OstreeRepo       *repo,
                       const char       *commit,
                       GVariant        **out_variant,
                       GCancellable     *cancellable,
                       GError          **error)
{
  char loose_path_buf[_LOOSE_PATH_MAX];
  glnx_autofd int fd = -1;
  g_autoptr(GVariant) ret_variant = NULL;
  g_autoptr(GError) temp_error = NULL;

  get_extra_commitmeta_path (commit, loose_path_buf, sizeof (loose_path_buf));

  if (!glnx_openat_rdonly (ostree_repo_get_dfd (repo), loose_path_buf, FALSE, &fd, &temp_error) &&
      !g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  if (fd != -1)
    {
      g_autoptr(GBytes) content = glnx_fd_readall_bytes (fd, cancellable, error);
      if (!content)
        return FALSE;
      ret_variant = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE ("a{sv}"), content, TRUE));
    }

  *out_variant = g_steal_pointer (&ret_variant);
  return TRUE;
}

static gboolean
save_extra_commitmeta (OstreeRepo       *repo,
                       const char       *commit,
                       GVariant         *variant,
                       GCancellable     *cancellable,
                       GError          **error)
{
  char loose_path_buf[_LOOSE_PATH_MAX];

  get_extra_commitmeta_path (commit, loose_path_buf, sizeof (loose_path_buf));

  if (!glnx_file_replace_contents_at (ostree_repo_get_dfd (repo), loose_path_buf,
                                      g_variant_get_data (variant),
                                      g_variant_get_size (variant),
                                      GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
remove_extra_commitmeta (OstreeRepo       *repo,
                         const char       *commit,
                         GCancellable     *cancellable,
                         GError          **error)
{
  char loose_path_buf[_LOOSE_PATH_MAX];

  get_extra_commitmeta_path (commit, loose_path_buf, sizeof (loose_path_buf));

  /* Ignore errors */
  (void) unlinkat (ostree_repo_get_dfd (repo), loose_path_buf, 0);

  return TRUE;
}



/* Traverse parent commits starting at commit_checksum, and
 * up to maxdepth parents (-1 for unlimited).
 *
 * This doesn't do any locking, so need something else to have an exclusive lock
 * on the repo to avoid races with other processes modifying the repo.
 */
static gboolean
traverse_commit_parents_unlocked (OstreeRepo      *repo,
                                  const char      *commit_checksum,
                                  int              maxdepth,
                                  GHashTable      *inout_checksums,
                                  GCancellable    *cancellable,
                                  GError         **error)
{
  g_autofree char *tmp_checksum = NULL;

  while (TRUE)
    {
      g_autoptr(GVariant) commit = NULL;

      if (!ostree_repo_load_variant_if_exists (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                               commit_checksum, &commit,
                                               error))
        return FALSE;

      /* Just return if the parent isn't found; we do expect most
       * people to have partial repositories.
       */
      if (commit == NULL)
        break;

      g_hash_table_add (inout_checksums, g_strdup (commit_checksum));

      gboolean recurse = FALSE;
      if (maxdepth == -1 || maxdepth > 0)
        {
          g_free (tmp_checksum);
          tmp_checksum = ostree_commit_get_parent (commit);
          if (tmp_checksum)
            {
              commit_checksum = tmp_checksum;
              if (maxdepth > 0)
                maxdepth -= 1;
              recurse = TRUE;
            }
        }
      if (!recurse)
        break;
    }

  return TRUE;
}

/* We need to keep track of possibly a lot of object names (flathub has > 16 million objects atm),
 * so the list of reachable objectnames need to be very compact. To handle this we use a fixed
 * size array to reference the object names. The first 32 bytes is the checksum in raw form and
 * the final byte is the object type.
 */

#define FLATPAK_OSTREE_OBJECT_NAME_LEN (32 + 1)
typedef guint8 FlatpakOstreeObjectName[FLATPAK_OSTREE_OBJECT_NAME_LEN];

#define FLATPAK_OSTREE_OBJECT_NAME_ELEMENT_TYPE "(yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy)" /* 32 + 1 bytes, is a fixed type */

static void
flatpak_ostree_object_name_serialize (FlatpakOstreeObjectName *name,
                                      const char *checksum,
                                      OstreeObjectType objtype)
{
  ostree_checksum_inplace_to_bytes (checksum, &(*name)[0]);
  g_assert (objtype < 255);
  (*name)[32] = (guint8) objtype;
}

static gint
flatpak_ostree_name_compare (const FlatpakOstreeObjectName *name_a,
                             const FlatpakOstreeObjectName *name_b)
{
  return memcmp (name_a, name_b, sizeof (FlatpakOstreeObjectName));
}

static guint
flatpak_ostree_object_name_hash (gconstpointer a)
{
  const FlatpakOstreeObjectName *name = a;
  const guint8 *data = &(*name)[0];

  /* The checksum is essentially all random, so any 4 bytes of it should
     be a good hash value. However, we avoid using the first ones, because
     those are the ones that will be first compared on a hash collision,
     so if they were always the same that would waste 4 comparisons. */
  return
    ((guint32) data[32]) |
    ((guint32) data[31]) << 8 |
    ((guint32) data[30]) << 16 |
    ((guint32) data[29]) << 24;
}

static gboolean
flatpak_ostree_object_name_equal (gconstpointer a,
                                  gconstpointer b)
{
  const FlatpakOstreeObjectName *name_a = a;
  const FlatpakOstreeObjectName *name_b = b;

  return flatpak_ostree_name_compare (name_a, name_b) == 0;
}

/* This is a container for allocating FlatpakOstreeObjectNames in chunks without relocations so
 * that the resulting pointers are stable and can be stored in e.g. a hashtable.
 * Storing the names in chunks like this means we avoid fragmentation and overhead related to
 * each individual name which is important as we can have millions of object names in a repo.
 */

#define BAG_CHUNK_SIZE 1985 /* nr of objects per chunk in bag, makes chunk fit in 64k with some spare for overhead */
typedef struct {
  FlatpakOstreeObjectName *current_chunk; /* Null if non started */
  gsize current_chunk_used; /* number of used objects in current chunk */
  GSList *chunks; /* List of allocated chunks */
  GHashTable *hash; /* (element-type FlatpakOstreeObjectName) */
} FlatpakOstreeObjectNameBag;

static FlatpakOstreeObjectNameBag *
object_name_bag_new (void)
{
  FlatpakOstreeObjectNameBag *bag = g_new0 (FlatpakOstreeObjectNameBag, 1);

  bag->hash = g_hash_table_new_full (flatpak_ostree_object_name_hash, flatpak_ostree_object_name_equal,
                                     NULL, NULL);

  return bag;
}

static void
object_name_bag_free (FlatpakOstreeObjectNameBag *bag)
{
  g_hash_table_unref (bag->hash);
  g_slist_free_full (bag->chunks, g_free);
  g_free (bag);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOstreeObjectNameBag, object_name_bag_free)

static gboolean
object_name_bag_contains (FlatpakOstreeObjectNameBag *bag,
                          const FlatpakOstreeObjectName *name)
{
  return g_hash_table_contains (bag->hash, name);
}

static void
object_name_bag_insert (FlatpakOstreeObjectNameBag *bag,
                        const FlatpakOstreeObjectName *name)
{
  FlatpakOstreeObjectName *res;

  if (g_hash_table_contains (bag->hash, name))
    return;

  if (bag->current_chunk == NULL)
    {
      bag->current_chunk = g_new (FlatpakOstreeObjectName, BAG_CHUNK_SIZE);
      bag->current_chunk_used = 0;
      bag->chunks = g_slist_prepend (bag->chunks, bag->current_chunk);
    }

  res = &bag->current_chunk[bag->current_chunk_used++];
  memcpy (res, name, sizeof (FlatpakOstreeObjectName));

  if (bag->current_chunk_used == BAG_CHUNK_SIZE)
    bag->current_chunk = NULL; /* Need new chunk */

  g_hash_table_add (bag->hash, res);
}

/* Find all reachable commit objects starting from any ref in the repo
 * optionally limiting the number of parent commits.
 *
 * This doesn't do any locking, so need something else to have an exclusive lock
 * on the repo to avoid races with other processes modifying the repo.
 */
static gboolean
traverse_reachable_refs_unlocked (OstreeRepo                  *repo,
                                  guint                        depth,
                                  FlatpakOstreeObjectNameBag  *reachable,
                                  GCancellable                *cancellable,
                                  GError                     **error)
{
  g_autoptr(GHashTable) all_refs = NULL;  /* (element-type utf8 utf8) */
  g_autoptr(GHashTable) all_collection_refs = NULL;  /* (element-type OstreeChecksumRef utf8) */
  g_autoptr(GHashTable) checksums = NULL;  /* (element-type const char *) */

  checksums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Get all commits up to depth from the regular refs */
  if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                              cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH_V (all_refs, const char*, checksum)
    {
      if (!traverse_commit_parents_unlocked (repo, checksum, depth, checksums, cancellable, error))
        return FALSE;
    }

  /* Get all commits up to depth from the collection refs */
  if (!ostree_repo_list_collection_refs (repo, NULL, &all_collection_refs,
                                         OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES, cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH_V (all_collection_refs, const char*, checksum)
    {
      if (!traverse_commit_parents_unlocked (repo, checksum, depth, checksums, cancellable, error))
        return FALSE;
    }

  /* Find reachable objects from each commit checksum */
  GLNX_HASH_TABLE_FOREACH_V (checksums, const char*, checksum)
    {
      g_autoptr(GVariant) extra_commitmeta = NULL;
      g_autoptr(GVariant) commit_reachable = NULL;
      FlatpakOstreeObjectName commit_name;

      /* Early bail-out if we already scanned this commit in the first phase (or via some other branch) */
      flatpak_ostree_object_name_serialize (&commit_name, checksum, OSTREE_OBJECT_TYPE_COMMIT);
      if (object_name_bag_contains (reachable, &commit_name))
        continue;

      g_debug ("Finding objects to keep for commit %s", checksum);

      if (!load_extra_commitmeta (repo, checksum, &extra_commitmeta, cancellable, error))
        return FALSE;

      if (extra_commitmeta)
        commit_reachable = g_variant_lookup_value (extra_commitmeta, "xa.reachable", G_VARIANT_TYPE ("a" FLATPAK_OSTREE_OBJECT_NAME_ELEMENT_TYPE));

      if (commit_reachable == NULL)
        {
          g_autoptr(GHashTable) commit_reachable_ht = reachable_commits_new ();
          g_autoptr(GVariant) new_extra_commitmeta = NULL;
          g_autofree FlatpakOstreeObjectName *commit_reachable_raw = NULL;
          FlatpakOstreeObjectName *next_commit_reachable_raw;
          g_auto(GVariantDict) extra_commitmeta_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
          OstreeRepoCommitState commitstate = 0;
          g_autoptr(GError) local_error = NULL;

          if (!ostree_repo_load_commit (repo, checksum, NULL, &commitstate, &local_error) &&
              !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          if (!ostree_repo_traverse_commit_union (repo, checksum, 0, commit_reachable_ht,
                                                  cancellable, error))
            return FALSE;

          commit_reachable_raw = g_new (FlatpakOstreeObjectName, g_hash_table_size (commit_reachable_ht));

          next_commit_reachable_raw = &commit_reachable_raw[0];
          GLNX_HASH_TABLE_FOREACH_V (commit_reachable_ht, GVariant *, reachable_commit)
            {
              VarObjectNameRef ref = var_object_name_from_gvariant ((GVariant *)reachable_commit);

              flatpak_ostree_object_name_serialize (next_commit_reachable_raw,
                                                    var_object_name_get_checksum (ref),
                                                    var_object_name_get_objtype (ref));
              next_commit_reachable_raw++;
            }

          commit_reachable = g_variant_ref_sink (g_variant_new_fixed_array (G_VARIANT_TYPE (FLATPAK_OSTREE_OBJECT_NAME_ELEMENT_TYPE),
                                                                            commit_reachable_raw,
                                                                            g_hash_table_size (commit_reachable_ht),
                                                                            sizeof(FlatpakOstreeObjectName)));

          /* Don't save the reachable set for later reuse if the commit is partial, as it may not be complete */
          if ((commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL) == 0)
            {
              g_variant_dict_init (&extra_commitmeta_builder, extra_commitmeta);
              g_variant_dict_insert_value (&extra_commitmeta_builder, "xa.reachable", commit_reachable);

              new_extra_commitmeta = g_variant_ref_sink (g_variant_dict_end (&extra_commitmeta_builder));
              if (!save_extra_commitmeta (repo, checksum, new_extra_commitmeta, cancellable, error))
                return FALSE;
            }
        }

      {
        gsize n_reachable, i;
        const FlatpakOstreeObjectName *reachable_objects =
          g_variant_get_fixed_array (commit_reachable, &n_reachable,
                                     sizeof(FlatpakOstreeObjectName));

        for (i = 0; i < n_reachable; i++)
          object_name_bag_insert (reachable, &reachable_objects[i]);
      }
    }

  return TRUE;
}

typedef struct {
  OstreeRepo *repo;
  FlatpakOstreeObjectNameBag *reachable;
  gboolean dont_prune;
  guint n_reachable;
  guint n_unreachable;
  guint64 freed_bytes;
} OtPruneData;

static gboolean
prune_loose_object (OtPruneData          *data,
                    const char           *checksum,
                    OstreeObjectType      objtype,
                    GCancellable         *cancellable,
                    GError              **error)
{
  guint64 storage_size = 0;

  g_debug ("Pruning unneeded object %s.%s", checksum,
           ostree_object_type_to_string (objtype));

  if (!ostree_repo_query_object_storage_size (data->repo, objtype, checksum,
                                              &storage_size, cancellable, error))
    return FALSE;

  data->freed_bytes += storage_size;
  data->n_unreachable++;

  if (!data->dont_prune)
    {
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          if (!remove_extra_commitmeta (data->repo, checksum, cancellable, error))
            return FALSE;

          if (!ostree_repo_mark_commit_partial (data->repo, checksum, FALSE, error))
            return FALSE;
        }

      if (!ostree_repo_delete_object (data->repo, objtype, checksum,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
prune_unreachable_loose_objects_at (OstreeRepo             *self,
                                    OtPruneData            *data,
                                    int                     dfd,
                                    const char             *prefix,
                                    GCancellable           *cancellable,
                                    GError                **error)
{

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (dfd, prefix, &dfd_iter, &exists, error))
    return FALSE;
  /* Note early return */
  if (!exists)
    return TRUE;

  while (TRUE)
    {
      struct dirent *dent;
      FlatpakOstreeObjectName key;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      const char *name = dent->d_name;
      if (strcmp (name, ".") == 0 ||
          strcmp (name, "..") == 0)
        continue;

      const char *dot = strrchr (name, '.');
      if (!dot)
        continue;

      OstreeObjectType objtype;

      if (strcmp (dot, ".filez") == 0)
        objtype = OSTREE_OBJECT_TYPE_FILE;
      else if (strcmp (dot, ".dirtree") == 0)
        objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
      else if (strcmp (dot, ".dirmeta") == 0)
        objtype = OSTREE_OBJECT_TYPE_DIR_META;
      else if (strcmp (dot, ".commit") == 0)
        objtype = OSTREE_OBJECT_TYPE_COMMIT;
      else /* No need to handle payload links, they don't happen in archive repos and we call the ostree prune for all other repos */
        continue;

      if ((dot - name) != 62)
        continue;

      char buf[OSTREE_SHA256_STRING_LEN+1];

      memcpy (buf, prefix+8, 2);
      memcpy (buf + 2, name, 62);
      buf[sizeof(buf)-1] = '\0';

      flatpak_ostree_object_name_serialize (&key, buf, objtype);
      if (object_name_bag_contains (data->reachable, &key))
        {
          data->n_reachable++;
          continue;
        }

      if (!prune_loose_object (data, buf, objtype, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
prune_unreachable_loose_objects (OstreeRepo                  *self,
                                 OtPruneData                 *data,
                                 GCancellable                *cancellable,
                                 GError                     **error)
{
 static const gchar hexchars[] = "0123456789abcdef";
 int dfd = ostree_repo_get_dfd (self);

 g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

 for (guint c = 0; c < 256; c++)
   {
     char buf[] = "objects/XX";
     buf[8] = hexchars[c >> 4];
     buf[9] = hexchars[c & 0xF];

     if (!prune_unreachable_loose_objects_at (self, data, dfd, buf, cancellable, error))
       return FALSE;
   }

 return TRUE;
}

gboolean
flatpak_repo_prune (OstreeRepo    *repo,
                    int            depth,
                    gboolean       dry_run,
                    int           *out_objects_total,
                    int           *out_objects_pruned,
                    guint64       *out_pruned_object_size_total,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autoptr(FlatpakOstreeObjectNameBag) reachable = object_name_bag_new ();
  OtPruneData data = { 0, };
  g_autoptr(GTimer) timer = NULL;

  /* This version only handles archive repos, if called for something else call ostree */
  if (ostree_repo_get_mode (repo) != OSTREE_REPO_MODE_ARCHIVE)
    {
      OstreeRepoPruneFlags flags = OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY;
      if (dry_run)
        flags |= OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE;

      return ostree_repo_prune (repo, flags, depth,
                                out_objects_total, out_objects_pruned, out_pruned_object_size_total,
                                cancellable, error);
    }

  {
    /* shared lock in this region, see locking strategy above */
    glnx_autofd int lock_fd = -1;

    if (!get_repo_lock (repo, LOCK_SH, &lock_fd, cancellable, error))
      return FALSE;

    timer = g_timer_new ();
    g_info ("Finding reachable objects, unlocked (depth=%d)", depth);
    g_timer_start (timer);

    if (!traverse_reachable_refs_unlocked (repo, depth, reachable, cancellable, error))
      return FALSE;

    g_timer_stop (timer);
    g_info ("Elapsed time: %.1f sec",  g_timer_elapsed (timer, NULL));
    g_clear_pointer (&timer, g_timer_destroy);
  }

  if (!dry_run)
    {
      /* exclusive lock in this region, see locking strategy above */
      glnx_autofd int lock_fd = -1;

      if (!get_repo_lock (repo, LOCK_EX, &lock_fd, cancellable, error))
        return FALSE;

      timer = g_timer_new ();
      g_info ("Finding reachable objects, locked (depth=%d)", depth);
      g_timer_start (timer);

      if (!traverse_reachable_refs_unlocked (repo, depth, reachable, cancellable, error))
        return FALSE;

      data.repo = repo;
      data.reachable = reachable;
      data.dont_prune = dry_run;

      g_timer_stop (timer);
      g_info ("Elapsed time: %.1f sec",  g_timer_elapsed (timer, NULL));

      {
        g_info ("Pruning unreachable objects");
        g_timer_start (timer);

        if (!prune_unreachable_loose_objects (repo, &data, cancellable, error))
          return FALSE;

        g_timer_stop (timer);
        g_info ("Elapsed time: %.1f sec",  g_timer_elapsed (timer, NULL));
      }
    }

  /* Prune static deltas outside lock to avoid conflict with its exclusive lock */
  if (!dry_run)
    {
      g_info ("Pruning static deltas");
      g_timer_start (timer);

      if (!ostree_repo_prune_static_deltas (repo, NULL, cancellable, error))
        return FALSE;

      g_timer_stop (timer);
      g_info ("Elapsed time: %.1f sec",  g_timer_elapsed (timer, NULL));
    }

  *out_objects_total = data.n_reachable + data.n_unreachable;
  *out_objects_pruned = data.n_unreachable;
  *out_pruned_object_size_total = data.freed_bytes;
  return TRUE;
}

