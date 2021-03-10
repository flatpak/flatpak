/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2019 Red Hat, Inc
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
#include "flatpak-utils-base-private.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-prune-private.h"

static char *opt_title;
static char *opt_comment;
static char *opt_description;
static char *opt_homepage;
static char *opt_icon;
static char *opt_redirect_url;
static char *opt_default_branch;
static char *opt_collection_id = NULL;
static gboolean opt_deploy_sideload_collection_id = FALSE;
static gboolean opt_deploy_collection_id = FALSE;
static gboolean opt_no_summary_index = FALSE;
static char **opt_gpg_import;
static char *opt_generate_delta_from;
static char *opt_generate_delta_to;
static char *opt_generate_delta_ref;
static char *opt_gpg_homedir;
static char **opt_gpg_key_ids;
static char **opt_sign_keys;
static char *opt_sign_name;
static gboolean opt_prune;
static gboolean opt_prune_dry_run;
static gboolean opt_generate_deltas;
static gboolean opt_no_update_appstream;
static gboolean opt_no_update_summary;
static gint opt_prune_depth = -1;
static gint opt_static_delta_jobs;
static char **opt_static_delta_ignore_refs;
static char *opt_authenticator_name = NULL;
static gboolean opt_authenticator_install = -1;
static char **opt_authenticator_options = NULL;

static GOptionEntry options[] = {
  { "redirect-url", 0, 0, G_OPTION_ARG_STRING, &opt_redirect_url, N_("Redirect this repo to a new URL"), N_("URL") },
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, N_("A nice name to use for this repository"), N_("TITLE") },
  { "comment", 0, 0, G_OPTION_ARG_STRING, &opt_comment, N_("A one-line comment for this repository"), N_("COMMENT") },
  { "description", 0, 0, G_OPTION_ARG_STRING, &opt_description, N_("A full-paragraph description for this repository"), N_("DESCRIPTION") },
  { "homepage", 0, 0, G_OPTION_ARG_STRING, &opt_homepage, N_("URL for a website for this repository"), N_("URL") },
  { "icon", 0, 0, G_OPTION_ARG_STRING, &opt_icon, N_("URL for an icon for this repository"), N_("URL") },
  { "default-branch", 0, 0, G_OPTION_ARG_STRING, &opt_default_branch, N_("Default branch to use for this repository"), N_("BRANCH") },
  { "collection-id", 0, 0, G_OPTION_ARG_STRING, &opt_collection_id, N_("Collection ID"), N_("COLLECTION-ID") },
  /* Translators: A sideload is when you install from a local USB drive rather than the Internet. */
  { "deploy-sideload-collection-id", 0, 0, G_OPTION_ARG_NONE, &opt_deploy_sideload_collection_id, N_("Permanently deploy collection ID to client remote configurations, only for sideload support"), NULL },
  { "deploy-collection-id", 0, 0, G_OPTION_ARG_NONE, &opt_deploy_collection_id, N_("Permanently deploy collection ID to client remote configurations"), NULL },
  { "authenticator-name", 0, 0, G_OPTION_ARG_STRING, &opt_authenticator_name, N_("Name of authenticator for this repository"), N_("NAME") },
  { "authenticator-install", 0, 0, G_OPTION_ARG_NONE, &opt_authenticator_install, N_("Autoinstall authenticator for this repository"), NULL },
  { "no-authenticator-install", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_authenticator_install, N_("Don't autoinstall authenticator for this repository"), NULL },
  { "authenticator-option", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_authenticator_options, N_("Authenticator option"), N_("KEY=VALUE") },
  { "gpg-import", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_import, N_("Import new default GPG public key from FILE"), N_("FILE") },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the summary with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
  { "sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sign_keys, N_("Key ID to sign the bundle with"), N_("KEY-ID")},
  { "sign-type", 0, 0, G_OPTION_ARG_STRING, &opt_sign_name, N_("Signature type to use for --sign (defaults to 'ed25519')"), N_("NAME")},
  { "generate-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_generate_deltas, N_("Generate delta files"), NULL },
  { "no-update-summary", 0, 0, G_OPTION_ARG_NONE, &opt_no_update_summary, N_("Don't update the summary"), NULL },
  { "no-update-appstream", 0, 0, G_OPTION_ARG_NONE, &opt_no_update_appstream, N_("Don't update the appstream branch"), NULL },
  { "static-delta-jobs", 0, 0, G_OPTION_ARG_INT, &opt_static_delta_jobs, N_("Max parallel jobs when creating deltas (default: NUMCPUs)"), N_("NUM-JOBS") },
  { "static-delta-ignore-ref", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_static_delta_ignore_refs, N_("Don't create deltas matching refs"), N_("PATTERN") },
  { "prune", 0, 0, G_OPTION_ARG_NONE, &opt_prune, N_("Prune unused objects"), NULL },
  { "prune-dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_prune_dry_run, N_("Prune but don't actually remove anything"), NULL },
  { "prune-depth", 0, 0, G_OPTION_ARG_INT, &opt_prune_depth, N_("Only traverse DEPTH parents for each commit (default: -1=infinite)"), N_("DEPTH") },
  { "generate-static-delta-from", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &opt_generate_delta_from, NULL, NULL },
  { "generate-static-delta-to", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &opt_generate_delta_to, NULL, NULL },
  { "generate-static-delta-ref", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &opt_generate_delta_ref, NULL, NULL },
  { "no-summary-index", 0, 0, G_OPTION_ARG_NONE, &opt_no_summary_index, N_("Don't generate a summary index"), NULL },
  { NULL }
};

static void
_ostree_parse_delta_name (const char *delta_name,
                          char      **out_from,
                          char      **out_to)
{
  g_auto(GStrv) parts = g_strsplit (delta_name, "-", 2);

  if (parts[0] && parts[1])
    {
      *out_from = g_steal_pointer (&parts[0]);
      *out_to = g_steal_pointer (&parts[1]);
    }
  else
    {
      *out_from = NULL;
      *out_to = g_steal_pointer (&parts[0]);
    }
}

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

static gboolean
_ostree_repo_static_delta_delete (OstreeRepo   *self,
                                  const char   *delta_id,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  gboolean ret = FALSE;
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;
  g_autofree char *deltadir = NULL;
  struct stat buf;
  int repo_dir_fd = ostree_repo_get_dfd (self);

  _ostree_parse_delta_name (delta_id, &from, &to);
  deltadir = _ostree_get_relative_static_delta_path (from, to, NULL);

  if (fstatat (repo_dir_fd, deltadir, &buf, 0) != 0)
    {
      if (errno == ENOENT)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "Can't find delta %s", delta_id);
      else
        glnx_set_error_from_errno (error);

      goto out;
    }

  if (!glnx_shutil_rm_rf_at (repo_dir_fd, deltadir,
                             cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
generate_one_delta (OstreeRepo   *repo,
                    const char   *from,
                    const char   *to,
                    const char   *ref,
                    GCancellable *cancellable,
                    GError      **error)
{
  g_autoptr(GVariantBuilder) parambuilder = NULL;
  g_autoptr(GVariant) params = NULL;

  parambuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  /* Fall back for 1 meg files */
  g_variant_builder_add (parambuilder, "{sv}",
                         "min-fallback-size", g_variant_new_uint32 (1));
  params = g_variant_ref_sink (g_variant_builder_end (parambuilder));

  if (ref == NULL)
    ref = "";

  if (from == NULL)
    g_print (_("Generating delta: %s (%.10s)\n"), ref, to);
  else
    g_print (_("Generating delta: %s (%.10s-%.10s)\n"), ref, from, to);

  if (!ostree_repo_static_delta_generate (repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                          from, to, NULL,
                                          params,
                                          cancellable, error))
    {
      if (from == NULL)
        g_prefix_error (error, _("Failed to generate delta %s (%.10s): "),
                        ref, to);
      else
        g_prefix_error (error, _("Failed to generate delta %s (%.10s-%.10s): "),
                        ref, from, to);
      return FALSE;
    }

  return TRUE;
}


static void
delta_generation_done (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  int *n_spawned_delta_generate = user_data;

  (*n_spawned_delta_generate)--;
}

static gboolean
spawn_delta_generation (GMainContext *context,
                        int          *n_spawned_delta_generate,
                        OstreeRepo   *repo,
                        GVariant     *params,
                        const char   *ref,
                        const char   *from,
                        const char   *to,
                        GError      **error)
{
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new (0);
  g_autoptr(GSubprocess) subprocess = NULL;
  const char *argv[] = {
    "/proc/self/exe",
    "build-update-repo",
    "--generate-static-delta-ref",
    ref,
    "--generate-static-delta-to",
    to,
    NULL, NULL, NULL, NULL
  };
  int i = 6;
  g_autofree char *exe = NULL;

  exe = flatpak_readlink ("/proc/self/exe", NULL);
  if (exe)
    argv[0] = exe;

  if (from)
    {
      argv[i++] = "--generate-static-delta-from";
      argv[i++] = from;
    }

  argv[i++] = flatpak_file_get_path_cached (ostree_repo_get_path (repo));
  argv[i++] = NULL;

  g_assert (i <= G_N_ELEMENTS (argv));

  while (*n_spawned_delta_generate >= opt_static_delta_jobs)
    g_main_context_iteration (context, TRUE);

  subprocess = g_subprocess_launcher_spawnv (launcher, argv, error);
  if (subprocess == NULL)
    return FALSE;

  (*n_spawned_delta_generate)++;

  g_subprocess_wait_async (subprocess, NULL, delta_generation_done, n_spawned_delta_generate);

  return TRUE;
}

static gboolean
generate_all_deltas (OstreeRepo   *repo,
                     GPtrArray   **unwanted_deltas,
                     GCancellable *cancellable,
                     GError      **error)
{
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) all_deltas_hash = NULL;
  g_autoptr(GHashTable) wanted_deltas_hash = NULL;
  g_autoptr(GPtrArray) all_deltas = NULL;
  int i;
  GHashTableIter iter;
  gpointer key, value;
  g_autoptr(GVariantBuilder) parambuilder = NULL;
  g_autoptr(GVariant) params = NULL;
  int n_spawned_delta_generate = 0;
  g_autoptr(GMainContextPopDefault) context = NULL;
  g_autoptr(GPtrArray) ignore_patterns = g_ptr_array_new_with_free_func ((GDestroyNotify)g_pattern_spec_free);

  g_print ("Generating static deltas\n");

  parambuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  /* Fall back for 1 meg files */
  g_variant_builder_add (parambuilder, "{sv}",
                         "min-fallback-size", g_variant_new_uint32 (1));
  params = g_variant_ref_sink (g_variant_builder_end (parambuilder));

  if (!ostree_repo_list_static_delta_names (repo, &all_deltas,
                                            cancellable, error))
    return FALSE;

  wanted_deltas_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  all_deltas_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (i = 0; i < all_deltas->len; i++)
    g_hash_table_insert (all_deltas_hash,
                         g_strdup (g_ptr_array_index (all_deltas, i)),
                         NULL);

  if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                              cancellable, error))
    return FALSE;

  context = flatpak_main_context_new_default ();

  if (opt_static_delta_ignore_refs != NULL)
    {
      for (i = 0; opt_static_delta_ignore_refs[i] != NULL; i++)
        g_ptr_array_add (ignore_patterns,
                         g_pattern_spec_new (opt_static_delta_ignore_refs[i]));
    }

  g_hash_table_iter_init (&iter, all_refs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *ref = key;
      const char *commit = value;
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) parent_variant = NULL;
      g_autofree char *parent_commit = NULL;
      g_autofree char *grandparent_commit = NULL;
      gboolean ignore_ref = FALSE;

      if (g_str_has_prefix (ref, "app/") || g_str_has_prefix (ref, "runtime/"))
        {
          g_auto(GStrv) parts = g_strsplit (ref, "/", 4);

          for (i = 0; i < ignore_patterns->len; i++)
            {
              GPatternSpec *pattern = g_ptr_array_index(ignore_patterns, i);
              if (g_pattern_match_string (pattern, parts[1]))
                {
                  ignore_ref = TRUE;
                  break;
                }
            }

        }
      else if (g_str_has_prefix (ref, "appstream/"))
        {
          /* Old appstream branch deltas poorly, and most users handle the new format */
          ignore_ref = TRUE;
        }
      else if (g_str_has_prefix (ref, "appstream2/"))
        {
          /* Always delta this */
          ignore_ref = FALSE;
        }
      else
        {
          /* Ignore unknown ref types */
          ignore_ref = TRUE;
        }

      if (ignore_ref)
        {
          g_debug ("Ignoring deltas for ref %s", ref);
          continue;
        }

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit,
                                     &variant, NULL))
        {
          g_warning ("Couldn't load commit %s", commit);
          continue;
        }

      /* From empty */
      if (!g_hash_table_contains (all_deltas_hash, commit))
        {
          if (!spawn_delta_generation (context, &n_spawned_delta_generate, repo, params,
                                       ref, NULL, commit,
                                       error))
            return FALSE;
        }

      /* Mark this one as wanted */
      g_hash_table_insert (wanted_deltas_hash, g_strdup (commit), GINT_TO_POINTER (1));

      parent_commit = ostree_commit_get_parent (variant);

      if (parent_commit != NULL &&
          !ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, parent_commit,
                                     &parent_variant, NULL))
        {
          g_warning ("Couldn't load parent commit %s", parent_commit);
          continue;
        }

      /* From parent */
      if (parent_variant != NULL)
        {
          g_autofree char *from_parent = g_strdup_printf ("%s-%s", parent_commit, commit);

          if (!g_hash_table_contains (all_deltas_hash, from_parent))
            {
              if (!spawn_delta_generation (context, &n_spawned_delta_generate, repo, params,
                                           ref, parent_commit, commit,
                                           error))
                return FALSE;
            }

          /* Mark parent-to-current as wanted */
          g_hash_table_insert (wanted_deltas_hash, g_strdup (from_parent), GINT_TO_POINTER (1));

          /* We also want to keep around the parent and the grandparent-to-parent deltas
           * because otherwise these will be deleted immediately which may cause a race if
           * someone is currently downloading them.
           * However, there is no need to generate these if they don't exist.
           */

          g_hash_table_insert (wanted_deltas_hash, g_strdup (parent_commit), GINT_TO_POINTER (1));
          grandparent_commit = ostree_commit_get_parent (parent_variant);
          if (grandparent_commit != NULL)
            g_hash_table_insert (wanted_deltas_hash,
                                 g_strdup_printf ("%s-%s", grandparent_commit, parent_commit),
                                 GINT_TO_POINTER (1));
        }
    }

  while (n_spawned_delta_generate > 0)
    g_main_context_iteration (context, TRUE);

  *unwanted_deltas = g_ptr_array_new_with_free_func (g_free);
  for (i = 0; i < all_deltas->len; i++)
    {
      const char *delta = g_ptr_array_index (all_deltas, i);
      if (!g_hash_table_contains (wanted_deltas_hash, delta))
        g_ptr_array_add (*unwanted_deltas, g_strdup (delta));
    }

  return TRUE;
}

gboolean
flatpak_builtin_build_update_repo (int argc, char **argv,
                                   GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  g_autoptr(GPtrArray) unwanted_deltas = NULL;

  context = g_option_context_new (_("LOCATION - Update repository metadata"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("LOCATION must be specified"), error);

  if (opt_static_delta_jobs <= 0)
    opt_static_delta_jobs = g_get_num_processors ();

  location = argv[1];

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (opt_generate_delta_to)
    {
      if (!generate_one_delta (repo, opt_generate_delta_from, opt_generate_delta_to, opt_generate_delta_ref, cancellable, error))
        return FALSE;
      return TRUE;
    }

  if (opt_title &&
      !flatpak_repo_set_title (repo, opt_title[0] ? opt_title : NULL, error))
    return FALSE;

  if (opt_comment &&
      !flatpak_repo_set_comment (repo, opt_comment[0] ? opt_comment : NULL, error))
    return FALSE;

  if (opt_description &&
      !flatpak_repo_set_description (repo, opt_description[0] ? opt_description : NULL, error))
    return FALSE;

  if (opt_homepage &&
      !flatpak_repo_set_homepage (repo, opt_homepage[0] ? opt_homepage : NULL, error))
    return FALSE;

  if (opt_icon &&
      !flatpak_repo_set_icon (repo, opt_icon[0] ? opt_icon : NULL, error))
    return FALSE;

  if (opt_redirect_url &&
      !flatpak_repo_set_redirect_url (repo, opt_redirect_url[0] ? opt_redirect_url : NULL, error))
    return FALSE;

  if (opt_default_branch &&
      !flatpak_repo_set_default_branch (repo, opt_default_branch[0] ? opt_default_branch : NULL, error))
    return FALSE;

  if (opt_authenticator_name &&
      !flatpak_repo_set_authenticator_name (repo, opt_authenticator_name[0] ? opt_authenticator_name : NULL, error))
    return FALSE;

  if (opt_authenticator_install != -1 &&
      !flatpak_repo_set_authenticator_install (repo, opt_authenticator_install, error))
    return FALSE;

  if (opt_authenticator_options)
    {
      for (int i = 0; opt_authenticator_options[i] != NULL; i++)
        {
          g_auto(GStrv) split = g_strsplit (opt_authenticator_options[i], "=", 2);
          const char *key = split[0];
          const char *value = NULL;

          if (split[0] != NULL && split[1] != NULL && *split[1] != 0)
            value = split[1];

          if (!flatpak_repo_set_authenticator_option (repo, key, value, error))
            return FALSE;
        }
    }

  if (opt_collection_id != NULL)
    {
      /* Only allow a transition from no collection ID to a non-empty collection ID.
       * Changing the collection ID between two different non-empty values is too
       * dangerous: it will break all clients who have previously pulled from the repository.
       * Require the user to recreate the repository from scratch in that case. */
      const char *old_collection_id = ostree_repo_get_collection_id (repo);
      const char *new_collection_id = opt_collection_id[0] ? opt_collection_id : NULL;

      if (old_collection_id != NULL &&
          g_strcmp0 (old_collection_id, new_collection_id) != 0)
        return flatpak_fail (error, "The collection ID of an existing repository cannot be changed. "
                                    "Recreate the repository to change or clear its collection ID.");

      if (!flatpak_repo_set_collection_id (repo, new_collection_id, error))
        return FALSE;
    }

  if (opt_deploy_sideload_collection_id &&
      !flatpak_repo_set_deploy_sideload_collection_id (repo, TRUE, error))
    return FALSE;

  if (opt_deploy_collection_id &&
      !flatpak_repo_set_deploy_collection_id (repo, TRUE, error))
    return FALSE;

  if (opt_gpg_import)
    {
      g_autoptr(GBytes) gpg_data = flatpak_load_gpg_keys (opt_gpg_import, cancellable, error);
      if (gpg_data == NULL)
        return FALSE;

      if (!flatpak_repo_set_gpg_keys (repo, gpg_data, error))
        return FALSE;
    }

  if (!opt_no_update_appstream)
    {
      g_print (_("Updating appstream branch\n"));
      if (!flatpak_repo_generate_appstream (repo,
                                            (const char **) opt_gpg_key_ids,
                                            opt_gpg_homedir,
                                            (const char **) opt_sign_keys,
                                            opt_sign_name,
                                            0,
                                            cancellable,
                                            error))
        return FALSE;
    }

  if (opt_generate_deltas &&
      !generate_all_deltas (repo, &unwanted_deltas, cancellable, error))
    return FALSE;

  if (unwanted_deltas != NULL)
    {
      int i;
      for (i = 0; i < unwanted_deltas->len; i++)
        {
          const char *delta = g_ptr_array_index (unwanted_deltas, i);
          g_print ("Deleting unwanted delta: %s\n", delta);
          g_autoptr(GError) my_error = NULL;
          if (!_ostree_repo_static_delta_delete (repo, delta, cancellable, &my_error))
            g_printerr ("Unable to delete delta %s: %s\n", delta, my_error->message);
        }
    }

  if (!opt_no_update_summary)
    {
      FlatpakRepoUpdateFlags flags = FLATPAK_REPO_UPDATE_FLAG_NONE;

      if (opt_no_summary_index)
        flags |= FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX;

      g_print (_("Updating summary\n"));
      if (!flatpak_repo_update (repo,
                                flags,
                                (const char **) opt_gpg_key_ids,
                                opt_gpg_homedir,
                                (const char **) opt_sign_keys,
                                opt_sign_name,
                                cancellable,
                                error))
        return FALSE;
    }

  if (opt_prune || opt_prune_dry_run)
    {
      gint n_objects_total;
      gint n_objects_pruned;
      guint64 objsize_total;
      g_autofree char *formatted_freed_size = NULL;

      if (opt_prune_dry_run)
        g_print ("Pruning old commits (dry-run)\n");
      else
        g_print ("Pruning old commits\n");
      if (!flatpak_repo_prune (repo, opt_prune_depth, opt_prune_dry_run,
                              &n_objects_total, &n_objects_pruned, &objsize_total,
                              cancellable, error))
        return FALSE;

      formatted_freed_size = g_format_size_full (objsize_total, 0);

      g_print (_("Total objects: %u\n"), n_objects_total);
      if (n_objects_pruned == 0)
        g_print (_("No unreachable objects\n"));
      else
        g_print (_("Deleted %u objects, %s freed\n"),
                 n_objects_pruned, formatted_freed_size);
    }

  return TRUE;
}


gboolean
flatpak_complete_build_update_repo (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* LOCATION */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;
    }

  return TRUE;
}
