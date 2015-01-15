#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static gboolean opt_show_details;

static GOptionEntry options[] = {
  { "show-details", 0, 0, G_OPTION_ARG_NONE, &opt_show_details, "Show arches and branches", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_list_runtimes (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *base = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;
  gs_unref_object GFile *child = NULL;
  gs_unref_object GFileEnumerator *dir_enum2 = NULL;
  gs_unref_object GFileInfo *child_info2 = NULL;
  gs_unref_object GFile *child2 = NULL;
  gs_unref_object GFileEnumerator *dir_enum3 = NULL;
  gs_unref_object GFileInfo *child_info3 = NULL;
  gs_unref_object GFile *child3 = NULL;
  GList *runtimes = NULL;
  GList *l;
  gint max_length[2];
  GError *temp_error = NULL;

  context = g_option_context_new (" - List runtimes");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  base = g_file_resolve_relative_path (xdg_app_dir_get_path (dir), "runtime");
  if (!g_file_query_exists (base, cancellable))
    {
      ret = TRUE;
      goto out;
    }

  dir_enum = g_file_enumerate_children (base, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  if (opt_show_details)
    {
      while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
        {
          const char *name;

          name = g_file_info_get_name (child_info);

          g_clear_object (&child);
          child = g_file_get_child (base, name);
          g_clear_object (&dir_enum2);
          dir_enum2 = g_file_enumerate_children (child, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                 cancellable, error);
          if (!dir_enum2)
            goto out;

          while ((child_info2 = g_file_enumerator_next_file (dir_enum2, cancellable, &temp_error)))
            {
              const char *arch;

              arch = g_file_info_get_name (child_info2);

              g_clear_object (&child2);
              child2 = g_file_get_child (child, arch);
              g_clear_object (&dir_enum3);
              dir_enum3 = g_file_enumerate_children (child2, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                     cancellable, error);
              if (!dir_enum3)
                goto out;

              while ((child_info3 = g_file_enumerator_next_file (dir_enum3, cancellable, &temp_error)))
                {
                  const char *branch;
                  char **r;

                  branch = g_file_info_get_name (child_info3);

                  r = g_new (gchar*, 4);
                  r[0] = g_strdup (name);
                  r[1] = g_strdup (arch);
                  r[2] = g_strdup (branch);
                  r[3] = NULL;
                  runtimes = g_list_prepend (runtimes, r);

                  g_clear_object (&child_info3);
                }

              if (temp_error != NULL)
                goto out;

              g_clear_object (&child_info2);
            }

          if (temp_error != NULL)
            goto out;

          g_clear_object (&child_info);
        }

      if (temp_error != NULL)
        goto out;

      runtimes = g_list_reverse (runtimes);

      max_length[0] = max_length[1] = 0;
      for (l = runtimes; l; l = l->next)
        {
          gchar **r = l->data;
          max_length[0] = MAX (max_length[0], strlen (r[0]));
          max_length[1] = MAX (max_length[1], strlen (r[1]));
        }
      for (l = runtimes; l; l = l->next)
        {
          gchar **r = l->data;
          g_print ("%-*s  %-*s  %s\n", max_length[0], r[0], max_length[1], r[1], r[2]);
        }
    }
  else
    {
      while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
        {
          g_print ("%s\n", g_file_info_get_name (child_info));
          g_clear_object (&child_info);
        }

      if (temp_error != NULL)
        goto out;
    }

  ret = TRUE;

 out:

  g_list_free_full (runtimes, (GDestroyNotify)g_strfreev);

  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  if (context)
    g_option_context_free (context);

  return ret;
}
