#include "config.h"

#include <glib/gi18n.h>

#include "flatpak-builtins.h"
#include "flatpak-table-printer.h"

static gboolean opt_system;

static GOptionEntry options[] = {
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, N_("Modify system installations"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_installations_remove (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  const char *id;

  context = g_option_context_new (_(" - Remove installation [ID]"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("Id must be specified"), error);

  id = argv[1];

  if (opt_system)
    return flatpak_dir_remove_system_installation (id, cancellable, error);
  else
    return flatpak_dir_remove_user_installation (id, cancellable, error);
}

gboolean
flatpak_complete_installations_remove (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, options);
  return TRUE;
}
