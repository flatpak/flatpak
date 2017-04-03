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
#include "flatpak-utils.h"
#include "flatpak-chain-input-stream.h"

static gboolean opt_no_gpg_verify;
static gboolean opt_do_gpg_verify;
static gboolean opt_do_enumerate;
static gboolean opt_no_enumerate;
static gboolean opt_do_deps;
static gboolean opt_no_deps;
static gboolean opt_if_not_exists;
static gboolean opt_enable;
static gboolean opt_oci;
static gboolean opt_update_metadata;
static gboolean opt_disable;
static int opt_prio = -1;
static char *opt_title;
static char *opt_default_branch;
static char *opt_url;
static gboolean opt_from;
static char **opt_gpg_import;


static GOptionEntry add_options[] = {
  { "if-not-exists", 0, 0, G_OPTION_ARG_NONE, &opt_if_not_exists, N_("Do nothing if the provided remote exists"), NULL },
  { "from", 0, 0, G_OPTION_ARG_NONE, &opt_from, N_("LOCATION specifies a configuration file, not the repo location"), NULL },
  { NULL }
};

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
  { "gpg-import", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_import, N_("Import GPG key from FILE (- for stdin)"), N_("FILE") },
  { "disable", 0, 0, G_OPTION_ARG_NONE, &opt_disable, N_("Disable the remote"), NULL },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Add OCI registry"), NULL },
  { NULL }
};

static gboolean
open_source_stream (GInputStream **out_source_stream,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  g_autoptr(GPtrArray) streams = NULL;

  if (opt_gpg_import != NULL)
    n_keyrings = g_strv_length (opt_gpg_import);

  guint ii;

  streams = g_ptr_array_new_with_free_func (g_object_unref);

  for (ii = 0; ii < n_keyrings; ii++)
    {
      GInputStream *input_stream = NULL;

      if (strcmp (opt_gpg_import[ii], "-") == 0)
        {
          input_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
        }
      else
        {
          g_autoptr(GFile) file = g_file_new_for_commandline_arg (opt_gpg_import[ii]);
          input_stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            {
              g_prefix_error (error, "The file %s specified for --gpg-import was not found: ", opt_gpg_import[ii]);
              return FALSE;
            }
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) flatpak_chain_input_stream_new (streams);

  *out_source_stream = g_steal_pointer (&source_stream);

  return TRUE;
}

static GBytes *
load_keys (GCancellable *cancellable,
           GError      **error)
{
  g_autoptr(GInputStream) input_stream = NULL;
  g_autoptr(GOutputStream) output_stream = NULL;
  gssize n_bytes_written;

  if (!open_source_stream (&input_stream, cancellable, error))
    return FALSE;

  output_stream = g_memory_output_stream_new_resizable ();

  n_bytes_written = g_output_stream_splice (output_stream, input_stream,
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                            NULL, error);
  if (n_bytes_written < 0)
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (output_stream));
}

static GKeyFile *
get_config_from_opts (FlatpakDir *dir, const char *remote_name)
{
  GKeyFile *config = ostree_repo_copy_config (flatpak_dir_get_repo (dir));
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  if (opt_no_gpg_verify)
    {
      g_key_file_set_boolean (config, group, "gpg-verify", FALSE);
      g_key_file_set_boolean (config, group, "gpg-verify-summary", FALSE);
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

  if (opt_title)
    g_key_file_set_string (config, group, "xa.title", opt_title);

  if (opt_default_branch)
    g_key_file_set_string (config, group, "xa.default-branch", opt_default_branch);

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
  else if (opt_enable)
    g_key_file_set_boolean (config, group, "xa.disable", FALSE);

  if (opt_oci)
    g_key_file_set_boolean (config, group, "xa.oci", TRUE);

  if (opt_prio != -1)
    {
      g_autofree char *prio_as_string = g_strdup_printf ("%d", opt_prio);
      g_key_file_set_string (config, group, "xa.prio", prio_as_string);
    }

  return config;
}

static void
load_options (const char *filename,
              GBytes **gpg_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  char *str;
  gboolean nodeps;
  g_autoptr(GBytes) bytes = NULL;
  g_autofree char *version = NULL;

  if (g_str_has_prefix (filename, "http:") ||
      g_str_has_prefix (filename, "https:"))
    {
      const char *options_data;
      gsize options_size;

      bytes = download_uri (filename, &error);

      if (bytes == NULL)
        {
          g_printerr ("Can't load uri %s: %s\n", filename, error->message);
          exit (1);
        }

      options_data = g_bytes_get_data (bytes, &options_size);
      if (!g_key_file_load_from_data (keyfile, options_data, options_size, 0, &error))
        {
          g_printerr ("Can't load uri %s: %s\n", filename, error->message);
          exit (1);
        }
    }
  else
    {
      if (!g_key_file_load_from_file (keyfile, filename, 0, &error))
        {
          g_printerr ("Can't load file %s: %s\n", filename, error->message);
          exit (1);
        }
    }


  if (!g_key_file_has_group (keyfile, FLATPAK_REPO_GROUP))
    {
      g_printerr ("Invalid file format");
      exit (1);
    }

  version = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_VERSION_KEY, NULL);
  if (version != NULL && strcmp (version, "1") != 0)
    {
      g_printerr ("Invalid version %s, only 1 supported", version);
      exit (1);
    }

  str = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                               FLATPAK_REPO_URL_KEY, NULL);
  if (str != NULL)
    opt_url = str;

  str = g_key_file_get_locale_string (keyfile, FLATPAK_REPO_GROUP,
                                      FLATPAK_REPO_TITLE_KEY, NULL, NULL);
  if (str != NULL)
    opt_title = str;

  str = g_key_file_get_locale_string (keyfile, FLATPAK_REPO_GROUP,
                                      FLATPAK_REPO_DEFAULT_BRANCH_KEY, NULL, NULL);
  if (str != NULL)
    opt_default_branch = str;

  nodeps = g_key_file_get_boolean (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_NODEPS_KEY, NULL);
  if (nodeps)
    {
      opt_no_deps = TRUE;
      opt_do_deps = FALSE;
    }

  str = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                               FLATPAK_REPO_GPGKEY_KEY, NULL);
  if (str != NULL)
    {
      guchar *decoded;
      gsize decoded_len;

      str = g_strstrip (str);
      decoded = g_base64_decode (str, &decoded_len);
      if (decoded_len < 10) /* Check some minimal size so we don't get crap */
        {
          g_printerr ("Invalid gpg key");
          exit (1);
        }

      *gpg_data = g_bytes_new_take (decoded, decoded_len);
      if (!opt_no_gpg_verify)
        opt_do_gpg_verify = TRUE;
    }
}

static gboolean
update_remote_with_extra_metadata (FlatpakDir* dir,
                                   const char *remote,
                                   GBytes *gpg_data,
                                   GCancellable *cancellable,
                                   GError **error)
{
  g_autofree char *title = NULL;
  g_autofree char *default_branch = NULL;
  g_autoptr(GKeyFile) config = NULL;

  if (opt_title == NULL)
    {
      title = flatpak_dir_fetch_remote_title (dir,
                                              remote,
                                              NULL,
                                              NULL);
      if (title)
        opt_title = title;
    }

    if (opt_default_branch == NULL)
    {
      default_branch = flatpak_dir_fetch_remote_default_branch (dir,
                                                                remote,
                                                                NULL,
                                                                NULL);
      if (default_branch)
        opt_default_branch = default_branch;
    }

    if (title != NULL || default_branch != NULL)
      {
        config = get_config_from_opts (dir, remote);
        return flatpak_dir_modify_remote (dir, remote, config, gpg_data, cancellable, error);
      }

    return TRUE;
}

gboolean
flatpak_builtin_add_remote (int argc, char **argv,
                            GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_auto(GStrv) remotes = NULL;
  g_autofree char *remote_url = NULL;
  const char *remote_name;
  const char *location = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_data = NULL;

  context = g_option_context_new (_("NAME LOCATION - Add a remote repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  g_option_context_add_main_entries (context, common_options, NULL);

  if (!flatpak_option_context_parse (context, add_options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("NAME must be specified"), error);

  if (argc < 3)
    return usage_error (context, _("LOCATION must be specified"), error);

  if (argc > 3)
    return usage_error (context, _("Too many arguments"), error);

  remote_name = argv[1];
  location = argv[2];

  remotes = flatpak_dir_list_remotes (dir, cancellable, error);
  if (remotes == NULL)
    return FALSE;

  if (g_strv_contains ((const char **)remotes, remote_name))
    {
      if (opt_if_not_exists)
        return TRUE; /* Do nothing */

      return flatpak_fail (error, _("Remote %s already exists"), remote_name);
    }

  if (opt_from ||
      flatpak_file_arg_has_suffix (location, ".flatpakrepo"))
    {
      load_options (location, &gpg_data);
      if (opt_url == NULL)
        return flatpak_fail (error, _("No url specified in flatpakrepo file"));
    }
  else
    {
      file = g_file_new_for_commandline_arg (location);
      if (g_file_is_native (file))
        remote_url = g_file_get_uri (file);
      else
        remote_url = g_strdup (location);
      opt_url = remote_url;
    }

  /* Default to gpg verify */
  if (!opt_no_gpg_verify)
    opt_do_gpg_verify = TRUE;

  config = get_config_from_opts (dir, remote_name);

  if (opt_gpg_import != NULL)
    {
      gpg_data = load_keys (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  if (!flatpak_dir_modify_remote (dir, remote_name, config, gpg_data, cancellable, error))
    return FALSE;

  /* Reload previously changed configuration */
  if (!flatpak_dir_recreate_repo (dir, cancellable, error))
    return FALSE;

  /* We can't retrieve the extra metadata until the remote has been added locally, since
     ostree_repo_remote_fetch_summary() works with the repository's name, not its URL. */
  return update_remote_with_extra_metadata (dir, remote_name, gpg_data, cancellable, error);
}

gboolean
flatpak_complete_add_remote (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;

  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, common_options, NULL);
  if (!flatpak_option_context_parse (context, add_options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
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

gboolean
flatpak_builtin_modify_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  const char *remote_name;

  context = g_option_context_new (_("NAME - Modify a remote repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  g_option_context_add_main_entries (context, common_options, NULL);

  if (!flatpak_option_context_parse (context, modify_options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("Remote NAME must be specified"), error);

  remote_name = argv[1];

  if (!ostree_repo_remote_get_url (flatpak_dir_get_repo (dir), remote_name, NULL, NULL))
    return flatpak_fail (error, _("No remote %s"), remote_name);

  if (opt_update_metadata)
    {
      g_autoptr(GError) local_error = NULL;

      g_print (_("Updating extra metadata from remote summary for %s\n"), remote_name);
      if (!flatpak_dir_update_remote_configuration (dir, remote_name, cancellable, &local_error))
        {
          g_printerr (_("Error updating extra metadata for '%s': %s\n"), remote_name, local_error->message);
          return flatpak_fail (error, _("Could not update extra metadata for %s"), remote_name);
        }

      /* Reload changed configuration */
      if (!flatpak_dir_recreate_repo (dir, cancellable, error))
        return FALSE;
    }

  config = get_config_from_opts (dir, remote_name);

  if (opt_gpg_import != NULL)
    {
      gpg_data = load_keys (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  return flatpak_dir_modify_remote (dir, remote_name, config, gpg_data, cancellable, error);
}

gboolean
flatpak_complete_modify_remote (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  int i;

  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, common_options, NULL);
  if (!flatpak_option_context_parse (context, modify_options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, common_options);
      flatpak_complete_options (completion, modify_options);
      flatpak_complete_options (completion, user_entries);

      {
        g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
        if (remotes == NULL)
          return FALSE;
        for (i = 0; remotes[i] != NULL; i++)
          flatpak_complete_word (completion, "%s ", remotes[i]);
      }

      break;
    }

  return TRUE;
}
