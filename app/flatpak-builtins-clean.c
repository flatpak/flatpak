/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include <sys/file.h>
#include <unistd.h>
#include <glib/gi18n.h>

#include "flatpak-builtins.h"

static gboolean opt_dry_run;

static GOptionEntry options[] = {
  { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, N_("Don't make any changes"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_clean (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto(GLnxLockFile) lock = { 0, };
  FlatpakDir *dir;
  OstreeRepo *repo;

  context = g_option_context_new (_("- Remove cached installers for deployed refs"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  g_option_context_set_description (context, _("Cleaned commits require downloading extra data again before they can be redeployed; --no-pull will no longer work for them."));

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR, &dirs, cancellable, error))
    return FALSE;
  if (argc != 1)
    return usage_error (context, _("Unexpected argument"), error);

  dir = g_ptr_array_index (dirs, 0);
  if (!flatpak_dir_is_user (dir) && geteuid () != 0)
    return glnx_throw (error, _("Root privileges are required to clean a system installation"));

  if (!flatpak_dir_ensure_repo (dir, cancellable, error))
    return FALSE;

  /* Exclude pulls and deployments so detached metadata stays consistent,
   * including during a dry run. */
  if (!flatpak_dir_repo_lock (dir, &lock, LOCK_EX | LOCK_NB, cancellable, error))
    {
      g_prefix_error (error, _("Unable to lock installation for cleaning: "));
      return FALSE;
    }

  repo = flatpak_dir_get_repo (dir);
  refs = flatpak_dir_list_refs (dir, FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME,
                                cancellable, error);
  if (refs == NULL)
    return FALSE;

  for (size_t i = 0; i < refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
      g_autoptr(GBytes) deploy_data = NULL;
      g_autoptr(GVariant) metadata = NULL;
      g_autoptr(GVariant) extra_data = NULL;
      g_autoptr(GVariant) cleaned_metadata = NULL;
      g_auto(GVariantDict) dict = FLATPAK_VARIANT_DICT_INITIALIZER;
      const char *checksum;

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      deploy_data = flatpak_dir_get_deploy_data (dir, ref, FLATPAK_DEPLOY_VERSION_ANY,
                                                 cancellable, error);
      if (deploy_data == NULL)
        return FALSE;

      checksum = flatpak_deploy_data_get_commit (deploy_data);
      if (!g_hash_table_add (seen, g_strdup (checksum)))
        continue;

      if (!ostree_repo_read_commit_detached_metadata (repo, checksum, &metadata,
                                                      cancellable, error))
        return FALSE;
      if (metadata == NULL)
        continue;

      extra_data = g_variant_lookup_value (metadata, "xa.extra-data", NULL);
      if (extra_data == NULL)
        continue;

      g_variant_dict_init (&dict, metadata);
      g_variant_dict_remove (&dict, "xa.extra-data");
      cleaned_metadata = g_variant_ref_sink (g_variant_dict_end (&dict));

      if (!opt_dry_run &&
          !ostree_repo_write_commit_detached_metadata (repo, checksum, cleaned_metadata,
                                                       cancellable, error))
        return FALSE;

      if (opt_dry_run)
        g_print (_("Would remove cached extra data for %s (%s)\n"),
                 flatpak_decomposed_get_ref (ref), checksum);
      else
        g_print (_("Removed cached extra data for %s (%s)\n"),
                 flatpak_decomposed_get_ref (ref), checksum);
    }

  return TRUE;
}

gboolean
flatpak_complete_clean (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  g_autoptr(GPtrArray) dirs = NULL;

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_options (completion, user_entries);
  return TRUE;
}
