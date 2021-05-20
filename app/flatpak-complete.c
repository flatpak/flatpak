/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include "flatpak-complete.h"
#include "flatpak-installation.h"
#include "flatpak-utils-private.h"

/* Uncomment to get debug traces in /tmp/flatpak-completion-debug.txt (nice
 * to not have it interfere with stdout/stderr)
 */
#if 0
void
flatpak_completion_debug (const gchar *format, ...)
{
  va_list var_args;
  gchar *s;
  static FILE *f = NULL;

  va_start (var_args, format);
  s = g_strdup_vprintf (format, var_args);
  if (f == NULL)
    f = fopen ("/tmp/flatpak-completion-debug.txt", "a+");
  fprintf (f, "%s\n", s);
  fflush (f);
  g_free (s);
}
#else
void
flatpak_completion_debug (const gchar *format, ...)
{
}
#endif

static gboolean
is_word_separator (char c)
{
  return g_ascii_isspace (c);
}

void
flatpak_complete_file (FlatpakCompletion *completion,
                       const char        *file_type)
{
  flatpak_completion_debug ("completing FILE");
  g_print ("%s\n", file_type);
}

void
flatpak_complete_dir (FlatpakCompletion *completion)
{
  flatpak_completion_debug ("completing DIR");
  g_print ("%s\n", "__FLATPAK_DIR");
}

void
flatpak_complete_word (FlatpakCompletion *completion,
                       char *format, ...)
{
  va_list args;
  const char *rest;
  const char *shell_cur;
  const char *shell_cur_end;
  g_autofree char *string = NULL;

  g_return_if_fail (format != NULL);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  va_start (args, format);
  string = g_strdup_vprintf (format, args);
  va_end (args);
#pragma GCC diagnostic pop

  if (!g_str_has_prefix (string, completion->cur))
    return;

  shell_cur = completion->shell_cur ? completion->shell_cur : "";

  rest = string + strlen (completion->cur);

  shell_cur_end = shell_cur + strlen (shell_cur);
  while (shell_cur_end > shell_cur &&
         rest > string &&
         shell_cur_end[-1] == rest[-1] &&
         /* I'm not sure exactly what bash is doing here with =, but this seems to work... */
         shell_cur_end[-1] != '=')
    {
      rest--;
      shell_cur_end--;
    }

  flatpak_completion_debug ("completing word: '%s' (%s)", string, rest);

  g_print ("%s\n", rest);
}

void
flatpak_complete_ref_id (FlatpakCompletion *completion,
                         GPtrArray         *refs)
{
  if (refs == NULL)
    return;

  for (int i = 0; i < refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
      g_autofree char *id = flatpak_decomposed_dup_id (ref);
      flatpak_complete_word (completion, "%s ", id);
    }
}

void
flatpak_complete_ref_branch (FlatpakCompletion *completion,
                             GPtrArray         *refs)
{
  if (refs == NULL)
    return;

  for (int i = 0; i < refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
      g_autofree char *branch = flatpak_decomposed_dup_branch (ref);
      flatpak_complete_word (completion, "%s ", branch);
    }
}

void
flatpak_complete_ref (FlatpakCompletion *completion,
                      OstreeRepo        *repo)
{
  g_autoptr(GHashTable) refs = NULL;
  flatpak_completion_debug ("completing REF");

  if (ostree_repo_list_refs (repo,
                             NULL,
                             &refs, NULL, NULL))
    {
      GHashTableIter hashiter;
      gpointer hashkey, hashvalue;

      g_hash_table_iter_init (&hashiter, refs);
      while ((g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue)))
        {
          const char *ref = (const char *) hashkey;
          if (!(g_str_has_prefix (ref, "runtime/") ||
                g_str_has_prefix (ref, "app/")))
            continue;
          flatpak_complete_word (completion, "%s", ref);
        }
    }
}

static int
find_current_element (const char *str)
{
  int count = 0;

  if (g_str_has_prefix (str, "app/"))
    str += strlen ("app/");
  else if (g_str_has_prefix (str, "runtime/"))
    str += strlen ("runtime/");

  while (str != NULL && count <= 3)
    {
      str = strchr (str, '/');
      count++;
      if (str != NULL)
        str = str + 1;
    }

  return count;
}

void
flatpak_complete_partial_ref (FlatpakCompletion *completion,
                              FlatpakKinds       kinds,
                              const char        *only_arch,
                              FlatpakDir        *dir,
                              const char        *remote)
{
  FlatpakKinds matched_kinds;
  const char *pref;
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  int element;
  const char *cur_parts[4] = { NULL };
  g_autoptr(GError) error = NULL;
  int i;

  pref = completion->cur;
  element = find_current_element (pref);

  flatpak_split_partial_ref_arg_novalidate (pref, kinds,
                                            NULL, NULL,
                                            &matched_kinds, &id, &arch, &branch);

  cur_parts[1] = id;
  cur_parts[2] = arch ? arch : "";
  cur_parts[3] = branch ? branch : "";

  if (remote)
    {
      g_autoptr(FlatpakRemoteState) state = get_remote_state (dir, remote, TRUE, FALSE,
                                                              (element > 2) ? arch : only_arch, NULL,
                                                              NULL, &error);
      if (state != NULL)
        refs = flatpak_dir_find_remote_refs (dir, state,
                                             (element > 1) ? id : NULL,
                                             (element > 3) ? branch : NULL,
                                             NULL, /* default branch */
                                             (element > 2) ? arch : only_arch,
                                             NULL, /* default arch */
                                             matched_kinds,
                                             FIND_MATCHING_REFS_FLAGS_NONE,
                                             NULL, &error);
    }
  else
    {
      refs = flatpak_dir_find_installed_refs (dir,
                                              (element > 1) ? id : NULL,
                                              (element > 3) ? branch : NULL,
                                              (element > 2) ? arch : only_arch,
                                              matched_kinds,
                                              FIND_MATCHING_REFS_FLAGS_NONE,
                                              &error);
    }
  if (refs == NULL)
    flatpak_completion_debug ("find refs error: %s", error->message);

  for (i = 0; refs != NULL && i < refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
      int j;
      g_autoptr(GString) comp = NULL;
      g_auto(GStrv) parts = g_strsplit (flatpak_decomposed_get_ref (ref), "/", 0);

      if (!g_str_has_prefix (parts[element], cur_parts[element]))
        continue;

      if (flatpak_id_has_subref_suffix (parts[element], -1))
        {
          char *last_dot = strrchr (parts[element], '.');

          if (last_dot == NULL)
            continue; /* Shouldn't really happen */

          /* Only complete to subrefs is fully matching real part.
           * For example, only match org.foo.Bar.Sources for
           * "org.foo.Bar", "org.foo.Bar." or "org.foo.Bar.S", but
           * not for "org.foo" or other shorter prefixes.
           */
          if (strncmp (parts[element], cur_parts[element], last_dot - parts[element] - 1) != 0)
            continue;
        }

      comp = g_string_new (pref);
      g_string_append (comp, parts[element] + strlen (cur_parts[element]));

      /* Only complete on the last part if the user explicitly adds a / */
      if (element >= 2)
        {
          for (j = element + 1; j < 4; j++)
            {
              g_string_append (comp, "/");
              g_string_append (comp, parts[j]);
            }
        }

      flatpak_complete_word (completion, "%s", comp->str);
    }
}

static gboolean
switch_already_in_line (FlatpakCompletion *completion,
                        GOptionEntry      *entry)
{
  guint i = 0;
  guint line_part_len = 0;

  for (; i < completion->original_argc; ++i)
    {
      line_part_len = strlen (completion->original_argv[i]);
      if (line_part_len > 2 &&
          g_strcmp0 (&completion->original_argv[i][2], entry->long_name) == 0)
        return TRUE;

      if (line_part_len == 2 &&
          completion->original_argv[i][1] == entry->short_name)
        return TRUE;
    }

  return FALSE;
}

static gboolean
should_filter_out_option_from_completion (FlatpakCompletion *completion,
                                          GOptionEntry      *entry)
{
  switch (entry->arg)
    {
    case G_OPTION_ARG_NONE:
    case G_OPTION_ARG_STRING:
    case G_OPTION_ARG_INT:
    case G_OPTION_ARG_FILENAME:
    case G_OPTION_ARG_DOUBLE:
    case G_OPTION_ARG_INT64:
      return switch_already_in_line (completion, entry);

    default:
      return FALSE;
    }
}

void
flatpak_complete_options (FlatpakCompletion *completion,
                          GOptionEntry      *entries)
{
  GOptionEntry *e = entries;
  int i;

  while (e->long_name != NULL)
    {
      if (e->arg_description)
        {
          g_autofree char *prefix = g_strdup_printf ("--%s=", e->long_name);

          if (g_str_has_prefix (completion->cur, prefix))
            {
              if (strcmp (e->arg_description, "ARCH") == 0)
                {
                  const char *arches[] = {"i386", "x86_64", "aarch64", "arm"};
                  for (i = 0; i < G_N_ELEMENTS (arches); i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, arches[i]);
                }
              else if (strcmp (e->arg_description, "SHARE") == 0)
                {
                  for (i = 0; flatpak_context_shares[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_shares[i]);
                }
              else if (strcmp (e->arg_description, "DEVICE") == 0)
                {
                  for (i = 0; flatpak_context_devices[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_devices[i]);
                }
              else if (strcmp (e->arg_description, "FEATURE") == 0)
                {
                  for (i = 0; flatpak_context_features[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_features[i]);
                }
              else if (strcmp (e->arg_description, "SOCKET") == 0)
                {
                  for (i = 0; flatpak_context_sockets[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_sockets[i]);
                }
              else if (strcmp (e->arg_description, "FILE") == 0)
                {
                  flatpak_complete_file (completion, "__FLATPAK_FILE");
                }
              else if (strcmp (e->long_name, "installation") == 0)
                {
                  g_autoptr(GPtrArray) installations = NULL;
                  installations = flatpak_get_system_installations (NULL, NULL);
                  for (i = 0; i < installations->len; i++)
                    {
                      FlatpakInstallation *inst = g_ptr_array_index (installations, i);
                      flatpak_complete_word (completion, "%s%s ", prefix, flatpak_installation_get_id (inst));
                    }
                }
              else if (strcmp (e->long_name, "columns") == 0)
                {
                  /* columns are treated separately */
                }
              else
                flatpak_complete_word (completion, "%s", prefix);
            }
          else
            flatpak_complete_word (completion, "%s", prefix);
        }
      else
        {
          /* If this is just a switch, then don't add it multiple
           * times */
          if (!should_filter_out_option_from_completion (completion, e))
            {
              flatpak_complete_word (completion, "--%s ", e->long_name);
            }
          else
            {
              flatpak_completion_debug ("switch --%s is already in line %s", e->long_name, completion->line);
            }
        }

      /* We may end up checking switch_already_in_line twice, but this is
       * for simplicity's sake - the alternative solution would be to
       * continue the loop early and have to increment e. */
      if (e->short_name != 0)
        {
          /* This is a switch, we may not want to add it */
          if (!e->arg_description)
            {
              if (!should_filter_out_option_from_completion (completion, e))
                {
                  flatpak_complete_word (completion, "-%c ", e->short_name);
                }
              else
                {
                  flatpak_completion_debug ("switch -%c is already in line %s", e->short_name, completion->line);
                }
            }
          else
            {
              flatpak_complete_word (completion, "-%c ", e->short_name);
            }
        }
      e++;
    }
}

static void
flatpak_complete_column (FlatpakCompletion *completion,
                         char             **used,
                         const char        *column)
{
  g_autoptr(GString) s = NULL;

  s = g_string_new ("");

  if (used[0] != NULL)
    {
      int i;

      if (g_strv_contains ((const char * const *) used, column))
        return;

      const char *last = NULL;
      last = used[g_strv_length (used) - 1];
      if (!g_str_has_prefix (column, last))
        return;

      for (i = 0; used[i + 1]; i++)
        {
          g_string_append (s, used[i]);
          g_string_append_c (s, ',');
        }
    }

  g_string_append (s, column);
  flatpak_completion_debug ("completing column: %s", s->str);

  g_print ("%s\n", s->str);
}

void
flatpak_complete_columns (FlatpakCompletion *completion,
                          Column            *columns)
{
  int i;
  const char *list = NULL;
  g_auto(GStrv) used = NULL;

  if (!g_str_has_prefix (completion->cur, "--columns="))
    return;

  list = completion->cur + strlen ("--columns=");
  if (strcmp (list, "all") == 0 ||
      strcmp (list, "help") == 0)
    return;

  used = g_strsplit (list, ",", 0);
  flatpak_completion_debug ("complete columns, used: '%s'", list);

  if (g_strv_length (used) <= 1)
    {
      flatpak_complete_column (completion, used, "all");
      flatpak_complete_column (completion, used, "help");
    }

  for (i = 0; columns[i].name; i++)
    flatpak_complete_column (completion, used, columns[i].name);
}

void
flatpak_complete_context (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, flatpak_context_get_option_entries ());
}

static gchar *
pick_word_at (const char *s,
              int         cursor,
              int        *out_word_begins_at)
{
  int begin, end;

  if (s[0] == '\0')
    {
      if (out_word_begins_at != NULL)
        *out_word_begins_at = -1;
      return NULL;
    }

  if (is_word_separator (s[cursor]) && ((cursor > 0 && is_word_separator (s[cursor - 1])) || cursor == 0))
    {
      if (out_word_begins_at != NULL)
        *out_word_begins_at = cursor;
      return g_strdup ("");
    }

  while (cursor > 0 && !is_word_separator (s[cursor - 1]))
    cursor--;
  begin = cursor;

  end = begin;
  while (!is_word_separator (s[end]) && s[end] != '\0')
    end++;

  if (out_word_begins_at != NULL)
    *out_word_begins_at = begin;

  return g_strndup (s + begin, end - begin);
}

static gboolean
parse_completion_line_to_argv (const char        *initial_completion_line,
                               FlatpakCompletion *completion)
{
  gboolean parse_result = g_shell_parse_argv (initial_completion_line,
                                              &completion->original_argc,
                                              &completion->original_argv,
                                              NULL);

  /* Make a shallow copy of argv, which will be our "working set" */
  completion->argc = completion->original_argc;
  completion->argv = g_memdup (completion->original_argv,
                               sizeof (gchar *) * (completion->original_argc + 1));

  return parse_result;
}

FlatpakCompletion *
flatpak_completion_new (const char *arg_line,
                        const char *arg_point,
                        const char *arg_cur)
{
  FlatpakCompletion *completion;
  g_autofree char *initial_completion_line = NULL;
  int _point;
  char *endp;
  int cur_begin;
  int i;

  _point = strtol (arg_point, &endp, 10);
  if (endp == arg_point || *endp != '\0')
    return NULL;

  /* Ensure we're not going oob if we got weird arguments. */
  _point = MIN (_point, strlen (arg_line));

  completion = g_new0 (FlatpakCompletion, 1);
  completion->line = g_strdup (arg_line);
  completion->shell_cur = g_strdup (arg_cur);
  completion->point = _point;

  flatpak_completion_debug ("========================================");
  flatpak_completion_debug ("completion_point=%d", completion->point);
  flatpak_completion_debug ("completion_shell_cur='%s'", completion->shell_cur);
  flatpak_completion_debug ("----");
  flatpak_completion_debug (" 0123456789012345678901234567890123456789012345678901234567890123456789");
  flatpak_completion_debug ("'%s'", completion->line);
  flatpak_completion_debug (" %*s^", completion->point, "");

  /* compute cur and prev */
  completion->prev = NULL;
  completion->cur = pick_word_at (completion->line, completion->point, &cur_begin);
  if (cur_begin > 0)
    {
      gint prev_end;
      for (prev_end = cur_begin - 1; prev_end >= 0; prev_end--)
        {
          if (!is_word_separator (completion->line[prev_end]))
            {
              completion->prev = pick_word_at (completion->line, prev_end, NULL);
              break;
            }
        }

      initial_completion_line = g_strndup (completion->line, cur_begin);
    }
  else
    initial_completion_line = g_strdup ("");

  flatpak_completion_debug ("'%s'", initial_completion_line);
  flatpak_completion_debug ("----");

  flatpak_completion_debug (" cur='%s'", completion->cur);
  flatpak_completion_debug ("prev='%s'", completion->prev);

  if (!parse_completion_line_to_argv (initial_completion_line,
                                      completion))
    {
      /* it's very possible the command line can't be parsed (for
       * example, missing quotes etc) - in that case, we just
       * don't autocomplete at all
       */
      flatpak_completion_free (completion);
      return NULL;
    }

  flatpak_completion_debug ("completion_argv %i:", completion->original_argc);
  for (i = 0; i < completion->original_argc; i++)
    flatpak_completion_debug ("%s", completion->original_argv[i]);

  flatpak_completion_debug ("----");

  return completion;
}

void
flatpak_completion_free (FlatpakCompletion *completion)
{
  g_free (completion->cur);
  g_free (completion->prev);
  g_free (completion->line);
  g_free (completion->argv);
  g_free (completion->shell_cur);
  g_strfreev (completion->original_argv);
  g_free (completion);
}
