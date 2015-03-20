#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_title;

static GOptionEntry options[] = {
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, "A nice name to use for this repository", "TITLE" },
  { NULL }
};


gboolean
xdg_app_builtin_repo_update (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  GVariant *extra = NULL;

  context = g_option_context_new ("LOCATION - Update repository metadata");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
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

  if (opt_title)
    {
      GVariantBuilder builder;

      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&builder, "{sv}", "xa.title", g_variant_new_string (opt_title));
      extra = g_variant_builder_end (&builder);
    }

  if (!ostree_repo_regenerate_summary (repo, extra, cancellable, error))
    goto out;

  /* TODO: appstream data */

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);

  return ret;
}
