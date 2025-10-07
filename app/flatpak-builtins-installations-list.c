#include "config.h"

#include <glib/gi18n.h>

#include "flatpak-builtins.h"
#include "flatpak-table-printer.h"

static const char **opt_cols;

static GOptionEntry options[] = {
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { NULL }
};

static Column all_columns[] = {
  { "id",           N_("Id"),           N_("Show the name"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "name",         N_("Name"),         N_("Show the name"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "path",         N_("Path"),         N_("Show the name"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "storage-type", N_("Storage type"), N_("Show the name"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "priority",     N_("Priority"),     N_("Show the name"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "scope",        N_("Scope"),        N_("Show the scope"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { NULL }
};

static const char *
storage_type_to_string (FlatpakDirStorageType storage_type)
{
   switch (storage_type) {
    case FLATPAK_DIR_STORAGE_TYPE_DEFAULT:
      return _("Default");
    case FLATPAK_DIR_STORAGE_TYPE_HARD_DISK:
      return _("Harddisk");
    case FLATPAK_DIR_STORAGE_TYPE_SDCARD:
      return _("SD Card");
    case FLATPAK_DIR_STORAGE_TYPE_MMC:
      return _("MMC");
    case FLATPAK_DIR_STORAGE_TYPE_NETWORK:
      return _("Network");
    default:
      return _("Unknown");
   }
}

static gboolean
list_installations (Column *columns, GCancellable *cancellable, GError **error)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i, c;

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_columns (printer, columns,
                                     opt_cols == NULL);

  dirs = flatpak_dir_get_list (cancellable, error);
  if (dirs == NULL)
    return FALSE;

  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);

      for (c = 0; columns[c].name; c++)
        {
          if (strcmp (columns[c].name, "id") == 0)
            flatpak_table_printer_add_column (printer, flatpak_dir_get_id (dir));
          if (strcmp (columns[c].name, "name") == 0)
            flatpak_table_printer_add_column (printer, flatpak_dir_get_display_name (dir));
          if (strcmp (columns[c].name, "path") == 0)
            flatpak_table_printer_add_column (printer, g_file_get_path (flatpak_dir_get_path (dir)));
          if (strcmp (columns[c].name, "storage-type") == 0)
            flatpak_table_printer_add_column (printer, storage_type_to_string (flatpak_dir_get_storage_type (dir)));
          if (strcmp (columns[c].name, "priority") == 0)
            flatpak_table_printer_add_column (printer, g_strdup_printf ("%d", flatpak_dir_get_priority (dir)));
          if (strcmp (columns[c].name, "scope") == 0)
            {
              if (flatpak_dir_is_user (dir))
                flatpak_table_printer_add_column (printer, _("User"));
              else
                flatpak_table_printer_add_column (printer, _("System"));
            }
        }

      flatpak_table_printer_finish_row (printer);
    }

  flatpak_table_printer_print (printer);

  return TRUE;
}

gboolean
flatpak_builtin_installations_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree Column *columns = NULL;
  g_autofree char *col_help = NULL;

  context = g_option_context_new (_(" - Show installations"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  col_help = column_help (all_columns);
  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  columns = handle_column_args (all_columns, FALSE, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  return list_installations (columns, cancellable, error);
}

gboolean
flatpak_complete_installations_list (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, options);
  flatpak_complete_columns (completion, all_columns);
  return TRUE;
}
