/*
 * Copyright © 2018 Red Hat, Inc
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
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-error.h"
#include "flatpak-cli-transaction.h"

static GOptionEntry options[] = {
  { NULL }
};

typedef enum {
  FSCK_STATUS_OK,
  FSCK_STATUS_HAS_MISSING_OBJECTS,
  FSCK_STATUS_HAS_INVALID_OBJECTS,
} FsckStatus;

static FsckStatus
fsck_one_object (OstreeRepo      *repo,
                 const char      *checksum,
                 OstreeObjectType objtype,
                 gboolean         allow_missing)
{
  g_autoptr(GError) local_error = NULL;

  if (!ostree_repo_fsck_object (repo, objtype, checksum, NULL, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          if (!allow_missing)
            g_printerr (_("Object missing: %s.%s\n"), checksum,
                        ostree_object_type_to_string (objtype));
          return FSCK_STATUS_HAS_MISSING_OBJECTS;
        }
      else
        {
          g_printerr (_("%s, deleting object\n"), local_error->message);
          (void) ostree_repo_delete_object (repo, objtype, checksum, NULL, NULL);
          return FSCK_STATUS_HAS_INVALID_OBJECTS;
        }
    }

  return FSCK_STATUS_OK;
}

/* This is used for leaf object types */
static FsckStatus
fsck_leaf_object (OstreeRepo      *repo,
                  const char      *checksum,
                  OstreeObjectType objtype,
                  GHashTable      *object_status_cache)
{
  g_autoptr(GVariant) key = NULL;
  gpointer cached_status;
  FsckStatus status = 0;

  key = g_variant_ref_sink (ostree_object_name_serialize (checksum, objtype));

  if (g_hash_table_lookup_extended (object_status_cache, key, NULL, &cached_status))
    {
      status = GPOINTER_TO_INT (cached_status);
    }
  else
    {
      status = fsck_one_object (repo, checksum, objtype, FALSE);
      g_hash_table_insert (object_status_cache, g_steal_pointer (&key), GINT_TO_POINTER (status));
    }

  return status;
}


static FsckStatus
fsck_dirtree (OstreeRepo *repo,
              gboolean    partial,
              const char *checksum,
              GHashTable *object_status_cache)
{
  OstreeRepoCommitIterResult iterres;

  g_autoptr(GError) local_error = NULL;
  FsckStatus status = 0;
  g_autoptr(GVariant) key = NULL;
  g_autoptr(GVariant) dirtree = NULL;
  gpointer cached_status;
  ostree_cleanup_repo_commit_traverse_iter
  OstreeRepoCommitTraverseIter iter = { 0, };

  key = g_variant_ref_sink (ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_DIR_TREE));
  if (g_hash_table_lookup_extended (object_status_cache, key, NULL, &cached_status))
    return GPOINTER_TO_INT (cached_status);

  /* First verify the dirtree itself */
  status = fsck_one_object (repo, checksum, OSTREE_OBJECT_TYPE_DIR_TREE, partial);

  if (status == FSCK_STATUS_OK)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                     &dirtree, &local_error) ||
          !ostree_repo_commit_traverse_iter_init_dirtree (&iter, repo, dirtree, 0, &local_error))
        {
          g_printerr (_("Can't load object %s: %s\n"), checksum, local_error->message);
          g_clear_error (&local_error);
          status = MAX (status, FSCK_STATUS_HAS_INVALID_OBJECTS);
        }
      else
        {
          /* Then its children, recursively */
          while (TRUE)
            {
              iterres = ostree_repo_commit_traverse_iter_next (&iter, NULL, &local_error);
              if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_ERROR)
                {
                  /* Some internal error in the dir-object */
                  g_print ("%s\n", local_error->message);
                  g_clear_error (&local_error);
                  status = MAX (status, FSCK_STATUS_HAS_INVALID_OBJECTS);
                  break;
                }
              else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_END)
                break;
              else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_FILE)
                {
                  char *name;
                  char *checksum;
                  FsckStatus file_status;

                  ostree_repo_commit_traverse_iter_get_file (&iter, &name, &checksum);
                  file_status = fsck_leaf_object (repo, checksum, OSTREE_OBJECT_TYPE_FILE, object_status_cache);
                  status = MAX (status, file_status);
                }
              else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_DIR)
                {
                  char *name;
                  char *meta_checksum;
                  char *dirtree_checksum;
                  FsckStatus meta_status;
                  FsckStatus dirtree_status;

                  ostree_repo_commit_traverse_iter_get_dir (&iter, &name, &dirtree_checksum, &meta_checksum);

                  meta_status = fsck_leaf_object (repo, meta_checksum, OSTREE_OBJECT_TYPE_DIR_META, object_status_cache);
                  status = MAX (status, meta_status);

                  dirtree_status = fsck_dirtree (repo, partial, dirtree_checksum, object_status_cache);

                  status = MAX (status, dirtree_status);
                }
              else
                g_assert_not_reached ();
            }
        }
    }

  g_hash_table_insert (object_status_cache, g_steal_pointer (&key), GINT_TO_POINTER (status));
  return status;
}

static FsckStatus
fsck_commit (OstreeRepo *repo,
             const char *checksum,
             GHashTable *object_status_cache)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) dirtree_csum_bytes = NULL;
  g_autofree char *dirtree_checksum = NULL;
  g_autoptr(GVariant) meta_csum_bytes = NULL;
  g_autofree char *meta_checksum = NULL;
  OstreeRepoCommitState commitstate = 0;
  FsckStatus status, dirtree_status, meta_status;
  gboolean partial;

  status = fsck_one_object (repo, checksum, OSTREE_OBJECT_TYPE_COMMIT, FALSE);
  if (status != FSCK_STATUS_OK)
    return status;

  if (!ostree_repo_load_commit (repo, checksum, &commit, &commitstate, &local_error))
    {
      g_print ("%s\n", local_error->message);
      g_clear_error (&local_error);
      return FSCK_STATUS_HAS_INVALID_OBJECTS;
    }

  partial = (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL) != 0;

  g_variant_get_child (commit, 7, "@ay", &meta_csum_bytes);
  meta_checksum = ostree_checksum_from_bytes (ostree_checksum_bytes_peek (meta_csum_bytes));

  meta_status = fsck_leaf_object (repo, meta_checksum, OSTREE_OBJECT_TYPE_DIR_META, object_status_cache);
  status = MAX (status, meta_status);

  g_variant_get_child (commit, 6, "@ay", &dirtree_csum_bytes);
  dirtree_checksum = ostree_checksum_from_bytes (ostree_checksum_bytes_peek (dirtree_csum_bytes));

  dirtree_status = fsck_dirtree (repo, partial, dirtree_checksum, object_status_cache);
  status = MAX (status, dirtree_status);

  /* Its ok for partial commits to have missing objects */
  if (status == FSCK_STATUS_HAS_MISSING_OBJECTS && partial)
    status = FSCK_STATUS_OK;

  return status;
}

static void
transaction_add_local_ref (FlatpakDir         *dir,
                           FlatpakTransaction *transaction,
                           const char         *ref)
{
  g_autoptr(GVariant) deploy_data = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *repo_checksum = NULL;
  const char *origin;
  const char **subpaths;

  deploy_data = flatpak_dir_get_deploy_data (dir, ref, NULL, &local_error);
  if (deploy_data == NULL)
    {
      if (!g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
        g_printerr (_("Problems loading data for %s: %s\n"), ref, local_error->message);
      g_clear_error (&local_error);
      return;
    }

  origin = flatpak_deploy_data_get_origin (deploy_data);
  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);

  repo_checksum = flatpak_dir_read_latest (dir, origin, ref, NULL, NULL, NULL);
  if (repo_checksum == NULL)
    {
      if (!flatpak_transaction_add_install (transaction, origin, ref, subpaths, &local_error))
        {
          g_printerr (_("Error reinstalling %s: %s\n"), ref, local_error->message);
          g_clear_error (&local_error);
        }
    }
}


gboolean
flatpak_builtin_repair (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir = NULL;
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) invalid_refs = NULL;
  g_autoptr(GHashTable) object_status_cache = NULL;
  g_auto(GStrv) app_refs = NULL;
  g_auto(GStrv) runtime_refs = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  OstreeRepo *repo;
  int i;

  context = g_option_context_new (_("- Repair a flatpak installation"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR, &dirs, cancellable, error))
    return FALSE;


  dir = g_ptr_array_index (dirs, 0);

  if (!flatpak_dir_ensure_repo (dir, cancellable, error))
    return FALSE;

  repo = flatpak_dir_get_repo (dir);

  /*
   * Try to repair a flatpak directory:
   *  + Scan all locally available refs
   *  + remove ref that don't correspond to a deployed ref
   *  + Verify the commits they point to and all object they reference:
   *  +  Remove any invalid objects
   *  +  Note any missing objects
   *  + Any refs that had invalid object, or non-partial refs that had missing objects are removed
   *  + prune (depth=0) all object not references by a ref, which gets rid of any possibly invalid non-scanned objects
   *  + Enumerate all deployed refs:
   *  +   if they are not in the repo (or is partial for a non-subdir deploy), re-install them (pull + deploy)
   */

  object_status_cache = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                               (GDestroyNotify) g_variant_unref, NULL);

  invalid_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Validate that the commit for each ref is available */
  if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                              cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH_KV (all_refs, const char *, refspec, const char *, checksum)
  {
    g_autofree char *remote = NULL;
    g_autofree char *ref_name = NULL;
    FsckStatus status;

    if (!ostree_parse_refspec (refspec, &remote, &ref_name, error))
      return FALSE;

    /* Does this look like a regular ref? */
    if (g_str_has_prefix (ref_name, "app/") || g_str_has_prefix (ref_name, "runtime/"))
      {
        g_autofree char *origin = flatpak_dir_get_origin (dir, ref_name, cancellable, NULL);

        /* If so, is it deployed, and from this remote? */
        if (remote == NULL || g_strcmp0 (origin, remote) != 0)
          {
            g_print (_("Removing non-deployed ref %s...\n"), refspec);
            (void) ostree_repo_set_ref_immediate (repo, remote, ref_name, NULL, cancellable, NULL);
            continue;
          }
      }

    g_print (_("Verifying %s...\n"), refspec);

    status = fsck_commit (repo, checksum, object_status_cache);
    if (status != FSCK_STATUS_OK)
      {
        g_printerr (_("Deleting ref %s due to missing objects\n"), refspec);
        (void) ostree_repo_set_ref_immediate (repo, remote, ref_name, NULL, cancellable, NULL);
      }
  }

  g_print (_("Pruning objects\n"));

  if (!flatpak_dir_prune (dir, cancellable, error))
    return FALSE;

  if (!flatpak_dir_list_refs (dir, "app", &app_refs, cancellable, NULL))
    return FALSE;

  if (!flatpak_dir_list_refs (dir, "runtime", &runtime_refs, cancellable, NULL))
    return FALSE;

  transaction = flatpak_cli_transaction_new (dir, TRUE, FALSE, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_disable_dependencies (transaction, TRUE);
  flatpak_transaction_set_disable_related (transaction, TRUE);
  flatpak_transaction_set_reinstall (transaction, TRUE);

  for (i = 0; app_refs[i] != NULL; i++)
    {
      const char *ref = app_refs[i];

      transaction_add_local_ref (dir, transaction, ref);
    }

  for (i = 0; runtime_refs[i] != NULL; i++)
    {
      const char *ref = runtime_refs[i];

      transaction_add_local_ref (dir, transaction, ref);
    }

  if (!flatpak_transaction_is_empty (transaction))
    {
      g_print (_("Reinstalling removed refs\n"));
      if (!flatpak_cli_transaction_run (transaction, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_repair (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      break;
    }

  return TRUE;
}
