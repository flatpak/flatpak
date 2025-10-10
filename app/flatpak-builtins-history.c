/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2018 Red Hat, Inc
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
 *       Matthias Clasen
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>

#include <glib/gi18n.h>

#include "libglnx.h"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"

static char *opt_since;
static char *opt_until;
static gboolean opt_reverse;
static const char **opt_cols;
static gboolean opt_json;

static GOptionEntry options[] = {
  { "since", 0, 0, G_OPTION_ARG_STRING, &opt_since, N_("Only show changes after TIME"), N_("TIME") },
  { "until", 0, 0, G_OPTION_ARG_STRING, &opt_until, N_("Only show changes before TIME"), N_("TIME") },
  { "reverse", 0, 0, G_OPTION_ARG_NONE, &opt_reverse, N_("Show newest entries first"), NULL },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL }
};

static Column all_columns[] = {
  { "time",         N_("Time"),         N_("Show when the change happened"),   0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "change",       N_("Change"),       N_("Show the kind of change"),         0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "ref",          N_("Ref"),          N_("Show the ref"),                    0, FLATPAK_ELLIPSIZE_MODE_NONE, 0, 0 },
  { "application",  N_("Application"),  N_("Show the application/runtime ID"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "arch",         N_("Arch"),         N_("Show the architecture"),           0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "branch",       N_("Branch"),       N_("Show the branch"),                 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "installation", N_("Installation"), N_("Show the affected installation"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "remote",       N_("Remote"),       N_("Show the remote"),                 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "commit",       N_("Commit"),       N_("Show the current commit"),         0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "old-commit",   N_("Old Commit"),   N_("Show the previous commit"),        0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "url",          N_("URL"),          N_("Show the remote URL"),             0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "user",         N_("User"),         N_("Show the user doing the change"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "tool",         N_("Tool"),         N_("Show the tool that was used"),     0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "version",      N_("Version"),      N_("Show the Flatpak version"),        0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { NULL }
};

#ifdef HAVE_LIBSYSTEMD

static char *
get_field (sd_journal *j,
           const char *name,
           GError    **error)
{
  const char *data;
  gsize len;
  int r;

  if ((r = sd_journal_get_data (j, name, (const void **) &data, &len)) < 0)
    {
      if (r != -ENOENT)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("Failed to get journal data (%s): %s"),
                     name, strerror (-r));

      return NULL;
    }

  return g_strndup (data + strlen (name) + 1, len - (strlen (name) + 1));
}

static GDateTime *
get_time (sd_journal *j,
          GError    **error)
{
  g_autofree char *value = NULL;
  GError *local_error = NULL;
  gint64 t;

  value = get_field (j, "_SOURCE_REALTIME_TIMESTAMP", &local_error);

  if (local_error)
    {
      g_propagate_error (error, local_error);
      return NULL;
    }

  t = g_ascii_strtoll (value, NULL, 10) / 1000000;
  return g_date_time_new_from_unix_local (t);
}

static gboolean
print_history (GPtrArray    *dirs,
               Column       *columns,
               GDateTime    *since,
               GDateTime    *until,
               gboolean      reverse,
               GCancellable *cancellable,
               GError      **error)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  sd_journal *j;
  int r;
  int i;
  int k;
  int ret;

  if (columns[0].name == NULL)
    return TRUE;

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_columns (printer, columns, opt_cols == NULL);

  if ((r = sd_journal_open (&j, 0)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Failed to open journal: %s"), strerror (-r));
      return FALSE;
    }

  if ((r = sd_journal_add_match (j, "MESSAGE_ID=" FLATPAK_MESSAGE_ID, 0)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Failed to add match to journal: %s"), strerror (-r));
      return FALSE;
    }

  if (reverse)
    ret = sd_journal_seek_tail (j);
  else
    ret = sd_journal_seek_head (j);
  if (ret == 0)
    while ((reverse && sd_journal_previous (j) > 0) ||
           (!reverse && sd_journal_next (j) > 0))
      {
        g_autofree char *ref_str = NULL;
        g_autofree char *remote = NULL;

        /* determine whether to skip this entry */

        ref_str = get_field (j, "REF", error);
        if (*error)
          return FALSE;

        /* Appstream pulls are probably not interesting, and they are confusing
         * since by default we include the Application column which shows up blank.
         */
        if (ref_str && ref_str[0] && g_str_has_prefix (ref_str, "appstream"))
          continue;

        remote = get_field (j, "REMOTE", error);
        if (*error)
          return FALSE;

        /* Exclude pull to temp repo */
        if (remote && remote[0] == '/')
          continue;

        if (dirs)
          {
            gboolean include = FALSE;
            g_autofree char *installation = get_field (j, "INSTALLATION", NULL);

            for (i = 0; i < dirs->len && !include; i++)
              {
                g_autofree char *name = flatpak_dir_get_name (dirs->pdata[i]);
                if (g_strcmp0 (name, installation) == 0)
                  include = TRUE;
              }
            if (!include)
              continue;
          }

        if (since || until)
          {
            g_autoptr(GDateTime) time = get_time (j, NULL);

            if (since && time && g_date_time_difference (since, time) >= 0)
              continue;

            if (until && time && g_date_time_difference (until, time) <= 0)
              continue;
          }

        for (k = 0; columns[k].name; k++)
          {
            if (strcmp (columns[k].name, "time") == 0)
              {
                g_autoptr(GDateTime) time = NULL;
                g_autofree char *s = NULL;

                time = get_time (j, error);
                if (*error)
                  return FALSE;

                s = g_date_time_format (time, "%b %e %T");
                flatpak_table_printer_add_column (printer, s);
              }
            else if (strcmp (columns[k].name, "change") == 0)
              {
                g_autofree char *op = get_field (j, "OPERATION", error);
                if (*error)
                  return FALSE;
                flatpak_table_printer_add_column (printer, op);
              }
            else if (strcmp (columns[k].name, "ref") == 0 ||
                     strcmp (columns[k].name, "application") == 0 ||
                     strcmp (columns[k].name, "arch") == 0 ||
                     strcmp (columns[k].name, "branch") == 0)
              {
                g_autofree char *value = NULL;

                if (ref_str && ref_str[0] &&
                    !flatpak_is_app_runtime_or_appstream_ref (ref_str) &&
                    g_strcmp0 (ref_str, OSTREE_REPO_METADATA_REF) != 0)
                  g_warning ("Unknown ref in history: %s", ref_str);

                if (strcmp (columns[k].name, "ref") == 0)
                  value = g_strdup (ref_str);
                else if (ref_str && ref_str[0] &&
                         (g_str_has_prefix (ref_str, "app/") ||
                          g_str_has_prefix (ref_str, "runtime/")))
                  {
                    g_autoptr(FlatpakDecomposed) ref = NULL;
                    ref = flatpak_decomposed_new_from_ref (ref_str, NULL);
                    if (ref == NULL)
                      g_warning ("Invalid ref in history: %s", ref_str);
                    else
                      {
                        if (strcmp (columns[k].name, "application") == 0)
                          value = flatpak_decomposed_dup_id (ref);
                        else if (strcmp (columns[k].name, "arch") == 0)
                          value = flatpak_decomposed_dup_arch (ref);
                        else
                          value = flatpak_decomposed_dup_branch (ref);
                      }
                  }

                  flatpak_table_printer_add_column (printer, value);
              }
            else if (strcmp (columns[k].name, "installation") == 0)
              {
                g_autofree char *installation = get_field (j, "INSTALLATION", error);
                if (*error)
                  return FALSE;
                flatpak_table_printer_add_column (printer, installation);
              }
            else if (strcmp (columns[k].name, "remote") == 0)
              {
                flatpak_table_printer_add_column (printer, remote);
              }
            else if (strcmp (columns[k].name, "commit") == 0)
              {
                g_autofree char *commit = get_field (j, "COMMIT", error);
                if (*error)
                  return FALSE;
                flatpak_table_printer_add_column_len (printer, commit, 12);
              }
            else if (strcmp (columns[k].name, "old-commit") == 0)
              {
                g_autofree char *old_commit = get_field (j, "OLD_COMMIT", error);
                if (*error)
                  return FALSE;
                flatpak_table_printer_add_column_len (printer, old_commit, 12);
              }
            else if (strcmp (columns[k].name, "url") == 0)
              {
                g_autofree char *url = get_field (j, "URL", error);
                if (*error)
                  return FALSE;
                flatpak_table_printer_add_column (printer, url);
              }
            else if (strcmp (columns[k].name, "user") == 0)
              {
                g_autofree char *id = get_field (j, "_UID", error);
                g_autofree char *oid = NULL;
                int uid;
                struct passwd *pwd;

                if (*error)
                  return FALSE;

                uid = g_ascii_strtoll (id, NULL, 10);
                pwd = getpwuid (uid);
                if (pwd)
                  {
                    g_free (id);
                    id = g_strdup (pwd->pw_name);
                  }

                oid = get_field (j, "OBJECT_UID", NULL);
                if (oid)
                  {
                    /* flatpak-system-helper acting on behalf of sb else */
                    g_autofree char *str = NULL;
                    uid = g_ascii_strtoll (oid, NULL, 10);
                    pwd = getpwuid (uid);
                    str = g_strdup_printf ("%s (%s)", id, pwd ? pwd->pw_name : oid);
                    flatpak_table_printer_add_column (printer, str);
                  }
                else
                  flatpak_table_printer_add_column (printer, id);
              }
            else if (strcmp (columns[k].name, "tool") == 0)
              {
                g_autofree char *exe = get_field (j, "_EXE", error);
                g_autofree char *oexe = NULL;
                g_autofree char *tool = NULL;
                if (*error)
                  return FALSE;
                tool = g_path_get_basename (exe);
                oexe = get_field (j, "OBJECT_EXE", NULL);
                if (oexe)
                  {
                    /* flatpak-system-helper acting on behalf of sb else */
                    g_autofree char *otool = NULL;
                    g_autofree char *str = NULL;

                    otool = g_path_get_basename (oexe);
                    str = g_strdup_printf ("%s (%s)", tool, otool);
                    flatpak_table_printer_add_column (printer, str);
                  }
                else
                  flatpak_table_printer_add_column (printer, tool);
              }
            else if (strcmp (columns[k].name, "version") == 0)
              {
                g_autofree char *version = get_field (j, "FLATPAK_VERSION", error);
                if (*error)
                  return FALSE;
                flatpak_table_printer_add_column (printer, version);
              }
          }

        flatpak_table_printer_finish_row (printer);
      }

  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);

  sd_journal_close (j);

  return TRUE;
}

#else

static gboolean
print_history (GPtrArray    *dirs,
               Column       *columns,
               GDateTime    *since,
               GDateTime    *until,
               gboolean      reverse,
               GCancellable *cancellable,
               GError      **error)
{
  if (columns[0].name == NULL)
    return TRUE;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "history not available without libsystemd");
  return FALSE;
}

#endif

static GDateTime *
parse_time (const char *since_opt)
{
  g_autoptr(GDateTime) now = NULL;
  g_auto(GStrv) parts = NULL;
  int i;
  int days = 0;
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  const char *fmts[] = {
    "%H:%M",
    "%H:%M:%S",
    "%Y-%m-%d",
    "%Y-%m-%d %H:%M:%S"
  };

  now = g_date_time_new_now_local ();

  for (i = 0; i < G_N_ELEMENTS (fmts); i++)
    {
      const char *rest;
      struct tm tm;

      tm.tm_year = g_date_time_get_year (now);
      tm.tm_mon = g_date_time_get_month (now);
      tm.tm_mday = g_date_time_get_day_of_month (now);
      tm.tm_hour = 0;
      tm.tm_min = 0;
      tm.tm_sec = 0;

      rest = strptime (since_opt, fmts[i], &tm);
      if (rest && *rest == '\0')
        return g_date_time_new_local (tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

  parts = g_strsplit (since_opt, " ", -1);

  for (i = 0; parts[i]; i++)
    {
      gint64 n;
      char *end;

      n = g_ascii_strtoll (parts[i], &end, 10);
      if (g_strcmp0 (end, "d") == 0 ||
          g_strcmp0 (end, "day") == 0 ||
          g_strcmp0 (end, "days") == 0)
        days = (int) n;
      else if (g_strcmp0 (end, "h") == 0 ||
               g_strcmp0 (end, "hour") == 0 ||
               g_strcmp0 (end, "hours") == 0)
        hours = (int) n;
      else if (g_strcmp0 (end, "m") == 0 ||
               g_strcmp0 (end, "minute") == 0 ||
               g_strcmp0 (end, "minutes") == 0)
        minutes = (int) n;
      else if (g_strcmp0 (end, "s") == 0 ||
               g_strcmp0 (end, "second") == 0 ||
               g_strcmp0 (end, "seconds") == 0)
        seconds = (int) n;
      else
        return NULL;
    }

  return g_date_time_add_full (now, 0, 0, -days, -hours, -minutes, -seconds);
}

gboolean
flatpak_builtin_history (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GDateTime) since = NULL;
  g_autoptr(GDateTime) until = NULL;
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;

  context = g_option_context_new (_(" - Show history"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  if (opt_since)
    {
      since = parse_time (opt_since);
      if (!since)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Failed to parse the --since option"));
          return FALSE;
        }
    }

  if (opt_until)
    {
      until = parse_time (opt_until);
      if (!until)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Failed to parse the --until option"));
          return FALSE;
        }
    }
  columns = handle_column_args (all_columns, FALSE, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  if (!print_history (dirs, columns, since, until, opt_reverse, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_history (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, user_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_columns (completion, all_columns);
  return TRUE;
}
