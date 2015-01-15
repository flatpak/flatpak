#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"

static gboolean opt_show_urls;

static GOptionEntry options[] = {
  { "show-urls", 0, 0, G_OPTION_ARG_NONE, &opt_show_urls, "Show remote URLs in list", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_list_repos (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_strfreev char **remotes = NULL;
  guint ii, n_remotes = 0;

  context = g_option_context_new (" - List remote repositories");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  remotes = ostree_repo_remote_list (xdg_app_dir_get_repo (dir), &n_remotes);

  if (opt_show_urls)
    {
      int max_length = 0;

      for (ii = 0; ii < n_remotes; ii++)
        max_length = MAX (max_length, strlen (remotes[ii]));

      for (ii = 0; ii < n_remotes; ii++)
        {
          gs_free char *remote_url = NULL;

          if (!ostree_repo_remote_get_url (xdg_app_dir_get_repo (dir), remotes[ii], &remote_url, error))
            goto out;

          g_print ("%-*s  %s\n", max_length, remotes[ii], remote_url);
        }
    }
  else
    {
      for (ii = 0; ii < n_remotes; ii++)
        g_print ("%s\n", remotes[ii]);
    }

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
