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
  gs_unref_ptrarray GPtrArray *names = NULL;
  int i;
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

  names = g_ptr_array_new_with_free_func (g_free);

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

      if (name)
        {
          for (i = 0; i < names->len; i++)
            {
              int cmp;

              cmp = strcmp (name, g_ptr_array_index (names, i));
              if (cmp > 0)
                continue;
              else if (cmp < 0)
                g_ptr_array_insert (names, i, name);
              else
                g_free (name);
              break;
            }
          if (i == names->len)
            g_ptr_array_insert (names, i, name);
        }
    }

  for (i = 0; i < names->len; i++)
    g_print ("%s\n", (char *)g_ptr_array_index (names, i));

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);

  return ret;
}
