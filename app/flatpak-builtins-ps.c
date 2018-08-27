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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
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
#include "flatpak-table-printer.h"
#include "flatpak-run-private.h"


static GOptionEntry options[] = {
  { NULL }
};

static char *
get_instance_pid (const char *instance)
{
  g_autofree char *path = NULL;
  char *pid;
  g_autoptr(GError) error = NULL;

  path = g_build_filename (g_get_user_runtime_dir (), ".flatpak", instance, "pid", NULL);

  if (!g_file_get_contents (path, &pid, NULL, &error))
    {
      g_debug ("Failed to load pid file for instance '%s': %s", instance, error->message);
      return g_strdup ("?");
    }

  return pid; 
}

static char *
get_instance_app_id (const char *instance)
{
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) key_file = NULL;
  char *app_id = NULL;
  g_autoptr(GError) error = NULL;

  path = g_build_filename (g_get_user_runtime_dir (), ".flatpak", instance, "info", NULL);

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error))
    {
      g_debug ("Failed to load info file for instance '%s': %s", instance, error->message);
      goto out;
    }

  app_id = g_key_file_get_string (key_file, "Application", "name", &error);
  if (error)
    {
      g_debug ("Failed to get app ID for instance '%s': %s", instance, error->message);
    }

out:
  return app_id ? app_id : g_strdup ("?");
}

static gboolean
enumerate_instances (GError **error)
{
  g_autofree char *base_dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileEnumerator) enumerator = NULL;
  GFileInfo *info;
  FlatpakTablePrinter *printer;

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_column_title (printer, 0, _("Instance"));
  flatpak_table_printer_set_column_title (printer, 1, _("Application"));
  flatpak_table_printer_set_column_title (printer, 2, _("PID"));
 
  flatpak_run_gc_ids ();

  base_dir = g_build_filename (g_get_user_runtime_dir (), ".flatpak", NULL);
  file = g_file_new_for_path (base_dir);
  enumerator = g_file_enumerate_children (file, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, error);
  if (enumerator == NULL)
    return FALSE;

  while ((info = g_file_enumerator_next_file (enumerator, NULL, error)) != NULL)
    {
      g_autofree char *instance = g_file_info_get_attribute_as_string (info, "standard::name");
      g_autofree char *app_id = get_instance_app_id (instance);
      g_autofree char *pid = get_instance_pid (instance);

      flatpak_table_printer_add_column (printer, instance);
      flatpak_table_printer_add_column (printer, app_id);
      flatpak_table_printer_add_column (printer, pid);
      flatpak_table_printer_finish_row (printer);

      g_object_unref (info);
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  if (*error)
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_ps (int           argc,
                    char        **argv,
                    GCancellable *cancellable,
                    GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new (_(" - Enumerate running sandboxes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc > 1)
    {
      usage_error (context, _("Extra arguments given"), error);
      return FALSE;
    }

  return enumerate_instances (error);
}

gboolean
flatpak_complete_ps (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1:
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      break;

    default:
      break;
    }

  return TRUE;
}
