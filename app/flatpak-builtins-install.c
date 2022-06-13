/*
 * Copyright © 2014 Red Hat, Inc
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

#include <gio/gunixinputstream.h>

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-transaction-private.h"
#include "flatpak-cli-transaction.h"
#include "flatpak-quiet-transaction.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include "flatpak-chain-input-stream-private.h"

static char *opt_arch;
static char **opt_gpg_file;
static char **opt_subpaths;
static char **opt_sideload_repos;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_no_related;
static gboolean opt_no_deps;
static gboolean opt_no_static_deltas;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_include_sdk;
static gboolean opt_include_debug;
static gboolean opt_bundle;
static gboolean opt_from;
static gboolean opt_yes;
static gboolean opt_reinstall;
static gboolean opt_noninteractive;
static gboolean opt_or_update;
static gboolean opt_no_auto_pin;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to install for"), N_("ARCH") },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, N_("Don't pull, only install from local cache"), NULL },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, N_("Don't deploy, only download to local cache"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't install related refs"), NULL },
  { "no-deps", 0, 0, G_OPTION_ARG_NONE, &opt_no_deps, N_("Don't verify/install runtime dependencies"), NULL },
  { "no-auto-pin", 0, 0, G_OPTION_ARG_NONE, &opt_no_auto_pin, N_("Don't automatically pin explicit installs"), NULL },
  { "no-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_no_static_deltas, N_("Don't use static deltas"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "include-sdk", 0, 0, G_OPTION_ARG_NONE, &opt_include_sdk, N_("Additionally install the SDK used to build the given refs") },
  { "include-debug", 0, 0, G_OPTION_ARG_NONE, &opt_include_debug, N_("Additionally install the debug info for the given refs and their dependencies") },
  { "bundle", 0, 0, G_OPTION_ARG_NONE, &opt_bundle, N_("Assume LOCATION is a .flatpak single-file bundle"), NULL },
  { "from", 0, 0, G_OPTION_ARG_NONE, &opt_from, N_("Assume LOCATION is a .flatpakref application description"), NULL },
  { "gpg-file", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, N_("Check bundle signatures with GPG key from FILE (- for stdin)"), N_("FILE") },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, N_("Only install this subpath"), N_("PATH") },
  { "assumeyes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes, N_("Automatically answer yes for all questions"), NULL },
  { "reinstall", 0, 0, G_OPTION_ARG_NONE, &opt_reinstall, N_("Uninstall first if already installed"), NULL },
  { "noninteractive", 0, 0, G_OPTION_ARG_NONE, &opt_noninteractive, N_("Produce minimal output and don't ask questions"), NULL },
  { "or-update", 0, 0, G_OPTION_ARG_NONE, &opt_or_update, N_("Update install if already installed"), NULL },
  /* Translators: A sideload is when you install from a local USB drive rather than the Internet. */
  { "sideload-repo", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sideload_repos, N_("Use this local repo for sideloads"), N_("PATH") },
  { NULL }
};

static GBytes *
read_gpg_data (GCancellable *cancellable,
               GError      **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  g_autoptr(GPtrArray) streams = NULL;

  if (opt_gpg_file != NULL)
    n_keyrings = g_strv_length (opt_gpg_file);

  guint ii;

  streams = g_ptr_array_new_with_free_func (g_object_unref);

  for (ii = 0; ii < n_keyrings; ii++)
    {
      GInputStream *input_stream = NULL;

      if (strcmp (opt_gpg_file[ii], "-") == 0)
        {
          input_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
        }
      else
        {
          g_autoptr(GFile) file = g_file_new_for_commandline_arg (opt_gpg_file[ii]);
          input_stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            return NULL;
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) flatpak_chain_input_stream_new (streams);

  return flatpak_read_stream (source_stream, FALSE, error);
}

static gboolean
install_bundle (FlatpakDir *dir,
                GOptionContext *context,
                int argc, char **argv,
                GCancellable *cancellable,
                GError **error)
{
  g_autoptr(GFile) file = NULL;
  const char *filename;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;

  if (argc < 2)
    return usage_error (context, _("Bundle filename must be specified"), error);

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  filename = argv[1];

  file = g_file_new_for_commandline_arg (filename);

  if (!g_file_is_native (file))
    return flatpak_fail (error, _("Remote bundles are not supported"));

  if (opt_gpg_file != NULL)
    {
      /* Override gpg_data from file */
      gpg_data = read_gpg_data (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  if (opt_noninteractive)
    transaction = flatpak_quiet_transaction_new (dir, error);
  else
    transaction = flatpak_cli_transaction_new (dir, opt_yes, TRUE, opt_arch != NULL, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_no_pull (transaction, opt_no_pull);
  flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
  flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
  flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
  flatpak_transaction_set_disable_related (transaction, opt_no_related);
  flatpak_transaction_set_disable_auto_pin (transaction, opt_no_auto_pin);
  flatpak_transaction_set_reinstall (transaction, opt_reinstall);
  flatpak_transaction_set_auto_install_sdk (transaction, opt_include_sdk);
  flatpak_transaction_set_auto_install_debug (transaction, opt_include_debug);

  for (int i = 0; opt_sideload_repos != NULL && opt_sideload_repos[i] != NULL; i++)
    flatpak_transaction_add_sideload_repo (transaction, opt_sideload_repos[i]);

  if (!flatpak_transaction_add_install_bundle (transaction, file, gpg_data, error))
    return FALSE;

  if (!flatpak_transaction_run (transaction, cancellable, error))
    {
      if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
        g_clear_error (error); /* Don't report on stderr */

      return FALSE;
    }

  return TRUE;
}

static gboolean
install_from (FlatpakDir *dir,
              GOptionContext *context,
              int argc, char **argv,
              GCancellable *cancellable,
              GError **error)
{
  g_autoptr(GFile) file = NULL;
  g_autoptr(GBytes) file_data = NULL;
  g_autofree char *data = NULL;
  gsize data_len;
  const char *filename;
  g_autoptr(FlatpakTransaction) transaction = NULL;

  if (argc < 2)
    return usage_error (context, _("Filename or uri must be specified"), error);

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);


  filename = argv[1];

  if (g_str_has_prefix (filename, "http:") ||
      g_str_has_prefix (filename, "https:"))
    {
      g_autoptr(FlatpakHttpSession) http_session = NULL;
      http_session = flatpak_create_http_session (PACKAGE_STRING);
      file_data = flatpak_load_uri (http_session, filename, 0, NULL, NULL, NULL, NULL, cancellable, error);
      if (file_data == NULL)
        {
          g_prefix_error (error, "Can't load uri %s: ", filename);
          return FALSE;
        }
    }
  else
    {
      file = g_file_new_for_commandline_arg (filename);

      if (!g_file_load_contents (file, cancellable, &data, &data_len, NULL, error))
        return FALSE;

      file_data = g_bytes_new_take (g_steal_pointer (&data), data_len);
    }

  if (opt_noninteractive)
    transaction = flatpak_quiet_transaction_new (dir, error);
  else
    transaction = flatpak_cli_transaction_new (dir, opt_yes, TRUE, opt_arch != NULL, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_no_pull (transaction, opt_no_pull);
  flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
  flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
  flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
  flatpak_transaction_set_disable_related (transaction, opt_no_related);
  flatpak_transaction_set_disable_auto_pin (transaction, opt_no_auto_pin);
  flatpak_transaction_set_reinstall (transaction, opt_reinstall);
  flatpak_transaction_set_default_arch (transaction, opt_arch);
  flatpak_transaction_set_auto_install_sdk (transaction, opt_include_sdk);
  flatpak_transaction_set_auto_install_debug (transaction, opt_include_debug);

  for (int i = 0; opt_sideload_repos != NULL && opt_sideload_repos[i] != NULL; i++)
    flatpak_transaction_add_sideload_repo (transaction, opt_sideload_repos[i]);

  if (!flatpak_transaction_add_install_flatpakref (transaction, file_data, error))
    return FALSE;

  if (!flatpak_transaction_run (transaction, cancellable, error))
    {
      if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
        g_clear_error (error); /* Don't report on stderr */

      return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_builtin_install (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autofree char *remote = NULL;
  g_autofree char *remote_url = NULL;
  char **prefs = NULL;
  int i, n_prefs;
  g_autofree char *target_branch = NULL;
  g_autofree char *default_branch = NULL;
  FlatpakKinds kinds;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(FlatpakDir) dir_with_remote = NULL;
  gboolean auto_remote = FALSE;

  context = g_option_context_new (_("[LOCATION/REMOTE] [REF…] - Install applications or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  /* Start with the default or specified dir, this is fine for opt_bundle or opt_from */
  dir = g_object_ref (g_ptr_array_index (dirs, 0));

  if (!opt_bundle && !opt_from && argc >= 2)
    {
      if (flatpak_file_arg_has_suffix (argv[1], ".flatpakref"))
        opt_from = TRUE;
      if (flatpak_file_arg_has_suffix (argv[1], ".flatpak"))
        opt_bundle = TRUE;
    }

  if (opt_bundle)
    return install_bundle (dir, context, argc, argv, cancellable, error);

  if (opt_from)
    return install_from (dir, context, argc, argv, cancellable, error);

  if (argc < 2)
    return usage_error (context, _("At least one REF must be specified"), error);

  if (argc == 2)
    auto_remote = TRUE;

  if (opt_noninteractive)
    opt_yes = TRUE; /* Implied */

  if (opt_include_sdk || opt_include_debug)
    opt_or_update = TRUE;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  if (!opt_noninteractive)
    g_print (_("Looking for matches…\n"));

  if (!auto_remote &&
      (g_path_is_absolute (argv[1]) ||
       g_str_has_prefix (argv[1], "./")))
    {
      g_autoptr(GFile) remote_file = g_file_new_for_commandline_arg (argv[1]);
      remote_url = g_file_get_uri (remote_file);
      remote = g_strdup (remote_url);
    }
  else
    {
      /* If the remote was used, and no single dir was specified, find which
       * one based on the remote. If the remote isn't found assume it's a ref
       * and we should auto-detect the remote. */
      if (!auto_remote)
        {
          g_autoptr(GError) local_error = NULL;
          if (!flatpak_resolve_duplicate_remotes (dirs, argv[1], &dir_with_remote, cancellable, &local_error))
            {
              if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND))
                {
                  auto_remote = TRUE;
                }
              else
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
        }

      if (!auto_remote)
        {
          remote = g_strdup (argv[1]);
          g_clear_object (&dir);
          dir = g_object_ref (dir_with_remote);
        }
      else
        {
          g_autoptr(GPtrArray) remote_dir_pairs = NULL;
          RemoteDirPair *chosen_pair = NULL;

          remote_dir_pairs = g_ptr_array_new_with_free_func ((GDestroyNotify) remote_dir_pair_free);

          /* Search all remotes for a matching ref. This is imperfect
           * because it only takes the first specified ref into account and
           * doesn't distinguish between an exact match and a fuzzy match, but
           * that's okay because the user will be asked to confirm the remote
           */
          for (i = 0; i < dirs->len; i++)
            {
              FlatpakDir *this_dir = g_ptr_array_index (dirs, i);
              g_auto(GStrv) remotes = NULL;
              guint j = 0;
              FindMatchingRefsFlags matching_refs_flags;

              remotes = flatpak_dir_list_remotes (this_dir, cancellable, error);
              if (remotes == NULL)
                return FALSE;

              if (!flatpak_allow_fuzzy_matching (argv[1]))
                matching_refs_flags = FIND_MATCHING_REFS_FLAGS_NONE;
              else
                matching_refs_flags = FIND_MATCHING_REFS_FLAGS_FUZZY;

              for (j = 0; remotes[j] != NULL; j++)
                {
                  const char *this_remote = remotes[j];
                  g_autofree char *this_default_branch = NULL;
                  g_autofree char *id = NULL;
                  g_autofree char *arch = NULL;
                  g_autofree char *branch = NULL;
                  FlatpakKinds matched_kinds;
                  g_autoptr(GPtrArray) refs = NULL;
                  g_autoptr(GError) local_error = NULL;

                  if (flatpak_dir_get_remote_disabled (this_dir, this_remote) ||
                      flatpak_dir_get_remote_noenumerate (this_dir, this_remote))
                    continue;

                  this_default_branch = flatpak_dir_get_remote_default_branch (this_dir, this_remote);

                  flatpak_split_partial_ref_arg_novalidate (argv[1], kinds, opt_arch, target_branch,
                                                            &matched_kinds, &id, &arch, &branch);

                  if (opt_no_pull)
                    refs = flatpak_dir_find_local_refs (this_dir, this_remote, id, branch, this_default_branch, arch,
                                                        flatpak_get_default_arch (),
                                                        matched_kinds, matching_refs_flags,
                                                        cancellable, &local_error);
                  else
                    {
                      g_autoptr(FlatpakRemoteState) state = get_remote_state (this_dir, this_remote, FALSE, FALSE,
                                                                              arch, (const char **)opt_sideload_repos,
                                                                              cancellable, error);
                      if (state == NULL)
                        return FALSE;

                      refs = flatpak_dir_find_remote_refs (this_dir, state, id, branch, this_default_branch, arch,
                                                           flatpak_get_default_arch (),
                                                           matched_kinds, matching_refs_flags,
                                                           cancellable, &local_error);
                      if (refs == NULL)
                        {
                          g_warning ("An error was encountered searching remote ‘%s’ for ‘%s’: %s", this_remote, argv[1], local_error->message);
                          continue;
                        }
                    }

                  if (refs->len == 0)
                    continue;
                  else
                    {
                      RemoteDirPair *pair = remote_dir_pair_new (this_remote, this_dir);
                      g_ptr_array_add (remote_dir_pairs, pair);
                    }
                }
            }

          if (remote_dir_pairs->len == 0)
            return flatpak_fail (error, _("No remote refs found for ‘%s’"), argv[1]);

          if (!flatpak_resolve_matching_remotes (remote_dir_pairs, argv[1], &chosen_pair, error))
            return FALSE;

          remote = g_strdup (chosen_pair->remote_name);
          g_clear_object (&dir);
          dir = g_object_ref (chosen_pair->dir);
        }
    }

  if (auto_remote)
    {
      prefs = &argv[1];
      n_prefs = argc - 1;
    }
  else
    {
      prefs = &argv[2];
      n_prefs = argc - 2;
    }

  /* Backwards compat for old "REMOTE NAME [BRANCH]" argument version */
  if (argc == 4 && flatpak_is_valid_name (argv[2], -1, NULL) && looks_like_branch (argv[3]))
    {
      target_branch = g_strdup (argv[3]);
      n_prefs = 1;
    }

  default_branch = flatpak_dir_get_remote_default_branch (dir, remote);

  if (opt_noninteractive)
    transaction = flatpak_quiet_transaction_new (dir, error);
  else
    transaction = flatpak_cli_transaction_new (dir, opt_yes, TRUE, opt_arch != NULL, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_no_pull (transaction, opt_no_pull);
  flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
  flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
  flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
  flatpak_transaction_set_disable_related (transaction, opt_no_related);
  flatpak_transaction_set_disable_auto_pin (transaction, opt_no_auto_pin);
  flatpak_transaction_set_reinstall (transaction, opt_reinstall);
  flatpak_transaction_set_auto_install_sdk (transaction, opt_include_sdk);
  flatpak_transaction_set_auto_install_debug (transaction, opt_include_debug);

  for (i = 0; opt_sideload_repos != NULL && opt_sideload_repos[i] != NULL; i++)
    flatpak_transaction_add_sideload_repo (transaction, opt_sideload_repos[i]);

  for (i = 0; i < n_prefs; i++)
    {
      const char *pref = prefs[i];
      FlatpakKinds matched_kinds;
      g_autofree char *id = NULL;
      g_autofree char *arch = NULL;
      g_autofree char *branch = NULL;
      g_autofree char *ref = NULL;
      g_autoptr(GPtrArray) refs = NULL;
      g_autoptr(GError) local_error = NULL;
      FindMatchingRefsFlags matching_refs_flags;

      if (!flatpak_allow_fuzzy_matching (pref))
        matching_refs_flags = FIND_MATCHING_REFS_FLAGS_NONE;
      else
        matching_refs_flags = FIND_MATCHING_REFS_FLAGS_FUZZY;

      if (matching_refs_flags & FIND_MATCHING_REFS_FLAGS_FUZZY)
        {
          flatpak_split_partial_ref_arg_novalidate (pref, kinds, opt_arch, target_branch,
                                                    &matched_kinds, &id, &arch, &branch);

          /* We used _novalidate so that the id can be partial, but we can still validate the branch */
          if (branch != NULL && !flatpak_is_valid_branch (branch, -1, &local_error))
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF,
                                       _("Invalid branch %s: %s"), branch, local_error->message);
        }
      else if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, target_branch,
                                               &matched_kinds, &id, &arch, &branch, error))
        {
          return FALSE;
        }

      if (opt_no_pull)
        refs = flatpak_dir_find_local_refs (dir, remote, id, branch, default_branch, arch,
                                            flatpak_get_default_arch (),
                                            matched_kinds, matching_refs_flags,
                                            cancellable, error);
      else
        {
          g_autoptr(FlatpakRemoteState) state = NULL;

          state = flatpak_transaction_ensure_remote_state (transaction, FLATPAK_TRANSACTION_OPERATION_INSTALL,
                                                           remote, arch, error);
          if (state == NULL)
            return FALSE;

          refs = flatpak_dir_find_remote_refs (dir, state, id, branch, default_branch, arch,
                                               flatpak_get_default_arch (),
                                               matched_kinds, matching_refs_flags,
                                               cancellable, error);
          if (refs == NULL)
            return FALSE;
        }

      if (refs->len == 0)
        {
          if (opt_no_pull)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("Nothing matches %s in local repository for remote %s"), id, remote);
          else
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("Nothing matches %s in remote %s"), id, remote);
          return FALSE;
        }

      if (!flatpak_resolve_matching_refs (remote, dir, opt_yes, refs, id, &ref, error))
        return FALSE;

      if (!flatpak_transaction_add_install (transaction, remote, ref, (const char **) opt_subpaths, &local_error))
        {
          if (!g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          if (opt_or_update)
            {
              if (!flatpak_transaction_add_update (transaction, ref, (const char **) opt_subpaths, NULL, error))
                return FALSE;
            }
          else
            g_printerr (_("Skipping: %s\n"), local_error->message);
        }
    }

  if (!flatpak_transaction_run (transaction, cancellable, error))
    {
      if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
        g_clear_error (error); /* Don't report on stderr */

      return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_install (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  FlatpakKinds kinds;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  switch (completion->argc)
    {
    case 0:
    case 1: /* LOCATION/REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      flatpak_complete_file (completion, "__FLATPAK_BUNDLE_OR_REF_FILE");

      {
        g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
        if (remotes != NULL)
          {
            for (i = 0; remotes[i] != NULL; i++)
              flatpak_complete_word (completion, "%s ", remotes[i]);
          }
      }

      break;

    default: /* REF */
      flatpak_complete_partial_ref (completion, kinds, opt_arch, dir, completion->argv[1]);
      break;
    }

  return TRUE;
}
