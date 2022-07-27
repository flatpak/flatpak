/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2015 Red Hat, Inc
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

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "parse-datetime.h"

static char *opt_src_repo;
static char *opt_src_ref;
static char *opt_subject;
static char *opt_body;
static gboolean opt_update_appstream;
static gboolean opt_no_update_summary;
static gboolean opt_untrusted;
static gboolean opt_disable_fsync;
static gboolean opt_force;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;
static char *opt_endoflife;
static char **opt_endoflife_rebase;
static char **opt_endoflife_rebase_new;
static char **opt_subsets;
static char *opt_timestamp;
static char **opt_extra_collection_ids;
static int opt_token_type = -1;
static gboolean opt_no_summary_index = FALSE;

static GOptionEntry options[] = {
  { "src-repo", 0, 0, G_OPTION_ARG_STRING, &opt_src_repo, N_("Source repo dir"), N_("SRC-REPO") },
  { "src-ref", 0, 0, G_OPTION_ARG_STRING, &opt_src_ref, N_("Source repo ref"), N_("SRC-REF") },
  { "untrusted", 0, 0, G_OPTION_ARG_NONE, &opt_untrusted, "Do not trust SRC-REPO", NULL },
  { "force", 0, 0, G_OPTION_ARG_NONE, &opt_force, "Always commit, even if same content", NULL },
  { "extra-collection-id", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_extra_collection_ids, "Add an extra collection id ref and binding", "COLLECTION-ID" },
  { "subset", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_subsets, "Add to a named subset", "SUBSET" },
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, N_("One line subject"), N_("SUBJECT") },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &opt_body, N_("Full description"), N_("BODY") },
  { "update-appstream", 0, 0, G_OPTION_ARG_NONE, &opt_update_appstream, N_("Update the appstream branch"), NULL },
  { "no-update-summary", 0, 0, G_OPTION_ARG_NONE, &opt_no_update_summary, N_("Don't update the summary"), NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the commit with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
  { "end-of-life", 0, 0, G_OPTION_ARG_STRING, &opt_endoflife, N_("Mark build as end-of-life"), N_("REASON") },
  { "end-of-life-rebase", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_endoflife_rebase, N_("Mark refs matching the OLDID prefix as end-of-life, to be replaced with the given NEWID"), N_("OLDID=NEWID") },
  { "token-type", 0, 0, G_OPTION_ARG_INT, &opt_token_type, N_("Set type of token needed to install this commit"), N_("VAL") },
  { "timestamp", 0, 0, G_OPTION_ARG_STRING, &opt_timestamp, N_("Override the timestamp of the commit (NOW for current time)"), N_("TIMESTAMP") },
  { "disable-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
  { "no-summary-index", 0, 0, G_OPTION_ARG_NONE, &opt_no_summary_index, N_("Don't generate a summary index"), NULL },
  { NULL }
};

#define OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "(uayttay)"
#define OSTREE_STATIC_DELTA_FALLBACK_FORMAT "(yaytt)"
#define OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT "(a{sv}tayay" OSTREE_COMMIT_GVARIANT_STRING "aya" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT ")"

static char *
_ostree_get_relative_static_delta_path (const char *from,
                                        const char *to,
                                        const char *target)
{
  guint8 csum_to[OSTREE_SHA256_DIGEST_LEN];
  char to_b64[44];
  guint8 csum_to_copy[OSTREE_SHA256_DIGEST_LEN];
  GString *ret = g_string_new ("deltas/");

  ostree_checksum_inplace_to_bytes (to, csum_to);
  ostree_checksum_b64_inplace_from_bytes (csum_to, to_b64);
  ostree_checksum_b64_inplace_to_bytes (to_b64, csum_to_copy);

  g_assert (memcmp (csum_to, csum_to_copy, OSTREE_SHA256_DIGEST_LEN) == 0);

  if (from != NULL)
    {
      guint8 csum_from[OSTREE_SHA256_DIGEST_LEN];
      char from_b64[44];

      ostree_checksum_inplace_to_bytes (from, csum_from);
      ostree_checksum_b64_inplace_from_bytes (csum_from, from_b64);

      g_string_append_c (ret, from_b64[0]);
      g_string_append_c (ret, from_b64[1]);
      g_string_append_c (ret, '/');
      g_string_append (ret, from_b64 + 2);
      g_string_append_c (ret, '-');
    }

  g_string_append_c (ret, to_b64[0]);
  g_string_append_c (ret, to_b64[1]);
  if (from == NULL)
    g_string_append_c (ret, '/');
  g_string_append (ret, to_b64 + 2);

  if (target != NULL)
    {
      g_string_append_c (ret, '/');
      g_string_append (ret, target);
    }

  return g_string_free (ret, FALSE);
}

static GVariant *
new_bytearray (const guchar *data,
               gsize         len)
{
  gpointer data_copy = g_memdup (data, len);
  GVariant *ret = g_variant_new_from_data (G_VARIANT_TYPE ("ay"), data_copy,
                                           len, FALSE, g_free, data_copy);

  return ret;
}

static gboolean
rewrite_delta (OstreeRepo *src_repo,
               const char *src_commit,
               OstreeRepo *dst_repo,
               const char *dst_commit,
               GVariant   *dst_commitv,
               const char *from,
               GError    **error)
{
  g_autoptr(GFile) src_delta_file = NULL;
  g_autoptr(GFile) dst_delta_file = NULL;
  g_autofree char *src_detached_key = _ostree_get_relative_static_delta_path (from, src_commit, "commitmeta");
  g_autofree char *dst_detached_key = _ostree_get_relative_static_delta_path (from, dst_commit, "commitmeta");
  g_autofree char *src_delta_dir = _ostree_get_relative_static_delta_path (from, src_commit, NULL);
  g_autofree char *dst_delta_dir = _ostree_get_relative_static_delta_path (from, dst_commit, NULL);
  g_autofree char *src_superblock_path = _ostree_get_relative_static_delta_path (from, src_commit, "superblock");
  g_autofree char *dst_superblock_path = _ostree_get_relative_static_delta_path (from, dst_commit, "superblock");
  GMappedFile *mfile = NULL;
  g_auto(GVariantBuilder) superblock_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
  g_autoptr(GVariant) src_superblock = NULL;
  g_autoptr(GVariant) dst_superblock = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GVariant) dst_detached = NULL;
  g_autoptr(GVariant) src_metadata = NULL;
  g_autoptr(GVariant) src_recurse = NULL;
  g_autoptr(GVariant) src_parts = NULL;
  g_auto(GVariantDict) dst_metadata_dict = FLATPAK_VARIANT_DICT_INITIALIZER;
  int i;

  src_delta_file = g_file_resolve_relative_path (ostree_repo_get_path (src_repo), src_superblock_path);
  mfile = g_mapped_file_new (flatpak_file_get_path_cached (src_delta_file), FALSE, NULL);
  if (mfile == NULL)
    return TRUE; /* No superblock, not an error */

  bytes = g_mapped_file_get_bytes (mfile);
  g_mapped_file_unref (mfile);

  src_superblock = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT), bytes, FALSE));

  src_metadata = g_variant_get_child_value (src_superblock, 0);
  src_recurse = g_variant_get_child_value (src_superblock, 5);
  src_parts = g_variant_get_child_value (src_superblock, 6);

  if (g_variant_n_children (src_recurse) != 0)
    return flatpak_fail (error, "Recursive deltas not supported, ignoring");

  g_variant_builder_init (&superblock_builder, G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT));

  g_variant_dict_init (&dst_metadata_dict, src_metadata);
  g_variant_dict_remove (&dst_metadata_dict, src_detached_key);
  if (ostree_repo_read_commit_detached_metadata (dst_repo, dst_commit, &dst_detached, NULL, NULL) &&
      dst_detached != NULL)
    g_variant_dict_insert_value (&dst_metadata_dict, dst_detached_key, dst_detached);

  g_variant_builder_add_value (&superblock_builder, g_variant_dict_end (&dst_metadata_dict));
  g_variant_builder_add_value (&superblock_builder, g_variant_get_child_value (src_superblock, 1)); /* timestamp */
  g_variant_builder_add_value (&superblock_builder, from ? ostree_checksum_to_bytes_v (from) : new_bytearray ((guchar *) "", 0));
  g_variant_builder_add_value (&superblock_builder, ostree_checksum_to_bytes_v (dst_commit));
  g_variant_builder_add_value (&superblock_builder, dst_commitv);
  g_variant_builder_add_value (&superblock_builder, src_recurse);
  g_variant_builder_add_value (&superblock_builder, src_parts);
  g_variant_builder_add_value (&superblock_builder, g_variant_get_child_value (src_superblock, 7)); /* fallback */

  dst_superblock = g_variant_ref_sink (g_variant_builder_end (&superblock_builder));

  if (!glnx_shutil_mkdir_p_at (ostree_repo_get_dfd (dst_repo), dst_delta_dir, 0755, NULL, error))
    return FALSE;

  for (i = 0; i < g_variant_n_children (src_parts); i++)
    {
      g_autofree char *src_part_path = g_strdup_printf ("%s/%d", src_delta_dir, i);
      g_autofree char *dst_part_path = g_strdup_printf ("%s/%d", dst_delta_dir, i);

      if (!glnx_file_copy_at (ostree_repo_get_dfd (src_repo),
                              src_part_path,
                              NULL,
                              ostree_repo_get_dfd (dst_repo),
                              dst_part_path,
                              GLNX_FILE_COPY_OVERWRITE | GLNX_FILE_COPY_NOXATTRS,
                              NULL, error))
        return FALSE;
    }

  dst_delta_file = g_file_resolve_relative_path (ostree_repo_get_path (dst_repo), dst_superblock_path);
  if (!flatpak_variant_save (dst_delta_file, dst_superblock, NULL, error))
    return FALSE;

  return TRUE;
}

static gboolean
get_subsets (char **subsets, GVariant **out)
{
  g_autoptr(GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  gboolean found = FALSE;

  if (subsets == NULL)
    return FALSE;

  for (int i = 0; subsets[i] != NULL; i++)
    {
      const char *subset = subsets[i];
      if (*subset != 0)
        {
          found = TRUE;
          g_variant_builder_add (builder, "s", subset);
        }
    }

  if (!found)
    return FALSE;

  *out = g_variant_ref_sink (g_variant_builder_end (builder));
  return TRUE;
}


gboolean
flatpak_builtin_build_commit_from (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) dst_repofile = NULL;
  g_autoptr(OstreeRepo) dst_repo = NULL;
  g_autoptr(GFile) src_repofile = NULL;
  g_autoptr(OstreeRepo) src_repo = NULL;
  g_autofree char *src_repo_uri = NULL;
  const char *dst_repo_arg;
  const char **dst_refs;
  int n_dst_refs = 0;
  g_autoptr(FlatpakRepoTransaction) transaction = NULL;
  g_autoptr(GPtrArray) src_refs = NULL;
  g_autoptr(GPtrArray) resolved_src_refs = NULL;
  OstreeRepoCommitState src_commit_state;
  struct timespec ts;
  guint64 timestamp;
  int i;
  const char *src_collection_id;

  context = g_option_context_new (_("DST-REPO [DST-REF…] - Make a new commit from existing commits"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("DST-REPO must be specified"), error);

  dst_repo_arg = argv[1];

  dst_refs = (const char **) argv + 2;
  n_dst_refs = argc - 2;

  if (opt_src_repo == NULL && n_dst_refs != 1)
    return usage_error (context, _("If --src-repo is not specified, exactly one destination ref must be specified"), error);

  if (opt_src_ref != NULL && n_dst_refs != 1)
    return usage_error (context, _("If --src-ref is specified, exactly one destination ref must be specified"), error);

  if (opt_src_repo == NULL && opt_src_ref == NULL)
    return flatpak_fail (error, _("Either --src-repo or --src-ref must be specified"));

  /* Always create a commit if we're eol:ing, even though the app is the same */
  if (opt_endoflife != NULL || opt_endoflife_rebase != NULL)
    opt_force = TRUE;

  if (opt_endoflife_rebase)
    {
      opt_endoflife_rebase_new = g_new0 (char *, g_strv_length (opt_endoflife_rebase));

      for (i = 0; opt_endoflife_rebase[i] != NULL; i++)
        {
          char *rebase_old = opt_endoflife_rebase[i];
          char *rebase_new = strchr (rebase_old, '=');

          if (rebase_new == NULL) {
            return usage_error (context, _("Invalid argument format of use  --end-of-life-rebase=OLDID=NEWID"), error);
          }
          *rebase_new = 0;
          rebase_new++;

          if (!flatpak_is_valid_name (rebase_old, -1, error))
            return glnx_prefix_error (error, _("Invalid name %s in --end-of-life-rebase"), rebase_old);

          if (!flatpak_is_valid_name (rebase_new, -1, error))
            return glnx_prefix_error (error, _("Invalid name %s in --end-of-life-rebase"), rebase_new);

          opt_endoflife_rebase_new[i] = rebase_new;
        }
    }

  if (opt_timestamp)
    {
      if (!parse_datetime (&ts, opt_timestamp, NULL))
        return flatpak_fail (error, _("Could not parse '%s'"), opt_timestamp);
    }

  dst_repofile = g_file_new_for_commandline_arg (dst_repo_arg);
  if (!g_file_query_exists (dst_repofile, cancellable))
    return flatpak_fail (error, _("'%s' is not a valid repository"), dst_repo_arg);

  dst_repo = ostree_repo_new (dst_repofile);
  if (!ostree_repo_open (dst_repo, cancellable, error))
    return FALSE;

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (dst_repo, TRUE);

  if (opt_src_repo)
    {
      src_repofile = g_file_new_for_commandline_arg (opt_src_repo);
      if (!g_file_query_exists (src_repofile, cancellable))
        return flatpak_fail (error, _("'%s' is not a valid repository"), opt_src_repo);

      src_repo_uri = g_file_get_uri (src_repofile);
      src_repo = ostree_repo_new (src_repofile);
      if (!ostree_repo_open (src_repo, cancellable, error))
        return FALSE;
    }
  else
    {
      src_repo = g_object_ref (dst_repo);
    }

  src_refs = g_ptr_array_new_with_free_func (g_free);
  if (opt_src_ref)
    {
      g_assert (n_dst_refs == 1);
      g_ptr_array_add (src_refs, g_strdup (opt_src_ref));
    }
  else
    {
      g_assert (opt_src_repo != NULL);
      if (n_dst_refs == 0)
        {
          g_autofree const char **keys = NULL;
          g_autoptr(GHashTable) all_src_refs = NULL;

          if (!ostree_repo_list_refs (src_repo, NULL, &all_src_refs,
                                      cancellable, error))
            return FALSE;

          keys = (const char **) g_hash_table_get_keys_as_array (all_src_refs, NULL);

          for (i = 0; keys[i] != NULL; i++)
            {
              if (g_str_has_prefix (keys[i], "runtime/") ||
                  g_str_has_prefix (keys[i], "app/"))
                g_ptr_array_add (src_refs, g_strdup (keys[i]));
            }
          n_dst_refs = src_refs->len;
          dst_refs = (const char **) src_refs->pdata;
        }
      else
        {
          for (i = 0; i < n_dst_refs; i++)
            g_ptr_array_add (src_refs, g_strdup (dst_refs[i]));
        }
    }

  src_collection_id = ostree_repo_get_collection_id (src_repo);
  resolved_src_refs = g_ptr_array_new_with_free_func (g_free);
  for (i = 0; i < src_refs->len; i++)
    {
      const char *src_ref = g_ptr_array_index (src_refs, i);
      char *resolved_ref;

      if (!flatpak_repo_resolve_rev (src_repo, src_collection_id, NULL, src_ref, FALSE,
                                     &resolved_ref, cancellable, error))
        return FALSE;

      g_ptr_array_add (resolved_src_refs, resolved_ref);
    }

  if (src_repo_uri != NULL)
    {
      OstreeRepoPullFlags pullflags = 0;
      GVariantBuilder builder;
      g_autoptr(OstreeAsyncProgressFinish) progress = NULL;
      g_auto(GLnxConsoleRef) console = { 0, };
      g_autoptr(GVariant) pull_options = NULL;
      gboolean res;

      if (opt_untrusted)
        pullflags |= OSTREE_REPO_PULL_FLAGS_UNTRUSTED;

      glnx_console_lock (&console);
      if (console.is_tty)
        progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&builder, "{s@v}", "flags",
                             g_variant_new_variant (g_variant_new_int32 (pullflags)));
      g_variant_builder_add (&builder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) resolved_src_refs->pdata,
                                                                        resolved_src_refs->len)));
      g_variant_builder_add (&builder, "{s@v}", "depth",
                             g_variant_new_variant (g_variant_new_int32 (0)));

      pull_options = g_variant_ref_sink (g_variant_builder_end (&builder));
      res = ostree_repo_pull_with_options (dst_repo, src_repo_uri,
                                           pull_options,
                                           progress,
                                           cancellable, error);

      if (!res)
        return FALSE;
    }

  /* By now we have the commit with commit_id==resolved_ref and dependencies in dst_repo. We now create a new
   * commit based on the toplevel tree ref from that commit.
   * This is equivalent to:
   *   ostree commit --skip-if-unchanged --repo=${destrepo} --tree=ref=${resolved_ref}
   */

  transaction = flatpak_repo_transaction_start (dst_repo, cancellable, error);
  if (transaction == NULL)
    return FALSE;

  for (i = 0; i < resolved_src_refs->len; i++)
    {
      const char *dst_ref = dst_refs[i];
      const char *resolved_ref = g_ptr_array_index (resolved_src_refs, i);
      g_autofree char *dst_parent = NULL;
      g_autoptr(GFile) dst_parent_root = NULL;
      g_autoptr(GFile) src_ref_root = NULL;
      g_autoptr(GVariant) src_commitv = NULL;
      g_autoptr(GVariant) dst_commitv = NULL;
      g_autoptr(GVariant) subsets_v = NULL;
      g_autoptr(OstreeMutableTree) mtree = NULL;
      g_autoptr(GFile) dst_root = NULL;
      g_autoptr(GVariant) commitv_metadata = NULL;
      g_autoptr(GVariant) metadata = NULL;
      const char *subject;
      const char *body;
      g_autofree char *commit_checksum = NULL;
      GVariantBuilder metadata_builder;
      gint j;
      const char *dst_collection_id = NULL;
      const char *main_collection_id = NULL;
      g_autoptr(GPtrArray) collection_ids = NULL;

      dst_collection_id = ostree_repo_get_collection_id (dst_repo);

      if (!flatpak_repo_resolve_rev (dst_repo, dst_collection_id, NULL, dst_ref, TRUE,
                                     &dst_parent, cancellable, error))
        return FALSE;

      if (dst_parent != NULL &&
          !ostree_repo_read_commit (dst_repo, dst_parent, &dst_parent_root, NULL, cancellable, error))
        return FALSE;

      if (!ostree_repo_read_commit (dst_repo, resolved_ref, &src_ref_root, NULL, cancellable, error))
        return FALSE;

      if (!ostree_repo_load_commit (dst_repo, resolved_ref, &src_commitv, &src_commit_state, error))
        return FALSE;

      if (src_commit_state & OSTREE_REPO_COMMIT_STATE_PARTIAL)
        return flatpak_fail (error, _("Can't commit from partial source commit"));

      /* Don't create a new commit if this is the same tree */
      if (!opt_force && dst_parent_root != NULL && g_file_equal (dst_parent_root, src_ref_root))
        {
          g_print (_("%s: no change\n"), dst_ref);
          continue;
        }

      mtree = ostree_mutable_tree_new ();
      if (!ostree_repo_write_directory_to_mtree (dst_repo, src_ref_root, mtree, NULL,
                                                 cancellable, error))
        return FALSE;

      if (!ostree_repo_write_mtree (dst_repo, mtree, &dst_root, cancellable, error))
        return FALSE;

      commitv_metadata = g_variant_get_child_value (src_commitv, 0);

      g_variant_get_child (src_commitv, 3, "&s", &subject);
      if (opt_subject)
        subject = (const char *) opt_subject;
      g_variant_get_child (src_commitv, 4, "&s", &body);
      if (opt_body)
        body = (const char *) opt_body;

      collection_ids = g_ptr_array_new_with_free_func (g_free);
      if (dst_collection_id)
        {
          main_collection_id = dst_collection_id;
          g_ptr_array_add (collection_ids, g_strdup (dst_collection_id));
        }

      if (opt_extra_collection_ids != NULL)
        {
          for (j = 0; opt_extra_collection_ids[j] != NULL; j++)
            {
              const char *cid = opt_extra_collection_ids[j];
              if (main_collection_id == NULL)
                main_collection_id = cid; /* Fall back to first arg */

              if (g_strcmp0 (cid, dst_collection_id) != 0)
                g_ptr_array_add (collection_ids, g_strdup (cid));
            }
        }

      g_ptr_array_sort (collection_ids, (GCompareFunc) flatpak_strcmp0_ptr);

      /* Copy old metadata */
      g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

      /* Bindings. xa.ref is deprecated but added anyway for backwards compatibility. */
      g_variant_builder_add (&metadata_builder, "{sv}", "ostree.collection-binding",
                             g_variant_new_string (main_collection_id ? main_collection_id : ""));
      if (collection_ids->len > 0)
        {
          g_autoptr(GVariantBuilder) cr_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ss)"));

          for (j = 0; j < collection_ids->len; j++)
            g_variant_builder_add (cr_builder, "(ss)", g_ptr_array_index (collection_ids, j), dst_ref);

          g_variant_builder_add (&metadata_builder, "{sv}", "ostree.collection-refs-binding",
                                 g_variant_builder_end (cr_builder));
        }
      g_variant_builder_add (&metadata_builder, "{sv}", "ostree.ref-binding",
                             g_variant_new_strv (&dst_ref, 1));
      g_variant_builder_add (&metadata_builder, "{sv}", "xa.ref", g_variant_new_string (dst_ref));

      /* Record the source commit. This is nice to have, but it also
         means the commit-from gets a different commit id, which
         avoids problems with e.g.  sharing .commitmeta files
         (signatures) */
      g_variant_builder_add (&metadata_builder, "{sv}", "xa.from_commit", g_variant_new_string (resolved_ref));

      if (opt_src_repo)
        {
          guint64 download_size;
          if (!flatpak_repo_collect_sizes (dst_repo, src_ref_root, NULL, &download_size,
                                           cancellable, error))
            {
              return FALSE;
            }
          g_variant_builder_add (&metadata_builder, "{sv}", "xa.download-size", g_variant_new_uint64 (GUINT64_TO_BE (download_size)));
        }

      for (j = 0; j < g_variant_n_children (commitv_metadata); j++)
        {
          g_autoptr(GVariant) child = g_variant_get_child_value (commitv_metadata, j);
          g_autoptr(GVariant) keyv = g_variant_get_child_value (child, 0);
          const char *key = g_variant_get_string (keyv, NULL);

          if (strcmp (key, "xa.ref") == 0 ||
              strcmp (key, "xa.from_commit") == 0 ||
              strcmp (key, "ostree.collection-binding") == 0 ||
              strcmp (key, "ostree.collection-refs-binding") == 0 ||
              strcmp (key, "ostree.ref-binding") == 0)
            continue;

          if (opt_src_repo && strcmp (key, "xa.download-size") == 0)
            continue;

          if (opt_endoflife &&
              strcmp (key, OSTREE_COMMIT_META_KEY_ENDOFLIFE) == 0)
            continue;

          if (opt_endoflife_rebase &&
              strcmp (key, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE) == 0)
            continue;

          if (opt_token_type >= 0 && strcmp (key, "xa.token-type") == 0)
            continue;

          if (opt_subsets != NULL && strcmp (key, "xa.subsets") == 0)
            continue;

          g_variant_builder_add_value (&metadata_builder, child);
        }

      if (opt_endoflife && *opt_endoflife)
        g_variant_builder_add (&metadata_builder, "{sv}", OSTREE_COMMIT_META_KEY_ENDOFLIFE,
                               g_variant_new_string (opt_endoflife));

      if (opt_endoflife_rebase)
        {
          g_auto(GStrv) dst_ref_parts = g_strsplit (dst_ref, "/", 0);

          for (j = 0; opt_endoflife_rebase[j] != NULL; j++)
            {
              const char *old_prefix = opt_endoflife_rebase[j];

              if (flatpak_has_name_prefix (dst_ref_parts[1], old_prefix))
                {
                  g_autofree char *new_id = g_strconcat (opt_endoflife_rebase_new[j], dst_ref_parts[1] + strlen(old_prefix), NULL);
                  g_autofree char *rebased_ref = g_build_filename (dst_ref_parts[0], new_id, dst_ref_parts[2], dst_ref_parts[3], NULL);

                  g_variant_builder_add (&metadata_builder, "{sv}", OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE,
                                         g_variant_new_string (rebased_ref));
                  break;
                }
            }
        }

      if (opt_token_type >= 0)
        g_variant_builder_add (&metadata_builder, "{sv}", "xa.token-type",
                               g_variant_new_int32 (GINT32_TO_LE (opt_token_type)));

      /* Skip "" subsets as they mean everything. This way --subsets= causes old subsets to be stripped from the original commit */
      if (get_subsets (opt_subsets, &subsets_v))
        g_variant_builder_add (&metadata_builder, "{sv}", "xa.subsets", subsets_v);

      timestamp = ostree_commit_get_timestamp (src_commitv);
      if (opt_timestamp)
        timestamp = ts.tv_sec;

      metadata = g_variant_ref_sink (g_variant_builder_end (&metadata_builder));
      if (!ostree_repo_write_commit_with_time (dst_repo, dst_parent, subject, body, metadata,
                                               OSTREE_REPO_FILE (dst_root),
                                               timestamp,
                                               &commit_checksum, cancellable, error))
        return FALSE;

      g_print ("%s: %s\n", dst_ref, commit_checksum);

      if (!ostree_repo_load_commit (dst_repo, commit_checksum, &dst_commitv, NULL, error))
        return FALSE;

      /* This doesn't copy the detached metadata. I'm not sure if this is a problem.
       * The main thing there is commit signatures, and we can't copy those, as the commit hash changes.
       */

      if (opt_gpg_key_ids)
        {
          char **iter;

          for (iter = opt_gpg_key_ids; iter && *iter; iter++)
            {
              const char *keyid = *iter;
              g_autoptr(GError) my_error = NULL;

              if (!ostree_repo_sign_commit (dst_repo,
                                            commit_checksum,
                                            keyid,
                                            opt_gpg_homedir,
                                            cancellable,
                                            &my_error) &&
                  !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                {
                  g_propagate_error (error, g_steal_pointer (&my_error));
                  return FALSE;
                }
            }
        }

      if (dst_collection_id != NULL)
        {
          OstreeCollectionRef ref = { (char *) dst_collection_id, (char *) dst_ref };
          ostree_repo_transaction_set_collection_ref (dst_repo, &ref, commit_checksum);
        }
      else
        {
          ostree_repo_transaction_set_ref (dst_repo, NULL, dst_ref, commit_checksum);
        }

      if (opt_extra_collection_ids)
        {
          for (j = 0; opt_extra_collection_ids[j] != NULL; j++)
            {
              OstreeCollectionRef ref = { (char *) opt_extra_collection_ids[j], (char *) dst_ref };
              ostree_repo_transaction_set_collection_ref (dst_repo, &ref, commit_checksum);
            }
        }

      /* Copy + Rewrite any deltas */
      {
        const char *from[2];
        gsize n_from = 0;

        if (dst_parent != NULL)
          from[n_from++] = dst_parent;
        from[n_from++] = NULL;

        for (j = 0; j < n_from; j++)
          {
            g_autoptr(GError) local_error = NULL;
            if (!rewrite_delta (src_repo, resolved_ref, dst_repo, commit_checksum, dst_commitv, from[j], &local_error))
              g_debug ("Failed to copy delta: %s", local_error->message);
          }
      }
    }

  if (!ostree_repo_commit_transaction (dst_repo, NULL, cancellable, error))
    return FALSE;

  if (opt_update_appstream &&
      !flatpak_repo_generate_appstream (dst_repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, 0, cancellable, error))
    return FALSE;

  if (!opt_no_update_summary)
    {
      FlatpakRepoUpdateFlags flags = FLATPAK_REPO_UPDATE_FLAG_NONE;

      if (opt_no_summary_index)
        flags |= FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX;

      g_debug ("Updating summary");
      if (!flatpak_repo_update (dst_repo, flags,
                                (const char **) opt_gpg_key_ids,
                                opt_gpg_homedir,
                                cancellable,
                                error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_build_commit_from (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) dst_repofile = NULL;
  g_autoptr(OstreeRepo) dst_repo = NULL;
  g_autoptr(GFile) src_repofile = NULL;
  g_autoptr(OstreeRepo) src_repo = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* DST-REPO */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;

    case 2: /* DST-REF */
      dst_repofile = g_file_new_for_commandline_arg (completion->argv[1]);
      dst_repo = ostree_repo_new (dst_repofile);
      if (ostree_repo_open (dst_repo, NULL, NULL))
        flatpak_complete_ref (completion, dst_repo);

      if (opt_src_repo)
        {
          src_repofile = g_file_new_for_commandline_arg (opt_src_repo);
          src_repo = ostree_repo_new (src_repofile);
          if (ostree_repo_open (src_repo, NULL, NULL))
            flatpak_complete_ref (completion, src_repo);
        }

      break;
    }

  return TRUE;
}
