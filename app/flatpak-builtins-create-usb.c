/*
 * Copyright © 2018 Matthew Leeds
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
 *       Matthew Leeds <matthew.leeds@endlessm.com>
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
#include "flatpak-error.h"

static char *opt_arch;
static char *opt_destination_repo;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_allow_partial;

static GOptionEntry options[] = {
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to copy"), N_("ARCH") },
  { "destination-repo", 0, 0, G_OPTION_ARG_FILENAME, &opt_destination_repo, "Use custom repository directory within the mount", N_("DEST") },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "allow-partial", 0, 0, G_OPTION_ARG_NONE, &opt_allow_partial, N_("Allow partial commits in the created repo"), NULL },
  { NULL }
};

typedef struct CommitAndSubpaths
{
  gchar  *commit;
  gchar **subpaths;
} CommitAndSubpaths;

static void
commit_and_subpaths_free (CommitAndSubpaths *c_s)
{
  g_free (c_s->commit);
  g_strfreev (c_s->subpaths);
  g_free (c_s);
}

static CommitAndSubpaths *
commit_and_subpaths_new (const char *commit, const char * const *subpaths)
{
  CommitAndSubpaths *c_s = g_new (CommitAndSubpaths, 1);

  c_s->commit = g_strdup (commit);
  c_s->subpaths = g_strdupv ((char **) subpaths);
  return c_s;
}

static char **
get_flatpak_subpaths_from_deploy_subpaths (const char * const *subpaths)
{
  g_autoptr(GPtrArray) resolved_subpaths = NULL;
  gsize i;

  if (subpaths == NULL || subpaths[0] == NULL)
    return NULL;

  resolved_subpaths = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (resolved_subpaths, g_strdup ("/metadata"));
  for (i = 0; subpaths[i] != NULL; i++)
    g_ptr_array_add (resolved_subpaths, g_build_filename ("/files", subpaths[i], NULL));
  g_ptr_array_add (resolved_subpaths, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&resolved_subpaths), FALSE);
}

/* Add related refs specified in the metadata of @ref to @all_refs, also
 * updating @all_collection_ids with any new collection IDs. A warning will be
 * printed for related refs that are not installed, and they won't be added to
 * the list. */
static gboolean
add_related (GHashTable        *all_refs,
             GHashTable        *all_collection_ids,
             FlatpakDecomposed *ref,
             FlatpakDir        *dir,
             GCancellable      *cancellable,
             GError           **error)
{
  g_autoptr(GBytes) deploy_data = NULL;
  g_autoptr(FlatpakDeploy) deploy = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  const char *commit = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  GList *extensions, *l;

  g_debug ("Finding related refs for ‘%s’", flatpak_decomposed_get_ref (ref));

  arch = flatpak_decomposed_dup_arch (ref);
  branch = flatpak_decomposed_dup_branch (ref);

  deploy_data = flatpak_dir_get_deploy_data (dir, flatpak_decomposed_get_ref (ref), FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
  if (deploy_data == NULL)
    return FALSE;

  if (flatpak_deploy_data_has_subpaths (deploy_data) && !opt_allow_partial)
    g_printerr (_("Warning: Related ref ‘%s’ is partially installed. Use --allow-partial to suppress this message.\n"),
                flatpak_decomposed_get_ref (ref));

  commit = flatpak_deploy_data_get_commit (deploy_data);

  deploy = flatpak_dir_load_deployed (dir, ref, commit, cancellable, error);
  if (deploy == NULL)
    return FALSE;

  metakey = flatpak_deploy_get_metadata (deploy);

  extensions = flatpak_list_extensions (metakey, arch, branch);
  for (l = extensions; l; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autoptr(GBytes) ext_deploy_data = NULL;
      g_autoptr(OstreeCollectionRef) ext_collection_ref = NULL;
      g_autofree char *ext_collection_id = NULL;
      g_autofree const char **ext_subpaths = NULL;
      g_auto(GStrv) resolved_ext_subpaths = NULL;
      const char *ext_remote;
      const char *ext_commit = NULL;
      CommitAndSubpaths *c_s;

      if (ext->is_unmaintained)
        continue;

      g_assert (ext->ref);

      ext_deploy_data = flatpak_dir_get_deploy_data (dir, ext->ref, FLATPAK_DEPLOY_VERSION_ANY, cancellable, NULL);
      if (ext_deploy_data == NULL)
        {
          g_printerr (_("Warning: Omitting related ref ‘%s’ because it is not installed.\n"),
                      ext->ref);
          continue;
        }

      if (flatpak_deploy_data_has_subpaths (ext_deploy_data) && !opt_allow_partial)
        {
          g_printerr (_("Warning: Related ref ‘%s’ is partially installed. Use --allow-partial to suppress this message.\n"),
                      ext->ref);
        }

      ext_remote = flatpak_deploy_data_get_origin (ext_deploy_data);
      if (ext_remote == NULL)
        return FALSE;
      ext_collection_id = flatpak_dir_get_remote_collection_id (dir, ext_remote);
      if (ext_collection_id == NULL)
        {
          g_printerr (_("Warning: Omitting related ref ‘%s’ because its remote ‘%s’ does not have a collection ID set.\n"),
                      ext->ref, ext_remote);
          continue;
        }

      ext_commit = flatpak_deploy_data_get_commit (ext_deploy_data);
      ext_subpaths = flatpak_deploy_data_get_subpaths (ext_deploy_data);
      resolved_ext_subpaths = get_flatpak_subpaths_from_deploy_subpaths (ext_subpaths);
      c_s = commit_and_subpaths_new (ext_commit, (const char * const *) resolved_ext_subpaths);

      g_hash_table_insert (all_collection_ids, g_strdup (ext_collection_id), g_strdup (ext_remote));
      ext_collection_ref = ostree_collection_ref_new (ext_collection_id, ext->ref);
      g_hash_table_insert (all_refs, g_steal_pointer (&ext_collection_ref), c_s);
    }

  g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);

  return TRUE;
}

/* Add the runtime and its related refs to @all_refs, also updating
 * @all_collection_ids with any new collection IDs */
static gboolean
add_runtime (GHashTable        *all_refs,
             GHashTable        *all_collection_ids,
             FlatpakDecomposed *ref,
             FlatpakDir        *dir,
             GCancellable      *cancellable,
             GError           **error)
{
  g_autoptr(GBytes) deploy_data = NULL;
  g_autoptr(GBytes) runtime_deploy_data = NULL;
  g_autoptr(FlatpakDeploy) deploy = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(OstreeCollectionRef) runtime_collection_ref = NULL;
  g_autofree char *runtime_pref = NULL;
  g_autoptr(FlatpakDecomposed) runtime_ref = NULL;
  g_autofree char *runtime_remote = NULL;
  g_autofree char *runtime_collection_id = NULL;
  g_autofree const char **runtime_subpaths = NULL;
  g_auto(GStrv) resolved_runtime_subpaths = NULL;
  const char *commit = NULL;
  const char *runtime_commit = NULL;
  CommitAndSubpaths *c_s;


  g_debug ("Finding the runtime for ‘%s’", flatpak_decomposed_get_ref (ref));

  deploy_data = flatpak_dir_get_deploy_data (dir, flatpak_decomposed_get_ref (ref), FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
  if (deploy_data == NULL)
    return FALSE;

  commit = flatpak_deploy_data_get_commit (deploy_data);

  deploy = flatpak_dir_load_deployed (dir, ref, commit, cancellable, error);
  if (deploy == NULL)
    return FALSE;

  metakey = flatpak_deploy_get_metadata (deploy);

  runtime_pref = g_key_file_get_string (metakey, "Application", "runtime", error);
  if (runtime_pref == NULL)
    return FALSE;
  runtime_ref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, runtime_pref, error);
  if (runtime_ref == NULL)
    return FALSE;

  runtime_deploy_data = flatpak_dir_get_deploy_data (dir, flatpak_decomposed_get_ref (runtime_ref), FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
  if (runtime_deploy_data == NULL)
    return FALSE;
  runtime_remote = flatpak_dir_get_origin (dir, flatpak_decomposed_get_ref (runtime_ref), cancellable, error);
  if (runtime_remote == NULL)
    return FALSE;
  runtime_collection_id = flatpak_dir_get_remote_collection_id (dir, runtime_remote);
  if (runtime_collection_id == NULL)
    return flatpak_fail (error,
                         _("Remote ‘%s’ does not have a collection ID set, which is required for P2P distribution of ‘%s’."),
                         runtime_remote, flatpak_decomposed_get_ref (runtime_ref));

  runtime_commit = flatpak_deploy_data_get_commit (runtime_deploy_data);
  runtime_subpaths = flatpak_deploy_data_get_subpaths (runtime_deploy_data);
  resolved_runtime_subpaths = get_flatpak_subpaths_from_deploy_subpaths (runtime_subpaths);
  c_s = commit_and_subpaths_new (runtime_commit, (const char * const *) resolved_runtime_subpaths);

  g_hash_table_insert (all_collection_ids, g_strdup (runtime_collection_id), g_strdup (runtime_remote));
  runtime_collection_ref = ostree_collection_ref_new (runtime_collection_id, flatpak_decomposed_get_ref (runtime_ref));
  g_hash_table_insert (all_refs, g_steal_pointer (&runtime_collection_ref), c_s);

  if (!add_related (all_refs, all_collection_ids, runtime_ref, dir, cancellable, error))
    return FALSE;

  return TRUE;
}

/* Copied from src/ostree/ot-builtin-create-usb.c in ostree.git, with slight modifications */
static gboolean
ostree_create_usb (GOptionContext *context,
                   OstreeRepo     *src_repo,
                   const char     *mount_root_path,
                   struct stat     mount_root_stbuf,
                   int             mount_root_dfd,
                   GHashTable     *all_refs,
                   GCancellable   *cancellable,
                   GError        **error)
{
  g_autoptr(OstreeAsyncProgressFinish) progress = NULL;
  g_auto(GLnxConsoleRef) console = { 0, };
  guint num_refs = 0;

  /* Open the destination repository on the USB stick or create it if it doesn’t exist.
   * Check it’s below @mount_root_path, and that it’s not the same as the source
   * repository. */
  const char *dest_repo_path = (opt_destination_repo != NULL) ? opt_destination_repo : ".ostree/repo";

  if (!glnx_shutil_mkdir_p_at (mount_root_dfd, dest_repo_path, 0755, cancellable, error))
    return FALSE;

  /* Always use the archive repo mode, which works on FAT file systems that
   * don't support xattrs, compresses files to save space, doesn't store
   * permission info directly in the file attributes, and is at least sometimes
   * more performant than bare-user */
  OstreeRepoMode mode = OSTREE_REPO_MODE_ARCHIVE;

  g_debug ("%s: Creating repository in mode %u", G_STRFUNC, mode);
  g_autoptr(OstreeRepo) dest_repo = ostree_repo_create_at (mount_root_dfd, dest_repo_path,
                                                           mode, NULL, cancellable, error);

  if (dest_repo == NULL)
    return FALSE;

  struct stat dest_repo_stbuf;

  if (!glnx_fstat (ostree_repo_get_dfd (dest_repo), &dest_repo_stbuf, error))
    return FALSE;

  if (dest_repo_stbuf.st_dev != mount_root_stbuf.st_dev)
    return usage_error (context, "--destination-repo must be a descendent of MOUNT-PATH", error);

  if (ostree_repo_equal (src_repo, dest_repo))
    return usage_error (context, "--destination-repo must not be the source repository", error);

  if (!ostree_repo_is_writable (dest_repo, error))
    return glnx_prefix_error (error, "Cannot write to repository");

  /* Copy across all of the collection–refs to the destination repo. We have to
   * do it one ref at a time in order to get the subpaths right. */

  GLNX_HASH_TABLE_FOREACH_KV (all_refs, OstreeCollectionRef *, c_r, CommitAndSubpaths *, c_s)
  {
    GVariantBuilder builder;
    g_autoptr(GVariant) opts = NULL;
    OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_MIRROR;
    GVariantBuilder refs_builder;

    num_refs++;

    g_variant_builder_init (&refs_builder, G_VARIANT_TYPE ("a(sss)"));
    g_variant_builder_add (&refs_builder, "(sss)",
                           c_r->collection_id, c_r->ref_name,
                           c_s->commit ? c_s->commit : "");

    glnx_console_lock (&console);

    if (console.is_tty)
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

    g_variant_builder_add (&builder, "{s@v}", "collection-refs",
                           g_variant_new_variant (g_variant_builder_end (&refs_builder)));
    if (c_s->subpaths != NULL)
      {
        g_variant_builder_add (&builder, "{s@v}", "subdirs",
                               g_variant_new_variant (g_variant_new_strv ((const char * const *) c_s->subpaths, -1)));
      }
    g_variant_builder_add (&builder, "{s@v}", "flags",
                           g_variant_new_variant (g_variant_new_int32 (flags)));
    g_variant_builder_add (&builder, "{s@v}", "depth",
                           g_variant_new_variant (g_variant_new_int32 (0)));
    opts = g_variant_ref_sink (g_variant_builder_end (&builder));

    g_autofree char *src_repo_uri = g_file_get_uri (ostree_repo_get_path (src_repo));

    if (!ostree_repo_pull_with_options (dest_repo, src_repo_uri,
                                        opts,
                                        progress,
                                        cancellable, error))
      {
        ostree_repo_abort_transaction (dest_repo, cancellable, NULL);
        return FALSE;
      }

    glnx_console_unlock (&console);
  }

  /* Ensure a summary file is present to make it easier to look up commit checksums. */
  /* FIXME: It should be possible to work without this, but find_remotes_cb() in
   * ostree-repo-pull.c currently assumes a summary file (signed or unsigned) is
   * present. */
  if (!ostree_repo_regenerate_summary (dest_repo, NULL, cancellable, error))
    return FALSE;

  /* Add the symlinks .ostree/repos.d/@symlink_name → @dest_repo_path, unless
   * the @dest_repo_path is a well-known one like ostree/repo, in which case no
   * symlink is necessary; #OstreeRepoFinderMount always looks there. */
  if (!g_str_equal (dest_repo_path, "ostree/repo") &&
      !g_str_equal (dest_repo_path, ".ostree/repo"))
    {
      if (!glnx_shutil_mkdir_p_at (mount_root_dfd, ".ostree/repos.d", 0755, cancellable, error))
        return FALSE;

      /* Find a unique name for the symlink. If a symlink already targets
       * @dest_repo_path, use that and don’t create a new one. */
      GLnxDirFdIterator repos_iter;
      gboolean need_symlink = TRUE;

      if (!glnx_dirfd_iterator_init_at (mount_root_dfd, ".ostree/repos.d", TRUE, &repos_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *repo_dent;

          if (!glnx_dirfd_iterator_next_dent (&repos_iter, &repo_dent, cancellable, error))
            return FALSE;

          if (repo_dent == NULL)
            break;

          /* Does the symlink already point to this repository? (Or is the
           * repository itself present in repos.d?) We already guarantee that
           * they’re on the same device. */
          if (repo_dent->d_ino == dest_repo_stbuf.st_ino)
            {
              need_symlink = FALSE;
              break;
            }
        }

      /* If we need a symlink, find a unique name for it and create it. */
      if (need_symlink)
        {
          /* Relative to .ostree/repos.d. */
          g_autofree char *relative_dest_repo_path = g_build_filename ("..", "..", dest_repo_path, NULL);
          guint i;
          const guint max_attempts = 100;

          for (i = 0; i < max_attempts; i++)
            {
              g_autofree char *symlink_path = g_strdup_printf (".ostree/repos.d/%02u-generated", i);

              int ret = TEMP_FAILURE_RETRY (symlinkat (relative_dest_repo_path, mount_root_dfd, symlink_path));
              if (ret < 0 && errno != EEXIST)
                return glnx_throw_errno_prefix (error, "symlinkat(%s → %s)", symlink_path, relative_dest_repo_path);
              else if (ret >= 0)
                break;
            }

          if (i == max_attempts)
            return glnx_throw (error, "Could not find an unused symlink name for the repository");
        }
    }

  /* Report success to the user. */
  g_autofree char *src_repo_path = g_file_get_path (ostree_repo_get_path (src_repo));

  g_print ("Copied %u/%u refs successfully from ‘%s’ to ‘%s’ repository in ‘%s’.\n", num_refs, num_refs,
           src_repo_path, dest_repo_path, mount_root_path);

  return TRUE;
}

gboolean
flatpak_builtin_create_usb (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) all_collection_ids = NULL;
  g_autoptr(GHashTable) remote_arch_map = NULL;
  FlatpakDir *dir = NULL;
  char **prefs = NULL;
  unsigned int i, n_prefs;
  FlatpakKinds kinds;
  OstreeRepo *src_repo;

  context = g_option_context_new (_("MOUNT-PATH [REF…] - Copy apps or runtimes onto removable media"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc < 3)
    return usage_error (context, _("MOUNT-PATH and REF must be specified"), error);

  /* Open the USB stick, which must exist. Allow automounting and following symlinks. */
  const char *mount_root_path = argv[1];
  struct stat mount_root_stbuf;

  glnx_autofd int mount_root_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, mount_root_path, TRUE, &mount_root_dfd, error))
    return FALSE;
  if (!glnx_fstat (mount_root_dfd, &mount_root_stbuf, error))
    return FALSE;

  prefs = &argv[2];
  n_prefs = argc - 2;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  /* This is a mapping from OstreeCollectionRef instances to CommitAndSubpaths
   * structs.  We need to tell ostree which commit to copy because the deployed
   * commit is not necessarily the latest one for a given ref, and we need the
   * subpaths because otherwise ostree will try and fail to pull the whole
   * commit */
  all_refs = g_hash_table_new_full (ostree_collection_ref_hash,
                                    ostree_collection_ref_equal,
                                    (GDestroyNotify) ostree_collection_ref_free,
                                    (GDestroyNotify) commit_and_subpaths_free);

  /* This maps from each remote name to a set of architectures */
  remote_arch_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_strfreev);

  /* This is a mapping from collection IDs to remote names. It is possible
   * for multiple remotes to have the same collection ID, but in that case
   * they should be mirrors of each other. */
  all_collection_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (i = 0; i < n_prefs; i++)
    {
      const char *pref = NULL;
      FlatpakKinds matched_kinds;
      g_autofree char *id = NULL;
      g_autofree char *arch = NULL;
      g_autofree char *branch = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GError) first_error = NULL;
      g_autoptr(FlatpakDecomposed) installed_ref = NULL;
      g_autoptr(GPtrArray) dirs_with_ref = NULL;
      FlatpakDir *this_ref_dir = NULL;
      g_autofree char *remote = NULL;
      g_autofree char *ref_collection_id = NULL;
      g_autoptr(OstreeCollectionRef) collection_ref = NULL;
      unsigned int j = 0;
      const char **arches;
      FlatpakKinds installed_ref_kind = 0;

      pref = prefs[i];

      if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, NULL,
                                          &matched_kinds, &id, &arch, &branch, error))
        return FALSE;

      dirs_with_ref = g_ptr_array_new ();
      for (j = 0; j < dirs->len; j++)
        {
          FlatpakDir *candidate_dir = g_ptr_array_index (dirs, j);
          g_autoptr(FlatpakDecomposed) ref = NULL;
          g_autofree char *ref_str = NULL;
          FlatpakKinds kind;

          ref_str = flatpak_dir_find_installed_ref (candidate_dir, id, branch, arch,
                                                    kinds, &kind, &local_error);
          if (ref_str != NULL)
            ref = flatpak_decomposed_new_from_ref (ref_str, error);

          if (ref == NULL)
            {
              if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
                {
                  if (first_error == NULL)
                    first_error = g_steal_pointer (&local_error);
                  g_clear_error (&local_error);
                }
              else
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
          else
            {
              g_ptr_array_add (dirs_with_ref, candidate_dir);
              if (installed_ref == NULL)
                {
                  installed_ref = flatpak_decomposed_ref (ref);
                  installed_ref_kind = kind;
                }
            }
        }

      if (dirs_with_ref->len == 0)
        {
          g_assert (first_error != NULL);
          /* No match anywhere, return the first NOT_INSTALLED error */
          g_propagate_error (error, g_steal_pointer (&first_error));
          return FALSE;
        }

      if (dirs_with_ref->len > 1)
        {
          g_autoptr(GString) dir_names = g_string_new ("");
          for (j = 0; j < dirs_with_ref->len; j++)
            {
              FlatpakDir *dir_with_ref = g_ptr_array_index (dirs_with_ref, j);
              g_autofree char *dir_name = flatpak_dir_get_name (dir_with_ref);
              if (j > 0)
                g_string_append (dir_names, ", ");
              g_string_append (dir_names, dir_name);
            }

          return flatpak_fail (error,
                               _("Ref ‘%s’ found in multiple installations: %s. You must specify one."),
                               pref, dir_names->str);
        }

      this_ref_dir = g_ptr_array_index (dirs_with_ref, 0);
      if (dir == NULL)
        dir = this_ref_dir;
      else if (dir != this_ref_dir)
        {
          g_autofree char *dir_name = flatpak_dir_get_name (dir);
          g_autofree char *this_ref_dir_name = flatpak_dir_get_name (this_ref_dir);

          return flatpak_fail (error,
                               _("Refs must all be in the same installation (found in %s and %s)."),
                               dir_name, this_ref_dir_name);
        }

      g_assert (installed_ref);
      if (arch == NULL)
        arch = flatpak_decomposed_dup_arch (installed_ref);
      if (branch == NULL)
        branch = flatpak_decomposed_dup_branch (installed_ref);

      remote = flatpak_dir_get_origin (dir, flatpak_decomposed_get_ref (installed_ref), cancellable, error);
      if (remote == NULL)
        return FALSE;

      ref_collection_id = flatpak_dir_get_remote_collection_id (dir, remote);
      if (ref_collection_id == NULL)
        return flatpak_fail (error,
                             _("Remote ‘%s’ does not have a collection ID set, which is required for P2P distribution of ‘%s’."),
                             remote, flatpak_decomposed_get_ref (installed_ref));

      arches = g_hash_table_lookup (remote_arch_map, remote);
      if (arches == NULL)
        {
          GPtrArray *arches_array = g_ptr_array_new ();
          g_ptr_array_add (arches_array, g_strdup (arch));
          g_ptr_array_add (arches_array, NULL);
          g_hash_table_insert (remote_arch_map, g_strdup (remote), g_ptr_array_free (arches_array, FALSE));
        }
      else if (!g_strv_contains (arches, arch))
        {
          GPtrArray *arches_array = g_ptr_array_new ();
          for (const char **iter = arches; *iter != NULL; ++iter)
            g_ptr_array_add (arches_array, g_strdup (*iter));
          g_ptr_array_add (arches_array, g_strdup (arch));
          g_ptr_array_add (arches_array, NULL);
          g_hash_table_replace (remote_arch_map, g_strdup (remote), g_ptr_array_free (arches_array, FALSE));
        }

      /* Add the main ref */
      {
        g_autoptr(GBytes) deploy_data = NULL;
        const char *commit;
        CommitAndSubpaths *c_s;

        deploy_data = flatpak_dir_get_deploy_data (dir, flatpak_decomposed_get_ref (installed_ref), FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
        if (deploy_data == NULL)
          return FALSE;

        if (flatpak_deploy_data_has_subpaths (deploy_data) && !opt_allow_partial)
          g_printerr (_("Warning: Ref ‘%s’ is partially installed. Use --allow-partial to suppress this message.\n"),
                      flatpak_decomposed_get_ref (installed_ref));

        commit = flatpak_deploy_data_get_commit (deploy_data);
        c_s = commit_and_subpaths_new (commit, NULL);

        g_hash_table_insert (all_collection_ids, g_strdup (ref_collection_id), g_strdup (remote));
        collection_ref = ostree_collection_ref_new (ref_collection_id, flatpak_decomposed_get_ref (installed_ref));
        g_hash_table_insert (all_refs, g_steal_pointer (&collection_ref), c_s);
      }

      /* Add dependencies and related refs */
      if (!(installed_ref_kind & FLATPAK_KINDS_RUNTIME) &&
          !add_runtime (all_refs, all_collection_ids, installed_ref, dir, cancellable, error))
        return FALSE;
      if (!add_related (all_refs, all_collection_ids, installed_ref, dir, cancellable, error))
        return FALSE;
    }

  g_assert (dir);
  src_repo = flatpak_dir_get_repo (dir);

  /* Add ostree-metadata and appstream refs for each collection ID */
  GLNX_HASH_TABLE_FOREACH_KV (all_collection_ids, const char *, collection_id, const char *, remote_name)
  {
    g_autoptr(OstreeCollectionRef) metadata_collection_ref = NULL;
    g_autoptr(OstreeCollectionRef) appstream_collection_ref = NULL;
    g_autoptr(OstreeCollectionRef) appstream2_collection_ref = NULL;
    g_autoptr(FlatpakRemoteState) state = NULL;
    g_autoptr(GError) local_error = NULL;
    g_autofree char *appstream_refspec = NULL;
    g_autofree char *appstream2_refspec = NULL;
    g_autofree char *appstream_ref = NULL;
    g_autofree char *appstream2_ref = NULL;
    const char **remote_arches;

    /* Try to update the repo metadata by creating a FlatpakRemoteState object,
     * but don't fail on error because we want this to work offline. */
    state = flatpak_dir_get_remote_state_optional (dir, remote_name, FALSE, cancellable, &local_error);
    if (state == NULL)
      {
        g_printerr (_("Warning: Couldn't update repo metadata for remote ‘%s’: %s\n"),
                    remote_name, local_error->message);
        g_clear_error (&local_error);
      }

    /* Add the ostree-metadata ref to the list if available */
    metadata_collection_ref = ostree_collection_ref_new (collection_id, OSTREE_REPO_METADATA_REF);
    if (ostree_repo_resolve_collection_ref (src_repo, metadata_collection_ref, FALSE,
                                            OSTREE_REPO_RESOLVE_REV_EXT_NONE,
                                            NULL, NULL, NULL))
      g_hash_table_insert (all_refs, g_steal_pointer (&metadata_collection_ref),
                           commit_and_subpaths_new (NULL, NULL));

    /* Add whatever appstream data is available for each arch */
    remote_arches = g_hash_table_lookup (remote_arch_map, remote_name);
    for (const char **iter = remote_arches; iter != NULL && *iter != NULL; ++iter)
      {
        const char *current_arch = *iter;
        g_autoptr(GPtrArray) appstream_dirs = NULL;
        g_autoptr(GError) appstream_error = NULL;
        g_autoptr(GError) appstream2_error = NULL;
        g_autofree char *commit = NULL;
        g_autofree char *commit2 = NULL;

        /* Try to update the appstream data, but don't fail on error because we
         * want this to work offline. */
        appstream_dirs = g_ptr_array_new ();
        g_ptr_array_add (appstream_dirs, dir);
        if (!update_appstream (appstream_dirs, remote_name, current_arch, 0, TRUE, cancellable, &local_error))
          {
            g_printerr (_("Warning: Couldn't update appstream data for remote ‘%s’ arch ‘%s’: %s\n"),
                        remote_name, current_arch, local_error->message);
            g_clear_error (&local_error);
          }

        /* Copy the appstream data if it exists. It's optional because without it
         * the USB will still be useful to the flatpak CLI even if GNOME Software
         * wouldn't display the contents. */
        appstream_refspec = g_strdup_printf ("%s:appstream/%s", remote_name, current_arch);
        appstream_ref = g_strdup_printf ("appstream/%s", current_arch);
        appstream_collection_ref = ostree_collection_ref_new (collection_id, appstream_ref);
        if (ostree_repo_resolve_rev (src_repo, appstream_refspec, FALSE,
                                     &commit, &appstream_error))
          {
            g_hash_table_insert (all_refs, g_steal_pointer (&appstream_collection_ref),
                                 commit_and_subpaths_new (commit, NULL));
          }

        /* Copy the appstream2 data if it exists. */
        appstream2_refspec = g_strdup_printf ("%s:appstream2/%s", remote_name, current_arch);
        appstream2_ref = g_strdup_printf ("appstream2/%s", current_arch);
        appstream2_collection_ref = ostree_collection_ref_new (collection_id, appstream2_ref);
        if (ostree_repo_resolve_rev (src_repo, appstream2_refspec, FALSE,
                                     &commit2, &appstream2_error))
          {
            g_hash_table_insert (all_refs, g_steal_pointer (&appstream2_collection_ref),
                                 commit_and_subpaths_new (commit2, NULL));
          }
        else
          {
            if (appstream_error != NULL)
              {
                /* Print a warning if both appstream and appstream2 are missing */
                g_printerr (_("Warning: Couldn't find appstream data for remote ‘%s’ arch ‘%s’: %s; %s\n"),
                            remote_name, current_arch, appstream2_error->message, appstream_error->message);
              }
            else
              {
                /* Appstream2 is only for efficiency, so just print a debug message */
                g_debug (_("Couldn't find appstream2 data for remote ‘%s’ arch ‘%s’: %s\n"),
                         remote_name, current_arch, appstream2_error->message);
              }
          }
      }
  }

  /* Delete the local source repo summary if it exists. Old versions of this
   * command erroneously created it and if it's outdated that causes problems. */
  if (!flatpak_dir_update_summary (dir, TRUE, cancellable, error))
    return FALSE;

  /* Now use code copied from `ostree create-usb` to do the actual copying. We
   * can't just call out to `ostree` because (a) flatpak doesn't have a
   * dependency on the ostree command line tools and (b) we need to only pull
   * certain subpaths for partial refs. */
  /* FIXME: Use libostree after fixing https://github.com/ostreedev/ostree/issues/1610 */
  {
    g_autoptr(GString) all_refs_str = g_string_new ("");

    GLNX_HASH_TABLE_FOREACH (all_refs, OstreeCollectionRef *, collection_ref)
    {
      if (!ostree_validate_collection_id (collection_ref->collection_id, error))
        return FALSE;
      if (!ostree_validate_rev (collection_ref->ref_name, error))
        return FALSE;

      g_string_append_printf (all_refs_str, "(%s, %s) ", collection_ref->collection_id, collection_ref->ref_name);
    }
    g_debug ("Copying the following refs: %s", all_refs_str->str);

    if (!ostree_create_usb (context, src_repo, mount_root_path, mount_root_stbuf,
                            mount_root_dfd, all_refs, cancellable, error))
      return FALSE;
  }

  return TRUE;
}

gboolean
flatpak_complete_create_usb (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakKinds kinds;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, NULL, NULL))
    return FALSE;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  switch (completion->argc)
    {
    case 0:
    case 1: /* MOUNT-PATH */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      flatpak_complete_file (completion, "__FLATPAK_DIR");

      break;

    default: /* REF */
      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          flatpak_complete_partial_ref (completion, kinds, opt_arch, dir, NULL);
        }
      break;
    }

  return TRUE;
}
