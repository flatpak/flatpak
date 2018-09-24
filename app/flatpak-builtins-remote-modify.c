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

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"

static gboolean opt_no_gpg_verify;
static gboolean opt_do_gpg_verify;
static gboolean opt_do_enumerate;
static gboolean opt_no_enumerate;
static gboolean opt_do_deps;
static gboolean opt_no_deps;
static gboolean opt_enable;
static gboolean opt_update_metadata;
static gboolean opt_disable;
static int opt_prio = -1;
static char *opt_title;
static char *opt_default_branch;
static char *opt_url;
static char *opt_collection_id = NULL;
static char **opt_gpg_import;


static GOptionEntry modify_options[] = {
  { "gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_do_gpg_verify, N_("Enable GPG verification"), NULL },
  { "enumerate", 0, 0, G_OPTION_ARG_NONE, &opt_do_enumerate, N_("Mark the remote as enumerate"), NULL },
  { "use-for-deps", 0, 0, G_OPTION_ARG_NONE, &opt_do_deps, N_("Mark the remote as used for dependencies"), NULL },
  { "url", 0, 0, G_OPTION_ARG_STRING, &opt_url, N_("Set a new url"), N_("URL") },
  { "enable", 0, 0, G_OPTION_ARG_NONE, &opt_enable, N_("Enable the remote"), NULL },
  { "update-metadata", 0, 0, G_OPTION_ARG_NONE, &opt_update_metadata, N_("Update extra metadata from the summary file"), NULL },
  { NULL }
};

static GOptionEntry common_options[] = {
  { "no-gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_gpg_verify, N_("Disable GPG verification"), NULL },
  { "no-enumerate", 0, 0, G_OPTION_ARG_NONE, &opt_no_enumerate, N_("Mark the remote as don't enumerate"), NULL },
  { "no-use-for-deps", 0, 0, G_OPTION_ARG_NONE, &opt_no_deps, N_("Mark the remote as don't use for deps"), NULL },
  { "prio", 0, 0, G_OPTION_ARG_INT, &opt_prio, N_("Set priority (default 1, higher is more prioritized)"), N_("PRIORITY") },
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, N_("A nice name to use for this remote"), N_("TITLE") },
  { "default-branch", 0, 0, G_OPTION_ARG_STRING, &opt_default_branch, N_("Default branch to use for this remote"), N_("BRANCH") },
  { "collection-id", 0, 0, G_OPTION_ARG_STRING, &opt_collection_id, N_("Collection ID"), N_("COLLECTION-ID") },
  { "gpg-import", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_import, N_("Import GPG key from FILE (- for stdin)"), N_("FILE") },
  { "disable", 0, 0, G_OPTION_ARG_NONE, &opt_disable, N_("Disable the remote"), NULL },
  { NULL }
};


static GKeyFile *
get_config_from_opts (FlatpakDir *dir, const char *remote_name, gboolean *changed)
{
  OstreeRepo *repo;
  GKeyFile *config;
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  repo = flatpak_dir_get_repo (dir);
  if (repo == NULL)
    config = g_key_file_new ();
  else
    config = ostree_repo_copy_config (repo);

  if (opt_no_gpg_verify)
    {
      g_key_file_set_boolean (config, group, "gpg-verify", FALSE);
      g_key_file_set_boolean (config, group, "gpg-verify-summary", FALSE);
      *changed = TRUE;
    }

  if (opt_do_gpg_verify)
    {
      g_key_file_set_boolean (config, group, "gpg-verify", TRUE);
      g_key_file_set_boolean (config, group, "gpg-verify-summary", TRUE);
      *changed = TRUE;
    }

  if (opt_url)
    {
      if (g_str_has_prefix (opt_url, "metalink="))
        g_key_file_set_string (config, group, "metalink", opt_url + strlen ("metalink="));
      else
        g_key_file_set_string (config, group, "url", opt_url);
      *changed = TRUE;
    }

  if (opt_collection_id)
    {
      g_key_file_set_string (config, group, "collection-id", opt_collection_id);
      g_key_file_set_boolean (config, group, "gpg-verify-summary", FALSE);
      *changed = TRUE;
    }

  if (opt_title)
    {
      g_key_file_set_string (config, group, "xa.title", opt_title);
      g_key_file_set_boolean (config, group, "xa.title-is-set", TRUE);
      *changed = TRUE;
    }

  if (opt_default_branch)
    {
      g_key_file_set_string (config, group, "xa.default-branch", opt_default_branch);
      g_key_file_set_boolean (config, group, "xa.default-branch-is-set", TRUE);
      *changed = TRUE;
    }

  if (opt_no_enumerate)
    {
      g_key_file_set_boolean (config, group, "xa.noenumerate", TRUE);
      *changed = TRUE;
    }

  if (opt_do_enumerate)
    {
      g_key_file_set_boolean (config, group, "xa.noenumerate", FALSE);
      *changed = TRUE;
    }

  if (opt_no_deps)
    {
      g_key_file_set_boolean (config, group, "xa.nodeps", TRUE);
      *changed = TRUE;
    }

  if (opt_do_deps)
    {
      g_key_file_set_boolean (config, group, "xa.nodeps", FALSE);
      *changed = TRUE;
    }

  if (opt_disable)
    {
      g_key_file_set_boolean (config, group, "xa.disable", TRUE);
      *changed = TRUE;
    }
  else if (opt_enable)
    {
      g_key_file_set_boolean (config, group, "xa.disable", FALSE);
      *changed = TRUE;
    }

  if (opt_prio != -1)
    {
      g_autofree char *prio_as_string = g_strdup_printf ("%d", opt_prio);
      g_key_file_set_string (config, group, "xa.prio", prio_as_string);
      *changed = TRUE;
    }

  return config;
}

gboolean
flatpak_builtin_remote_modify (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(FlatpakDir) preferred_dir = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  const char *remote_name;
  gboolean changed = FALSE;

  context = g_option_context_new (_("NAME - Modify a remote repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  g_option_context_add_main_entries (context, common_options, NULL);

  if (!flatpak_option_context_parse (context, modify_options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("Remote NAME must be specified"), error);

  remote_name = argv[1];

  if (!flatpak_resolve_duplicate_remotes (dirs, remote_name, &preferred_dir, cancellable, error))
    return FALSE;

  if (opt_update_metadata)
    {
      g_autoptr(GError) local_error = NULL;

      g_print (_("Updating extra metadata from remote summary for %s\n"), remote_name);
      if (!flatpak_dir_update_remote_configuration (preferred_dir, remote_name, cancellable, &local_error))
        {
          g_printerr (_("Error updating extra metadata for '%s': %s\n"), remote_name, local_error->message);
          return flatpak_fail (error, _("Could not update extra metadata for %s"), remote_name);
        }

      /* Reload changed configuration */
      if (!flatpak_dir_recreate_repo (preferred_dir, cancellable, error))
        return FALSE;
    }

  config = get_config_from_opts (preferred_dir, remote_name, &changed);

  if (opt_gpg_import != NULL)
    {
      gpg_data = flatpak_load_gpg_keys (opt_gpg_import, cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
      changed = TRUE;
    }

  if (!changed)
    return TRUE;

  return flatpak_dir_modify_remote (preferred_dir, remote_name, config, gpg_data, cancellable, error);
}

gboolean
flatpak_complete_remote_modify (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;

  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, common_options, NULL);
  if (!flatpak_option_context_parse (context, modify_options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, common_options);
      flatpak_complete_options (completion, modify_options);
      flatpak_complete_options (completion, user_entries);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          int j;
          g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
          if (remotes == NULL)
            return FALSE;
          for (j = 0; remotes[j] != NULL; j++)
            flatpak_complete_word (completion, "%s ", remotes[j]);
        }

      break;
    }

  return TRUE;
}
