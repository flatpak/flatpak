/*
 * Copyright © 2020 Endless OS Foundation LLC
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
 *       Matthew Leeds <matthew.leeds@endlessm.com>
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
#include "flatpak-cli-transaction.h"
#include "flatpak-quiet-transaction.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"

/* Note: the code here is copied from flatpak-builtins-mask.c */

static gboolean opt_remove;

static GOptionEntry options[] = {
  { "remove", 0, 0, G_OPTION_ARG_NONE, &opt_remove, N_("Remove matching pins"), NULL },
  { NULL }
};

static GPtrArray *
get_old_patterns (FlatpakDir *dir)
{
  g_autoptr(GPtrArray) patterns = NULL;
  g_autofree char *pinned = NULL;
  int i;

  patterns = g_ptr_array_new_with_free_func (g_free);

  pinned = flatpak_dir_get_config (dir, "pinned", NULL);
  if (pinned)
    {
      g_auto(GStrv) oldv = g_strsplit (pinned, ";", -1);

      for (i = 0; oldv[i] != NULL; i++)
        {
          const char *old = oldv[i];

          if (*old != 0 && !flatpak_g_ptr_array_contains_string (patterns, old))
            g_ptr_array_add (patterns, g_strdup (old));
        }
    }

  return g_steal_pointer (&patterns);
}


gboolean
flatpak_builtin_pin (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  g_autofree char *merged_patterns = NULL;
  g_autoptr(GPtrArray) patterns = NULL;
  int i;

  context = g_option_context_new (_("[PATTERN…] - disable automatic removal of runtimes matching patterns"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR,
                                     &dirs, cancellable, error))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  patterns = get_old_patterns (dir);

  if (argc == 1)
    {
      if (patterns->len == 0)
        {
          g_print (_("No pinned patterns\n"));
        }
      else
        {
          g_print (_("Pinned patterns:\n"));

          for (i = 0; i < patterns->len; i++)
            {
              const char *old = g_ptr_array_index (patterns, i);
              g_print ("  %s\n", old);
            }
        }
    }
  else
    {
      for (i = 1; i < argc; i++)
        {
          const char *pattern = argv[i];

          if (opt_remove)
            {
              int j;

              for (j = 0; j < patterns->len; j++)
                {
                  if (strcmp (g_ptr_array_index (patterns, j), pattern) == 0)
                    break;
                }

              if (j == patterns->len)
                return flatpak_fail (error, _("No current pin matching %s"), pattern);
              else
                g_ptr_array_remove_index (patterns, j);
            }
          else
            {
              g_autofree char *regexp;

              regexp = flatpak_filter_glob_to_regexp (pattern,
                                                      TRUE, /* only match runtimes */
                                                      error);
              if (regexp == NULL)
                return FALSE;

              if (!flatpak_g_ptr_array_contains_string (patterns, pattern))
                g_ptr_array_add (patterns, g_strdup (pattern));
            }
        }

      g_ptr_array_sort (patterns, flatpak_strcmp0_ptr);

      g_ptr_array_add (patterns, NULL);
      merged_patterns = g_strjoinv (";", (char **)patterns->pdata);

      if (!flatpak_dir_set_config (dir, "pinned", merged_patterns, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_pin (FlatpakCompletion *completion)
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
    case 0:
    case 1: /* PATTERN */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);
      break;
    }

  return TRUE;
}
