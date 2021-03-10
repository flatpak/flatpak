/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"

static gboolean opt_no_sign_verify;
static gboolean opt_do_gpg_verify;
static gboolean opt_do_enumerate;
static gboolean opt_no_enumerate;
static gboolean opt_do_deps;
static gboolean opt_no_deps;
static gboolean opt_if_not_exists;
static gboolean opt_disable;
static int opt_prio = -1;
static char *opt_filter;
static char *opt_title;
static char *opt_comment;
static char *opt_description;
static char *opt_homepage;
static char *opt_icon;
static char *opt_subset;
static char *opt_default_branch;
static char *opt_url;
static char *opt_collection_id = NULL;
static gboolean opt_from;
static char **opt_gpg_import;
static char **opt_sign_keys;
static char *opt_authenticator_name = NULL;
static char **opt_authenticator_options = NULL;
static gboolean opt_authenticator_install = -1;
static gboolean opt_no_follow_redirect;

static GOptionEntry add_options[] = {
  { "if-not-exists", 0, 0, G_OPTION_ARG_NONE, &opt_if_not_exists, N_("Do nothing if the provided remote exists"), NULL },
  { "from", 0, 0, G_OPTION_ARG_NONE, &opt_from, N_("LOCATION specifies a configuration file, not the repo location"), NULL },
  { NULL }
};

static GOptionEntry common_options[] = {
  { "no-sign-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_sign_verify, N_("Disable signature verification"), NULL },
  { "no-gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_sign_verify, N_("Deprecated alternative to --no-sign-verify"), NULL },
  { "no-enumerate", 0, 0, G_OPTION_ARG_NONE, &opt_no_enumerate, N_("Mark the remote as don't enumerate"), NULL },
  { "no-use-for-deps", 0, 0, G_OPTION_ARG_NONE, &opt_no_deps, N_("Mark the remote as don't use for deps"), NULL },
  { "prio", 0, 0, G_OPTION_ARG_INT, &opt_prio, N_("Set priority (default 1, higher is more prioritized)"), N_("PRIORITY") },
  { "subset", 0, 0, G_OPTION_ARG_STRING, &opt_subset, N_("The named subset to use for this remote"), N_("SUBSET") },
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, N_("A nice name to use for this remote"), N_("TITLE") },
  { "comment", 0, 0, G_OPTION_ARG_STRING, &opt_comment, N_("A one-line comment for this remote"), N_("COMMENT") },
  { "description", 0, 0, G_OPTION_ARG_STRING, &opt_description, N_("A full-paragraph description for this remote"), N_("DESCRIPTION") },
  { "homepage", 0, 0, G_OPTION_ARG_STRING, &opt_homepage, N_("URL for a website for this remote"), N_("URL") },
  { "icon", 0, 0, G_OPTION_ARG_STRING, &opt_icon, N_("URL for an icon for this remote"), N_("URL") },
  { "default-branch", 0, 0, G_OPTION_ARG_STRING, &opt_default_branch, N_("Default branch to use for this remote"), N_("BRANCH") },
  { "collection-id", 0, 0, G_OPTION_ARG_STRING, &opt_collection_id, N_("Collection ID"), N_("COLLECTION-ID") },
  { "gpg-import", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_import, N_("Import GPG key from FILE (- for stdin)"), N_("FILE") },
  { "sign-verify", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sign_keys, N_("Verify signatures using KEYTYPE=inline:PUBKEY or KEYTYPE=file:/path/to/key"), N_("KEYTYPE=[inline|file]:PUBKEY") },
  { "filter", 0, 0, G_OPTION_ARG_FILENAME, &opt_filter, N_("Set path to local filter FILE"), N_("FILE") },
  { "disable", 0, 0, G_OPTION_ARG_NONE, &opt_disable, N_("Disable the remote"), NULL },
  { "authenticator-name", 0, 0, G_OPTION_ARG_STRING, &opt_authenticator_name, N_("Name of authenticator"), N_("NAME") },
  { "authenticator-option", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_authenticator_options, N_("Authenticator option"), N_("KEY=VALUE") },
  { "authenticator-install", 0, 0, G_OPTION_ARG_NONE, &opt_authenticator_install, N_("Autoinstall authenticator"), NULL },
  { "no-authenticator-install", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_authenticator_install, N_("Don't autoinstall authenticator"), NULL },
  { "no-follow_redirect", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_no_follow_redirect, N_("Don't follow the redirect set in the summary file"), NULL },
  { NULL }
};


static gboolean
get_config_from_opts (GKeyFile *config,
                      const char *remote_name,
                      GBytes    **gpg_data,
                      GError    **error)
{
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  if (opt_no_sign_verify)
    {
      g_key_file_set_boolean (config, group, "gpg-verify", FALSE);
      g_key_file_set_boolean (config, group, "gpg-verify-summary", FALSE);
      g_key_file_set_boolean (config, group, "sign-verify", FALSE);
      g_key_file_set_boolean (config, group, "sign-verify-summary", FALSE);
    }
  else
    {
      g_autofree gchar *verify = g_key_file_get_string (config, group, "sign-verify", NULL);
      g_autoptr(GPtrArray) sign_verify = g_ptr_array_new_with_free_func (g_free);
      char **iter;

      /*
       * `sign-verify` can be either a boolean, or a string representing the
       * signature type. In the latter case, this means it's enabled, so we
       * have to read the full string and check it doesn't translate to a
       * boolean false.
       */
      if (verify && !g_str_equal (verify, "false") && !g_str_equal (verify, "0"))
        {
          g_autofree char **verify_names = g_strsplit (verify, ",", -1);
          for (iter = verify_names; iter && *iter; iter++)
            g_ptr_array_add (sign_verify, g_steal_pointer (iter));
        }

      for (iter = opt_sign_keys; iter && *iter; iter++)
        {
          g_autofree char *signname = flatpak_verify_add_config_options (config, group, *iter, error);
          if (!signname)
            return FALSE;
          else if (!g_ptr_array_find_with_equal_func (sign_verify, signname, g_str_equal, NULL))
            g_ptr_array_add (sign_verify, g_steal_pointer (&signname));
        }

      if (sign_verify->len > 0)
        {
          g_autofree char *sign_verify_string = NULL;

          g_ptr_array_add (sign_verify, NULL);
          sign_verify_string = g_strjoinv (",", (char **)sign_verify->pdata);

          g_key_file_set_string (config, group, "sign-verify", sign_verify_string);
          g_key_file_set_boolean (config, group, "sign-verify-summary", TRUE);

          /* Ensure that these don't get automatically enabled from the remote's
             own configuration. */
          if (!opt_do_gpg_verify)
            {
              g_key_file_set_boolean (config, group, "gpg-verify", FALSE);
              g_key_file_set_boolean (config, group, "gpg-verify-summary", FALSE);
            }
        }
      else
        {
          g_key_file_set_boolean (config, group, "sign-verify", FALSE);
          g_key_file_set_boolean (config, group, "sign-verify-summary", FALSE);
        }
    }

  if (opt_do_gpg_verify)
    {
      g_key_file_set_boolean (config, group, "gpg-verify", TRUE);
      g_key_file_set_boolean (config, group, "gpg-verify-summary", TRUE);
    }

  if (opt_url)
    {
      if (g_str_has_prefix (opt_url, "metalink="))
        g_key_file_set_string (config, group, "metalink", opt_url + strlen ("metalink="));
      else
        g_key_file_set_string (config, group, "url", opt_url);
    }

  if (opt_collection_id)
    g_key_file_set_string (config, group, "collection-id", opt_collection_id);

  if (opt_subset)
    {
      g_key_file_set_string (config, group, "xa.subset", opt_subset);
      g_key_file_set_boolean (config, group, "xa.subset-is-set", TRUE);
    }

  if (opt_title)
    {
      g_key_file_set_string (config, group, "xa.title", opt_title);
      g_key_file_set_boolean (config, group, "xa.title-is-set", TRUE);
    }

  if (opt_comment)
    {
      g_key_file_set_string (config, group, "xa.comment", opt_comment);
      g_key_file_set_boolean (config, group, "xa.comment-is-set", TRUE);
    }

  if (opt_description)
    {
      g_key_file_set_string (config, group, "xa.description", opt_description);
      g_key_file_set_boolean (config, group, "xa.description-is-set", TRUE);
    }

  if (opt_homepage)
    {
      g_key_file_set_string (config, group, "xa.homepage", opt_homepage);
      g_key_file_set_boolean (config, group, "xa.homepage-is-set", TRUE);
    }

  if (opt_icon)
    {
      g_key_file_set_string (config, group, "xa.icon", opt_icon);
      g_key_file_set_boolean (config, group, "xa.icon-is-set", TRUE);
    }

  if (opt_default_branch)
    {
      g_key_file_set_string (config, group, "xa.default-branch", opt_default_branch);
      g_key_file_set_boolean (config, group, "xa.default-branch-is-set", TRUE);
    }

  if (opt_filter)
    g_key_file_set_string (config, group, "xa.filter", opt_filter);

  if (opt_no_enumerate)
    g_key_file_set_boolean (config, group, "xa.noenumerate", TRUE);

  if (opt_do_enumerate)
    g_key_file_set_boolean (config, group, "xa.noenumerate", FALSE);

  if (opt_no_deps)
    g_key_file_set_boolean (config, group, "xa.nodeps", TRUE);

  if (opt_do_deps)
    g_key_file_set_boolean (config, group, "xa.nodeps", FALSE);

  if (opt_disable)
    g_key_file_set_boolean (config, group, "xa.disable", TRUE);

  if (opt_prio != -1)
    {
      g_autofree char *prio_as_string = g_strdup_printf ("%d", opt_prio);
      g_key_file_set_string (config, group, "xa.prio", prio_as_string);
    }

  if (opt_gpg_import != NULL)
    {
      g_clear_pointer (gpg_data, g_bytes_unref); /* Free if set from flatpakrepo file */
      *gpg_data = flatpak_load_gpg_keys (opt_gpg_import, NULL, error);
      if (*gpg_data == NULL)
        return FALSE;
    }

  if (opt_authenticator_name)
    {
      g_key_file_set_string (config, group, "xa.authenticator-name", opt_authenticator_name);
      g_key_file_set_boolean (config, group, "xa.authenticator-name-is-set", TRUE);
    }

  if (opt_authenticator_install != -1)
    {
      g_key_file_set_boolean (config, group, "xa.authenticator-install", opt_authenticator_install);
      g_key_file_set_boolean (config, group, "xa.authenticator-install-is-set", TRUE);
    }

  if (opt_authenticator_options)
    {
      for (int i = 0; opt_authenticator_options[i] != NULL; i++)
        {
          g_auto(GStrv) split = g_strsplit (opt_authenticator_options[i], "=", 2);
          g_autofree char *key = g_strdup_printf ("xa.authenticator-options.%s", split[0]);

          if (split[0] == NULL || split[1] == NULL || *split[1] == 0)
            g_key_file_remove_key (config, group, key, NULL);
          else
            g_key_file_set_string (config, group, key, split[1]);
        }
    }

  if (opt_no_follow_redirect)
    g_key_file_set_boolean (config, group, "url-is-set", TRUE);

  return TRUE;
}

static GKeyFile *
load_options (const char *remote_name,
              const char *filename,
              GBytes    **gpg_data,
              GError    **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) bytes = NULL;

  if (g_str_has_prefix (filename, "http:") ||
      g_str_has_prefix (filename, "https:") ||
      g_str_has_prefix (filename, "file:"))
    {
      const char *options_data;
      gsize options_size;
      g_autoptr(FlatpakHttpSession) http_session = NULL;

      http_session = flatpak_create_http_session (PACKAGE_STRING);
      bytes = flatpak_load_uri (http_session, filename, 0, NULL, NULL, NULL, NULL, NULL, &local_error);

      if (bytes == NULL)
        {
          flatpak_fail (error, _("Can't load uri %s: %s\n"), filename, local_error->message);
          return NULL;
        }

      options_data = g_bytes_get_data (bytes, &options_size);
      if (!g_key_file_load_from_data (keyfile, options_data, options_size, 0, &local_error))
        {
          flatpak_fail (error, _("Can't load uri %s: %s\n"), filename, local_error->message);
          return NULL;
        }
    }
  else
    {
      if (!g_key_file_load_from_file (keyfile, filename, 0, &local_error))
        {
          flatpak_fail (error, _("Can't load file %s: %s\n"), filename, local_error->message);
          return NULL;
        }
    }

  config = flatpak_parse_repofile (remote_name, FALSE, keyfile, gpg_data, NULL, error);
  if (config == NULL)
    return NULL;

  return g_steal_pointer (&config);
}

gboolean
flatpak_builtin_remote_add (int argc, char **argv,
                            GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  g_autoptr(GFile) file = NULL;
  g_auto(GStrv) remotes = NULL;
  g_autofree char *remote_url = NULL;
  const char *remote_name;
  const char *location = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autoptr(GError) local_error = NULL;

  context = g_option_context_new (_("NAME LOCATION - Add a remote repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  g_option_context_add_main_entries (context, common_options, NULL);

  if (!flatpak_option_context_parse (context, add_options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR,
                                     &dirs, cancellable, error))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  if (argc < 2)
    return usage_error (context, _("NAME must be specified"), error);

  if (argc < 3)
    return usage_error (context, _("LOCATION must be specified"), error);

  if (argc > 3)
    return usage_error (context, _("Too many arguments"), error);

  if (opt_collection_id != NULL &&
      !ostree_validate_collection_id (opt_collection_id, &local_error))
    return flatpak_fail (error, _("‘%s’ is not a valid collection ID: %s"), opt_collection_id, local_error->message);

  if (opt_collection_id != NULL &&
      (opt_no_sign_verify
        || ((opt_gpg_import == NULL || opt_gpg_import[0] == NULL)
            && (opt_sign_keys == NULL || opt_sign_keys[0] == NULL))))
    return flatpak_fail (error, _("Signature verification is required if collections are enabled"));

  remote_name = argv[1];
  location = argv[2];

  if (opt_from ||
      flatpak_file_arg_has_suffix (location, ".flatpakrepo"))
    {
      config = load_options (remote_name, location, &gpg_data, error);
      if (config == NULL)
        return FALSE;
    }
  else
    {
      gboolean is_oci;

      config = g_key_file_new ();
      file = g_file_new_for_commandline_arg (location);
      if (g_file_is_native (file))
        remote_url = g_file_get_uri (file);
      else
        remote_url = g_strdup (location);
      opt_url = remote_url;

      /* Default to gpg verify if no verification is enabled, except for OCI registries */
      is_oci = opt_url && g_str_has_prefix (opt_url, "oci+");
      if (!opt_no_sign_verify && !(opt_sign_keys && opt_sign_keys[0]) && !is_oci)
        opt_do_gpg_verify = TRUE;
    }

  if (!get_config_from_opts (config, remote_name, &gpg_data, error))
    return FALSE;

  remotes = flatpak_dir_list_remotes (dir, cancellable, error);
  if (remotes == NULL)
    return FALSE;

  if (g_strv_contains ((const char **) remotes, remote_name))
    {
      if (opt_if_not_exists)
        {
          /* Do nothing */

          /* Except, for historical reasons this applies/clears the filter of pre-existing
             remotes, so that a default-shipped filtering remote can be replaced, clearing the
             filter, by following  standard docs. */
          g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);
          g_autofree char *new_filter = g_key_file_get_string (config, group, "xa.filter", NULL);

          if (!flatpak_dir_compare_remote_filter (dir, remote_name, new_filter))
            {
              g_autoptr(GKeyFile) new_config = ostree_repo_copy_config (flatpak_dir_get_repo (dir));

              g_key_file_set_string (new_config, group, "xa.filter", new_filter ? new_filter : "");

              if (!flatpak_dir_modify_remote (dir, remote_name, new_config, NULL, cancellable, error))
                return FALSE;
            }

          return TRUE;
        }

      return flatpak_fail (error, _("Remote %s already exists"), remote_name);
    }

  if (opt_gpg_import != NULL)
    {
      g_clear_pointer (&gpg_data, g_bytes_unref);
      gpg_data = flatpak_load_gpg_keys (opt_gpg_import, cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  if (opt_authenticator_name && !g_dbus_is_name (opt_authenticator_name))
    return flatpak_fail (error, _("Invalid authenticator name %s"), opt_authenticator_name);

  if (!flatpak_dir_modify_remote (dir, remote_name, config, gpg_data, cancellable, error))
    return FALSE;

  /* Reload previously changed configuration */
  if (!flatpak_dir_recreate_repo (dir, cancellable, error))
    return FALSE;

  /* We can't retrieve the extra metadata until the remote has been added locally, since
     ostree_repo_remote_fetch_summary() works with the repository's name, not its URL.
     Don't propagate IO failed errors here because we might just be offline - the
     remote should already be usable. */
  if (!flatpak_dir_update_remote_configuration (dir, remote_name, NULL, NULL, cancellable, &local_error))
    {
      if (local_error->domain == G_RESOLVER_ERROR ||
          (local_error->domain == G_IO_ERROR && strstr(local_error->message, "ed25519") == NULL))
        {
          g_printerr (_("Warning: Could not update extra metadata for '%s': %s\n"), remote_name, local_error->message);
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  return TRUE;
}

gboolean
flatpak_complete_remote_add (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, common_options, NULL);
  if (!flatpak_option_context_parse (context, add_options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1:
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, common_options);
      flatpak_complete_options (completion, add_options);
      flatpak_complete_options (completion, user_entries);

      break;
    }

  return TRUE;
}
