/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2024 Red Hat, Inc
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
 *       Kalev Lember <klember@redhat.com>
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
#include "flatpak-utils-http-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include "flatpak-chain-input-stream-private.h"

static char **opt_sideload_repos;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_no_related;
static gboolean opt_no_deps;
static gboolean opt_no_static_deltas;
static gboolean opt_include_sdk;
static gboolean opt_include_debug;
static gboolean opt_yes;
static gboolean opt_reinstall;
static gboolean opt_noninteractive;

static GOptionEntry options[] = {
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, N_("Don't pull, only install from local cache"), NULL },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, N_("Don't deploy, only download to local cache"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't install related refs"), NULL },
  { "no-deps", 0, 0, G_OPTION_ARG_NONE, &opt_no_deps, N_("Don't verify/install runtime dependencies"), NULL },
  { "no-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_no_static_deltas, N_("Don't use static deltas"), NULL },
  { "include-sdk", 0, 0, G_OPTION_ARG_NONE, &opt_include_sdk, N_("Additionally install the SDK used to build the given refs") },
  { "include-debug", 0, 0, G_OPTION_ARG_NONE, &opt_include_debug, N_("Additionally install the debug info for the given refs and their dependencies") },
  { "assumeyes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes, N_("Automatically answer yes for all questions"), NULL },
  { "reinstall", 0, 0, G_OPTION_ARG_NONE, &opt_reinstall, N_("Uninstall first if already installed"), NULL },
  { "noninteractive", 0, 0, G_OPTION_ARG_NONE, &opt_noninteractive, N_("Produce minimal output and don't ask questions"), NULL },
  /* Translators: A sideload is when you install from a local USB drive rather than the Internet. */
  { "sideload-repo", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sideload_repos, N_("Use this local repo for sideloads"), N_("PATH") },
  { NULL }
};

gboolean
flatpak_builtin_preinstall (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;

  context = g_option_context_new (_("- Install flatpaks that are part of the operating system"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  /* Use the default dir */
  dir = g_object_ref (g_ptr_array_index (dirs, 0));

  if (opt_noninteractive)
    opt_yes = TRUE; /* Implied */

  if (opt_noninteractive)
    transaction = flatpak_quiet_transaction_new (dir, error);
  else
    transaction = flatpak_cli_transaction_new (dir, opt_yes, TRUE, FALSE, error);
  if (transaction == NULL)
    return FALSE;

  flatpak_transaction_set_no_pull (transaction, opt_no_pull);
  flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
  flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
  flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
  flatpak_transaction_set_disable_related (transaction, opt_no_related);
  flatpak_transaction_set_reinstall (transaction, opt_reinstall);
  flatpak_transaction_set_auto_install_sdk (transaction, opt_include_sdk);
  flatpak_transaction_set_auto_install_debug (transaction, opt_include_debug);

  for (int i = 0; opt_sideload_repos != NULL && opt_sideload_repos[i] != NULL; i++)
    flatpak_transaction_add_sideload_repo (transaction, opt_sideload_repos[i]);

  if (!flatpak_transaction_add_sync_preinstalled (transaction, error))
    return FALSE;

  if (flatpak_transaction_is_empty (transaction))
    {
      g_print (_("Nothing to do.\n"));

      return TRUE;
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
flatpak_complete_preinstall (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    default: /* REF */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);
      break;
    }

  return TRUE;
}
