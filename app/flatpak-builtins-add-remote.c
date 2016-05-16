/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include <gio/gunixinputstream.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-chain-input-stream.h"

static gboolean opt_no_gpg_verify;
static gboolean opt_do_gpg_verify;
static gboolean opt_do_enumerate;
static gboolean opt_no_enumerate;
static gboolean opt_if_not_exists;
static gboolean opt_enable;
static gboolean opt_disable;
static int opt_prio = -1;
static char *opt_title;
static char *opt_url;
static char **opt_gpg_import;


static GOptionEntry add_options[] = {
  { "if-not-exists", 0, 0, G_OPTION_ARG_NONE, &opt_if_not_exists, "Do nothing if the provided remote exists", NULL },
  { NULL }
};

static GOptionEntry modify_options[] = {
  { "gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_do_gpg_verify, "Enable GPG verification", NULL },
  { "enumerate", 0, 0, G_OPTION_ARG_NONE, &opt_do_enumerate, "Mark the remote as enumerate", NULL },
  { "url", 0, 0, G_OPTION_ARG_STRING, &opt_url, "Set a new url", NULL },
  { "enable", 0, 0, G_OPTION_ARG_NONE, &opt_enable, "Enable the remote",  },
  { NULL }
};

static GOptionEntry common_options[] = {
  { "no-gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_gpg_verify, "Disable GPG verification", NULL },
  { "no-enumerate", 0, 0, G_OPTION_ARG_NONE, &opt_no_enumerate, "Mark the remote as don't enumerate", NULL },
  { "prio", 0, 0, G_OPTION_ARG_INT, &opt_prio, "Set priority (default 1, higher is more prioritized)", NULL },
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, "A nice name to use for this remote", "TITLE" },
  { "gpg-import", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_import, "Import GPG key from FILE (- for stdin)", "FILE" },
  { "disable", 0, 0, G_OPTION_ARG_NONE, &opt_disable, "Disable the remote",  },
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
          g_autoptr(GFile) file = g_file_new_for_path (opt_gpg_import[ii]);
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

GBytes *
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

  if (opt_no_enumerate)
    g_key_file_set_boolean (config, group, "xa.noenumerate", TRUE);

  if (opt_do_enumerate)
    g_key_file_set_boolean (config, group, "xa.noenumerate", FALSE);

  if (opt_disable)
    g_key_file_set_boolean (config, group, "xa.disable", TRUE);
  else if (opt_enable)
    g_key_file_set_boolean (config, group, "xa.disable", FALSE);

  if (opt_prio != -1)
    {
      g_autofree char *prio_as_string = g_strdup_printf ("%d", opt_prio);
      g_key_file_set_string (config, group, "xa.prio", prio_as_string);
    }

  return config;
}

gboolean
flatpak_builtin_add_remote (int argc, char **argv,
                            GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_auto(GStrv) remotes = NULL;
  g_autofree char *title = NULL;
  g_autofree char *remote_url = NULL;
  const char *remote_name;
  const char *url_or_path;
  g_autofree char *prio_as_string = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_data = NULL;

  context = g_option_context_new ("NAME LOCATION - Add a remote repository");

  g_option_context_add_main_entries (context, common_options, NULL);

  if (!flatpak_option_context_parse (context, add_options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 3)
    return usage_error (context, "NAME and LOCATION must be specified", error);

  remote_name = argv[1];
  url_or_path  = argv[2];

  remotes = flatpak_dir_list_remotes (dir, cancellable, error);
  if (remotes == NULL)
    return FALSE;

  if (g_strv_contains ((const char **)remotes, remote_name))
    {
      if (opt_if_not_exists)
        return TRUE; /* Do nothing */

      return flatpak_fail (error, "Remote %s already exists", remote_name);
    }

  file = g_file_new_for_commandline_arg (url_or_path);
  if (g_file_is_native (file))
    remote_url = g_file_get_uri (file);
  else
    remote_url = g_strdup (url_or_path);

  /* Default to gpg verify */
  if (!opt_no_gpg_verify)
    opt_do_gpg_verify = TRUE;

  opt_url = remote_url;

  if (opt_title == NULL)
    {
      title = flatpak_dir_fetch_remote_title (dir,
                                              remote_name,
                                              NULL,
                                              NULL);
      if (title)
        opt_title = title;
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
flatpak_builtin_modify_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  const char *remote_name;

  context = g_option_context_new ("NAME - Modify a remote repository");

  g_option_context_add_main_entries (context, common_options, NULL);

  if (!flatpak_option_context_parse (context, modify_options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "remote NAME must be specified", error);

  remote_name = argv[1];

  if (!ostree_repo_remote_get_url (flatpak_dir_get_repo (dir), remote_name, NULL, NULL))
    return flatpak_fail (error, "No remote %s", remote_name);

  config = get_config_from_opts (dir, remote_name);

  if (opt_gpg_import != NULL)
    {
      gpg_data = load_keys (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  return flatpak_dir_modify_remote (dir, remote_name, config, gpg_data, cancellable, error);
}
