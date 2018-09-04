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

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-cli-transaction.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include "flatpak-chain-input-stream-private.h"

static char *opt_arch;
static char **opt_gpg_file;
static char **opt_subpaths;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_no_related;
static gboolean opt_no_deps;
static gboolean opt_no_static_deltas;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_bundle;
static gboolean opt_from;
static gboolean opt_yes;
static gboolean opt_reinstall;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to install for"), N_("ARCH") },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, N_("Don't pull, only install from local cache"), NULL },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, N_("Don't deploy, only download to local cache"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't install related refs"), NULL },
  { "no-deps", 0, 0, G_OPTION_ARG_NONE, &opt_no_deps, N_("Don't verify/install runtime dependencies"), NULL },
  { "no-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_no_static_deltas, N_("Don't use static deltas"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "bundle", 0, 0, G_OPTION_ARG_NONE, &opt_bundle, N_("Assume LOCATION is a .flatpak single-file bundle"), NULL },
  { "from", 0, 0, G_OPTION_ARG_NONE, &opt_from, N_("Assume LOCATION is a .flatpakref application description"), NULL },
  { "gpg-file", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, N_("Check bundle signatures with GPG key from FILE (- for stdin)"), N_("FILE") },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, N_("Only install this subpath"), N_("PATH") },
  { "assumeyes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes, N_("Automatically answer yes for all questions"), NULL },
  { "reinstall", 0, 0, G_OPTION_ARG_NONE, &opt_reinstall, N_("Uninstall first if already installed"), NULL },
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

  transaction = flatpak_cli_transaction_new (dir, opt_yes, TRUE, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_no_pull (transaction, opt_no_pull);
  flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
  flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
  flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
  flatpak_transaction_set_disable_related (transaction, opt_no_related);
  flatpak_transaction_set_reinstall (transaction, opt_reinstall);

  if (!flatpak_transaction_add_install_bundle (transaction, file, gpg_data, error))
    return FALSE;

  if (!flatpak_cli_transaction_run (transaction, cancellable, error))
    return FALSE;

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
      file_data = download_uri (filename, error);
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

  transaction = flatpak_cli_transaction_new (dir, opt_yes, TRUE, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_no_pull (transaction, opt_no_pull);
  flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
  flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
  flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
  flatpak_transaction_set_disable_related (transaction, opt_no_related);
  flatpak_transaction_set_reinstall (transaction, opt_reinstall);
  flatpak_transaction_set_default_arch (transaction, opt_arch);

  if (!flatpak_transaction_add_install_flatpakref (transaction, file_data, error))
    return FALSE;

  if (!flatpak_cli_transaction_run (transaction, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_install (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  const char *remote;
  g_autofree char *remote_url = NULL;
  char **prefs = NULL;
  int i, n_prefs;
  g_autofree char *target_branch = NULL;
  g_autofree char *default_branch = NULL;
  FlatpakKinds kinds;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(FlatpakDir) dir_with_remote = NULL;

  context = g_option_context_new (_("LOCATION/REMOTE [REF...] - Install applications or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS,
                                     &dirs, cancellable, error))
    return FALSE;

  /* Start with the default or specified dir, this is fine for opt_bundle or opt_from */
  dir = g_ptr_array_index (dirs, 0);

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

  if (argc < 3)
    return usage_error (context, _("REMOTE and REF must be specified"), error);

  if (g_path_is_absolute (argv[1]) ||
      g_str_has_prefix (argv[1], "./"))
    {
      g_autoptr(GFile) remote_file = g_file_new_for_commandline_arg (argv[1]);
      remote_url = g_file_get_uri (remote_file);
      remote = remote_url;
    }
  else
    {
      remote = argv[1];

      /* If the remote was used, and no single dir was specified, find which one based on the remote */
      if (dirs->len > 1)
        {
          if (!flatpak_resolve_duplicate_remotes (dirs, remote, &dir_with_remote, cancellable, error))
            return FALSE;
          dir = dir_with_remote;
        }
    }

  prefs = &argv[2];
  n_prefs = argc - 2;

  /* Backwards compat for old "REMOTE NAME [BRANCH]" argument version */
  if (argc == 4 && looks_like_branch (argv[3]))
    {
      target_branch = g_strdup (argv[3]);
      n_prefs = 1;
    }

  default_branch = flatpak_dir_get_remote_default_branch (dir, remote);
  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  transaction = flatpak_cli_transaction_new (dir, opt_yes, TRUE, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_no_pull (transaction, opt_no_pull);
  flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
  flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
  flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
  flatpak_transaction_set_disable_related (transaction, opt_no_related);
  flatpak_transaction_set_reinstall (transaction, opt_reinstall);

  for (i = 0; i < n_prefs; i++)
    {
      const char *pref = prefs[i];
      FlatpakKinds matched_kinds;
      g_autofree char *id = NULL;
      g_autofree char *arch = NULL;
      g_autofree char *branch = NULL;
      FlatpakKinds kind;
      g_autofree char *ref = NULL;

      if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, target_branch,
                                          &matched_kinds, &id, &arch, &branch, error))
        return FALSE;


      if (opt_no_pull)
        ref = flatpak_dir_find_local_ref (dir, remote, id, branch, default_branch, arch,
                                          matched_kinds, &kind, cancellable, error);
      else
        ref = flatpak_dir_find_remote_ref (dir, remote, id, branch, default_branch, arch,
                                           matched_kinds, &kind, cancellable, error);

      if (ref == NULL)
        return FALSE;

      if (!flatpak_cli_transaction_add_install (transaction, remote, ref, (const char **) opt_subpaths, error))
        return FALSE;
    }

  if (!flatpak_cli_transaction_run (transaction, cancellable, error))
    return FALSE;

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
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR, &dirs, NULL, NULL))
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
