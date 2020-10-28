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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static gboolean opt_reset;
static gboolean opt_show;

static GOptionEntry options[] = {
  { "reset", 0, 0, G_OPTION_ARG_NONE, &opt_reset, N_("Remove existing overrides"), NULL },
  { "show", 0, 0, G_OPTION_ARG_NONE, &opt_show, N_("Show existing overrides"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_override (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  const char *app;
  g_autoptr(FlatpakContext) arg_context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_autoptr(GError) my_error = NULL;

  context = g_option_context_new (_("[APP] - Override settings [for application]"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR,
                                     &dirs, cancellable, error))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  if (argc >= 2)
    {
      app = argv[1];
      if (!flatpak_is_valid_name (app, -1, &my_error))
        return flatpak_fail (error, _("'%s' is not a valid application name: %s"), app, my_error->message);
    }
  else
    app = NULL;

  if (opt_reset)
    return flatpak_remove_override_keyfile (app, flatpak_dir_is_user (dir), &my_error);

  metakey = flatpak_load_override_keyfile (app, flatpak_dir_is_user (dir), &my_error);
  if (metakey == NULL)
    {
      if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
      metakey = g_key_file_new ();
    }

  if (opt_show)
    {
      g_autofree char *data = NULL;

      data = g_key_file_to_data (metakey, NULL, error);
      if (data == NULL)
        return FALSE;

      g_print ("%s", data);
      return TRUE;
    }

  overrides = flatpak_context_new ();
  if (!flatpak_context_load_metadata (overrides, metakey, error))
    return FALSE;

  flatpak_context_merge (overrides, arg_context);

  flatpak_context_save_metadata (overrides, FALSE, metakey);

  if (!flatpak_save_override_keyfile (metakey, app, flatpak_dir_is_user (dir), error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_override (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GError) error = NULL;
  int i;
  g_autoptr(FlatpakContext) arg_context = NULL;

  context = g_option_context_new ("");

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, user_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_context (completion);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          int j;
          g_auto(GStrv) refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, NULL,
                                                                FLATPAK_KINDS_APP,
                                                                FIND_MATCHING_REFS_FLAGS_NONE,
                                                                &error);
          if (refs == NULL)
            flatpak_completion_debug ("find local refs error: %s", error->message);
          for (j = 0; refs != NULL && refs[j] != NULL; j++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[j], NULL);
              if (parts)
                flatpak_complete_word (completion, "%s ", parts[1]);
            }
        }

      break;
    }

  return TRUE;
}
