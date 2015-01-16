#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static gboolean opt_show_details;
static gboolean opt_only_runtimes;
static gboolean opt_only_apps;

static GOptionEntry options[] = {
  { "show-details", 0, 0, G_OPTION_ARG_NONE, &opt_show_details, "Show arches and branches", NULL },
  { "runtimes", 0, 0, G_OPTION_ARG_NONE, &opt_only_runtimes, "Show only runtimes", NULL },
  { "apps", 0, 0, G_OPTION_ARG_NONE, &opt_only_apps, "Show only apps", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_repo_contents (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_hashtable GHashTable *refs = NULL;
  GHashTableIter iter;
  gpointer key;
  gs_unref_hashtable GHashTable *seen = NULL;
  const char *repository;

  context = g_option_context_new (" REPOSITORY - Show available runtimes and applications");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "REPOSITORY must be specified", error);
      goto out;
    }

  repository = argv[1];

  repo = xdg_app_dir_get_repo (dir);
  if (!ostree_repo_list_refs (repo, NULL, &refs, cancellable, error))
    goto out;

  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_hash_table_iter_init (&iter, refs);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      const char *refspec = key;
      gs_free char *remote = NULL;
      gs_free char *ref = NULL;
      char *name = NULL;
      char *p;

      if (!ostree_parse_refspec (refspec, &remote, &ref, error))
        goto out;

      if (!g_str_equal (remote, repository))
        continue;

      if (g_str_has_prefix (ref, "runtime/") && !opt_only_apps)
        {
          name = g_strdup (ref + strlen ("runtime/"));
          if (!opt_show_details)
            {
              p = strchr (name, '/');
              if (p)
                *p = 0;
            }
        }

      if (g_str_has_prefix (ref, "app/") && !opt_only_runtimes)
        {
          name = g_strdup (ref + strlen ("app/"));
          if (!opt_show_details)
            {
              p = strchr (name, '/');
              if (p)
                *p = 0;
            }
        }

      if (name && !g_hash_table_contains (seen, name))
        {
          g_hash_table_add (seen, name);
          g_print ("%s\n", name);
        }
    }

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);

  return ret;
}
