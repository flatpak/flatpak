/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2016 Red Hat, Inc
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

#include <glib/gi18n.h>

#include "libglnx.h"
#include "flatpak-document-dbus-generated.h"

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"
#include "flatpak-table-printer.h"

static const char **opt_cols;
static gboolean opt_json;

static GOptionEntry options[] = {
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…")  },
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL }
};

static Column all_columns[] = {
  { "id",          N_("ID"),          N_("Show the document ID"),              0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "path",        N_("Path"),        N_("Show the document path"),            0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "origin",      N_("Origin"),      N_("Show the document path"),            0, FLATPAK_ELLIPSIZE_MODE_NONE, 1 },
  { "application", N_("Application"), N_("Show applications with permission"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "permissions", N_("Permissions"), N_("Show permissions for applications"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { NULL }
};

static gboolean
print_documents (const char   *app_id,
                 Column       *columns,
                 GCancellable *cancellable,
                 GError      **error)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusDocuments *documents;
  g_autoptr(GVariant) apps = NULL;
  g_autoptr(GVariantIter) iter = NULL;
  const char *id;
  const char *origin;
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  g_autofree char *mountpoint = NULL;
  gboolean need_perms = FALSE;
  gboolean found_documents_to_print = FALSE;
  int i;

  if (columns[0].name == NULL)
    return TRUE;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, error);

  if (documents == NULL)
    return FALSE;

  if (!xdp_dbus_documents_call_list_sync (documents, app_id ? app_id : "", &apps, NULL, error))
    return FALSE;

  if (!xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint, NULL, error))
    return FALSE;

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_columns (printer, columns, opt_cols == NULL);
  for (i = 0; columns[i].name; i++)
    {
      if (strcmp (columns[i].name, "permissions") == 0 ||
          strcmp (columns[i].name, "application") == 0)
        {
          need_perms = TRUE;
          break;
        }
    }

  iter = g_variant_iter_new (apps);
  while (g_variant_iter_next (iter, "{&s^&ay}", &id, &origin))
    {
      g_autoptr(GVariant) apps2 = NULL;
      g_autoptr(GVariantIter) iter2 = NULL;
      const char *app_id2 = NULL;
      const char **perms = NULL;
      gboolean have_perms = FALSE, just_perms = FALSE;

      if (need_perms)
        {
          g_autofree char *origin2 = NULL;
          if (!xdp_dbus_documents_call_info_sync (documents, id, &origin2, &apps2, NULL, error))
            return FALSE;
          iter2 = g_variant_iter_new (apps2);
          have_perms = g_variant_iter_next (iter2, "{&s^a&s}", &app_id2, &perms);
        }

      do
        {
          for (i = 0; columns[i].name; i++)
            {
              if (strcmp (columns[i].name, "application") == 0)
                flatpak_table_printer_add_column (printer, app_id2);
              else if (strcmp (columns[i].name, "permissions") == 0)
                {
                  g_autofree char *value = NULL;
                  if (perms)
                    value = g_strjoinv (" ", (char **) perms);
                  flatpak_table_printer_add_column (printer, value);
                }
              else if (just_perms)
                flatpak_table_printer_add_column (printer, "");
              else if (strcmp (columns[i].name, "id") == 0)
                flatpak_table_printer_add_column (printer, id);
              else if (strcmp (columns[i].name, "origin") == 0)
                flatpak_table_printer_add_column (printer, origin);
              else if (strcmp (columns[i].name, "path") == 0)
                {
                  g_autofree char *basename = g_path_get_basename (origin);
                  g_autofree char *path = g_build_filename (mountpoint, id, basename, NULL);
                  flatpak_table_printer_add_column (printer, path);
                }
            }

          flatpak_table_printer_finish_row (printer);
          found_documents_to_print = TRUE;

          just_perms = TRUE;
        } while (have_perms && g_variant_iter_next (iter2, "{&s^a&s}", &app_id2, &perms));
    }
    if (!found_documents_to_print)
      {
        g_print (_("No documents found\n"));
        return TRUE;
      }
    
  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);

  return TRUE;
}

gboolean
flatpak_builtin_document_list (int argc, char **argv,
                               GCancellable *cancellable,
                               GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  const char *app_id = NULL;
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;

  context = g_option_context_new (_("[APPID] - List exported files"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  if (argc == 2)
    app_id = argv[1];

  columns = handle_column_args (all_columns, FALSE, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  return print_documents (app_id, columns, cancellable, error);
}

gboolean
flatpak_complete_document_list (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(GError) error = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* APPID */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_columns (completion, all_columns);

      user_dir = flatpak_dir_get_user ();
      {
        g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (user_dir, NULL, NULL, NULL,
                                                                     FLATPAK_KINDS_APP,
                                                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                                                     &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);

        flatpak_complete_ref_id (completion, refs);
      }

      system_dir = flatpak_dir_get_system_default ();
      {
        g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (system_dir, NULL, NULL, NULL,
                                                                     FLATPAK_KINDS_APP,
                                                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                                                     &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);

        flatpak_complete_ref_id (completion, refs);
      }

      break;
    }

  return TRUE;
}
