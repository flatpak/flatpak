#include "config.h"

#include <glib/gi18n.h>

#include "flatpak-builtins.h"
#include "flatpak-table-printer.h"

static char *opt_display_name;
static char *opt_storage_type;
static gboolean opt_system;
static int opt_priority;

static GOptionEntry options[] = {
  { "display-name", 0, 0, G_OPTION_ARG_STRING, &opt_display_name, N_("Set the display name"),        N_("Name")     },
  { "storage-type", 0, 0, G_OPTION_ARG_STRING, &opt_storage_type, N_("Set the storage type"),        N_("Type")     },
  { "priority",     0, 0, G_OPTION_ARG_INT,    &opt_priority,     N_("Set the priority"),            N_("Priority") },
  { "system",       0, 0, G_OPTION_ARG_NONE,   &opt_system,       N_("Modify system installations"), NULL           },
  { NULL }
};

gboolean
flatpak_builtin_installations_add (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  FlatpakDirStorageType storage_type;
  const char *path;
  const char *id;

  context = g_option_context_new (_(" - Add installation [ID] [PATH]"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("Id must be specified"), error);

  if (argc < 3)
    return usage_error (context, _("Path must be specified"), error);

  id = argv[1];
  path = argv[2];

  if (opt_storage_type != NULL)
    {
      storage_type = parse_storage_type (opt_storage_type);
      if (storage_type == FLATPAK_DIR_STORAGE_TYPE_DEFAULT)
        return usage_error (context, _("Invalid storage type. Valid types are harddisk, sdcard, mmc or network."), error);
    }
  else
    storage_type = FLATPAK_DIR_STORAGE_TYPE_DEFAULT;

  if (!g_file_test (path, G_FILE_TEST_IS_DIR))
    return flatpak_fail (error, _("Directory %s does not exists"), path);

  if (opt_system)
    return flatpak_dir_add_system_installation (id, path, opt_display_name, storage_type, opt_priority, cancellable, error);
  else
    return flatpak_dir_add_user_installation (id, path, opt_display_name, storage_type, opt_priority, cancellable, error);

  return TRUE;
}

gboolean
flatpak_complete_installations_add (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, options);
  return TRUE;
}
