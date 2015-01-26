#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"


gboolean
xdg_app_builtin_repo_update (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object GFile *repofile = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *location;

  context = g_option_context_new ("LOCATION - Update repository metadata");

  if (!xdg_app_option_context_parse (context, NULL, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "LOCATION must be specified", error);
      goto out;
    }

  location = argv[1];

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!ostree_repo_open (repo, cancellable, error))
    goto out;

  if (!ostree_repo_regenerate_summary (repo, NULL, cancellable, error))
    goto out;

  /* TODO: appstream data */

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);

  return ret;
}
