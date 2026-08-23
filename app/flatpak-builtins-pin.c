/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

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

gboolean
flatpak_builtin_pin (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  int i;

  context = g_option_context_new (_("[PATTERN…] - disable automatic removal of runtimes matching patterns"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR,
                                     &dirs, cancellable, error))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  if (argc == 1)
    {
      g_autoptr(GPtrArray) patterns = NULL;
      gboolean fancy_output = flatpak_fancy_output ();

      patterns = flatpak_dir_get_config_patterns (dir, "pinned");

      if (patterns->len == 0)
        {
          if (fancy_output)
            g_print (_("No pinned patterns\n"));
        }
      else
        {
          if (fancy_output)
            {
              g_autoptr(GPtrArray) refs = NULL;
              gsize pattern_width = 0;

              refs = flatpak_dir_list_refs (dir, FLATPAK_KINDS_RUNTIME,
                                            cancellable, error);
              if (refs == NULL)
                return FALSE;

              for (guint pattern_index = 0; pattern_index < patterns->len; pattern_index++)
                pattern_width = MAX (pattern_width,
                                     strlen (g_ptr_array_index (patterns, pattern_index)));

              g_print (_("Pinned patterns:\n"));

              for (guint pattern_index = 0; pattern_index < patterns->len; pattern_index++)
                {
                  const char *pattern = g_ptr_array_index (patterns, pattern_index);
                  g_autofree char *regexp_string = NULL;
                  g_autofree char *anchored_regexp = NULL;
                  g_autoptr(GRegex) regexp = NULL;
                  guint matches = 0;

                  regexp_string = flatpak_filter_glob_to_regexp (pattern,
                                                                  TRUE,
                                                                  error);
                  if (regexp_string == NULL)
                    return FALSE;

                  anchored_regexp = g_strdup_printf ("^(%s)$", regexp_string);
                  regexp = g_regex_new (anchored_regexp,
                                        G_REGEX_DOLLAR_ENDONLY | G_REGEX_RAW | G_REGEX_OPTIMIZE,
                                        G_REGEX_MATCH_ANCHORED, error);
                  if (regexp == NULL)
                    return FALSE;

                  for (guint ref_index = 0; ref_index < refs->len; ref_index++)
                    {
                      FlatpakDecomposed *ref = g_ptr_array_index (refs, ref_index);

                      if (g_regex_match (regexp, flatpak_decomposed_get_ref (ref),
                                         G_REGEX_MATCH_ANCHORED, NULL))
                        matches++;
                    }

                  g_print ("  %-*s  ", (int) pattern_width, pattern);
                  g_print (ngettext ("%u match\n", "%u matches\n", matches), matches);
                }
            }
          else
            {
              for (i = 0; i < patterns->len; i++)
                {
                  const char *old = g_ptr_array_index (patterns, i);
                  g_print ("  %s\n", old);
                }
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
              if (!flatpak_dir_config_remove_pattern (dir, "pinned", pattern, error))
                return FALSE;
            }
          else if (!flatpak_dir_config_append_pattern (dir, "pinned", pattern,
                                                       TRUE, /* only match runtimes */
                                                       NULL, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
flatpak_complete_pin (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  if (opt_remove)
    {
      g_autoptr(GPtrArray) patterns = flatpak_dir_get_config_patterns (dir, "pinned");

      for (guint pattern_index = 0; pattern_index < patterns->len; pattern_index++)
        flatpak_complete_word (completion, "%s ",
                               (const char *) g_ptr_array_index (patterns, pattern_index));

      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);
      return TRUE;
    }

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
