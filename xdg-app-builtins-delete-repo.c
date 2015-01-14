#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gs_free gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

gboolean
xdg_app_builtin_delete_repo (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_object XdgAppDir *dir = NULL;
  const char *remote_name;

  context = g_option_context_new ("NAME - Delete a remote repository");

  if (!xdg_app_option_context_parse (context, NULL, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "NAME must be specified", error);
      goto out;
    }

  remote_name = argv[1];

  if (!ostree_repo_remote_change (xdg_app_dir_get_repo (dir), NULL,
                                  OSTREE_REPO_REMOTE_CHANGE_DELETE,
                                  remote_name, NULL,
                                  NULL,
                                  cancellable, error))
    goto out;

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
