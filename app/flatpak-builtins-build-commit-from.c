/*
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

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"

static char *opt_src_repo;
static char *opt_src_ref;
static char *opt_subject;
static char *opt_body;
static gboolean opt_update_appstream;
static gboolean opt_no_update_summary;
static gboolean opt_untrusted;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;

static GOptionEntry options[] = {
  { "src-repo", 's', 0, G_OPTION_ARG_STRING, &opt_src_repo, N_("Source repo dir"), N_("SRC-REPO") },
  { "src-ref", 's', 0, G_OPTION_ARG_STRING, &opt_src_ref, N_("Source repo ref"), N_("SRC-REF") },
  { "untrusted", 0, 0, G_OPTION_ARG_NONE, &opt_untrusted, "Do not trust SRC-REPO", NULL },
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, N_("One line subject"), N_("SUBJECT") },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &opt_body, N_("Full description"), N_("BODY") },
  { "update-appstream", 0, 0, G_OPTION_ARG_NONE, &opt_update_appstream, N_("Update the appstream branch"), NULL },
  { "no-update-summary", 0, 0, G_OPTION_ARG_NONE, &opt_no_update_summary, N_("Don't update the summary"), NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the commit with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
  { NULL }
};

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
  g_autofree char *resolved_ref = NULL;
  g_autoptr(FlatpakRepoTransaction) transaction = NULL;
  g_autoptr(GPtrArray) src_refs = NULL;
  g_autoptr(GPtrArray) resolved_src_refs = NULL;
  OstreeRepoCommitState src_commit_state;
  int i;

  context = g_option_context_new (_("DST-REPO [DST-REF]... - Make a new commit based on existing commit(s)"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("DST-REPO must be specified"), error);

  dst_repo_arg = argv[1];

  dst_refs = (const char **)argv + 2;
  n_dst_refs = argc - 2;

  if (opt_src_repo == NULL && n_dst_refs != 1)
    return usage_error (context, _("If --src-repo is not specified, exactly one destination ref must be specified"), error);

  if (opt_src_ref != NULL && n_dst_refs != 1)
    return usage_error (context, _("If --src-ref is specified, exactly one destination ref must be specified"), error);

  if (opt_src_repo == NULL && opt_src_ref == NULL)
    return flatpak_fail (error, _("Either --src-repo or --src-ref must be specified."));

  dst_repofile = g_file_new_for_commandline_arg (dst_repo_arg);
  if (!g_file_query_exists (dst_repofile, cancellable))
    return flatpak_fail (error, _("'%s' is not a valid repository"), dst_repo_arg);

  dst_repo = ostree_repo_new (dst_repofile);
  if (!ostree_repo_open (dst_repo, cancellable, error))
    return FALSE;

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

          keys = (const char **)g_hash_table_get_keys_as_array (all_src_refs, NULL);

          for (i = 0; keys[i] != NULL; i++)
            {
              if (g_str_has_prefix (keys[i], "runtime/") ||
                  g_str_has_prefix (keys[i], "app/"))
                g_ptr_array_add (src_refs, g_strdup (keys[i]));
            }
          n_dst_refs = src_refs->len;
          dst_refs = (const char **)src_refs->pdata;
        }
      else
        {
          for (i = 0; i < n_dst_refs; i++)
            g_ptr_array_add (src_refs, g_strdup (dst_refs[i]));
        }
    }

  resolved_src_refs = g_ptr_array_new_with_free_func (g_free);
  for (i = 0; i < src_refs->len; i++)
    {
      const char *src_ref = g_ptr_array_index (src_refs, i);
      char *resolved_ref;

      if (!ostree_repo_resolve_rev (src_repo, src_ref, FALSE, &resolved_ref, error))
        return FALSE;

      g_ptr_array_add (resolved_src_refs, resolved_ref);
    }

  if (src_repo_uri != NULL)
    {
      OstreeRepoPullFlags pullflags = 0;
      GVariantBuilder builder;
      g_autoptr(OstreeAsyncProgress) progress = NULL;
      g_auto(GLnxConsoleRef) console = { 0, };
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
                             g_variant_new_variant (g_variant_new_strv ((const char *const*) resolved_src_refs->pdata,
                                                                        resolved_src_refs->len)));
      g_variant_builder_add (&builder, "{s@v}", "depth",
                             g_variant_new_variant (g_variant_new_int32 (0)));

      res = ostree_repo_pull_with_options (dst_repo, src_repo_uri,
                                           g_variant_builder_end (&builder),
                                           progress,
                                           cancellable, error);

      if (progress)
        ostree_async_progress_finish (progress);

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

  for (i = 0; i < resolved_src_refs->len; i ++)
    {
      const char *dst_ref = dst_refs[i];
      const char *resolved_ref = g_ptr_array_index (resolved_src_refs, i);
      g_autofree char *dst_parent = NULL;
      g_autoptr(GFile) dst_parent_root = NULL;
      g_autoptr(GFile) src_ref_root = NULL;
      g_autoptr(GVariant) src_commitv = NULL;
      g_autoptr(OstreeMutableTree) mtree = NULL;
      g_autoptr(GFile) dst_root = NULL;
      g_autoptr(GVariant) commitv_metadata = NULL;
      const char *subject;
      const char *body;
      g_autofree char *commit_checksum = NULL;

      if (!ostree_repo_resolve_rev (dst_repo, dst_ref, TRUE, &dst_parent, error))
        return FALSE;

      if (dst_parent != NULL &&
          !ostree_repo_read_commit (dst_repo, dst_parent, &dst_parent_root, NULL, cancellable, error))
        return FALSE;

      if (!ostree_repo_read_commit (dst_repo, resolved_ref, &src_ref_root, NULL, cancellable, error))
        return FALSE;

      if (!ostree_repo_load_commit (dst_repo, resolved_ref, &src_commitv, &src_commit_state, error))
        return FALSE;

      if (src_commit_state & OSTREE_REPO_COMMIT_STATE_PARTIAL)
        return flatpak_fail (error, _("Can't commit from partial source commit."));

      /* Don't create a new commit if this is the same tree */
      if (dst_parent_root != NULL && g_file_equal (dst_parent_root, src_ref_root))
        {
          g_print ("%s: no change\n", dst_ref);
          continue;
        }

      mtree = ostree_mutable_tree_new ();
      if (!ostree_repo_write_directory_to_mtree (dst_repo, src_ref_root, mtree, NULL,
                                                 cancellable, error))
        return FALSE;

      if (!ostree_repo_write_mtree (dst_repo, mtree, &dst_root, cancellable, error))
        return FALSE;

      commitv_metadata = g_variant_get_child_value (src_commitv, 0);

      g_variant_get_child (src_commitv, 3, "s", &subject);
      if (opt_subject)
        subject = (const char *)opt_subject;
      g_variant_get_child (src_commitv, 4, "s", &body);
      if (opt_body)
        body = (const char *)opt_body;

      if (!ostree_repo_write_commit_with_time (dst_repo, dst_parent, subject, body, commitv_metadata,
                                               OSTREE_REPO_FILE (dst_root),
                                               ostree_commit_get_timestamp (src_commitv),
                                               &commit_checksum, cancellable, error))
        return FALSE;

      g_print ("%s: %s\n", dst_ref, commit_checksum);

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

      ostree_repo_transaction_set_ref (dst_repo, NULL, dst_ref, commit_checksum);
    }

  if (!ostree_repo_commit_transaction (dst_repo, NULL, cancellable, error))
    return FALSE;

  if (opt_update_appstream &&
      !flatpak_repo_generate_appstream (dst_repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, cancellable, error))
    return FALSE;

  if (!opt_no_update_summary &&
      !flatpak_repo_update (dst_repo,
                            (const char **) opt_gpg_key_ids,
                            opt_gpg_homedir,
                            cancellable,
                            error))
    return FALSE;

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
