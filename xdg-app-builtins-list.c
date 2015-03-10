#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static gboolean opt_show_details;
static gboolean opt_user;
static gboolean opt_system;

static GOptionEntry options[] = {
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, "Show user installations", NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, "Show system-wide installations", NULL },
  { "show-details", 0, 0, G_OPTION_ARG_NONE, &opt_show_details, "Show arches and branches", NULL },
  { NULL }
};

static gboolean
print_installed_refs (const char *kind, gboolean print_system, gboolean print_user, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_strfreev gchar **refs = NULL;
  gs_free char *last_ref = NULL;
  gs_free char *last = NULL;
  gs_strfreev char **system = NULL;
  gs_strfreev char **user = NULL;
  int s, u;

  if (print_user)
    {
      gs_unref_object XdgAppDir *dir = NULL;

      dir = xdg_app_dir_get (TRUE);
      if (!xdg_app_dir_list_refs (dir, kind, &user, cancellable, error))
        goto out;
    }
  else
    user = g_new0 (char *, 1);

  if (print_system)
    {
      gs_unref_object XdgAppDir *dir = NULL;

      dir = xdg_app_dir_get (FALSE);
      if (!xdg_app_dir_list_refs (dir, kind, &system, cancellable, error))
        goto out;
    }
  else
    system = g_new0 (char *, 1);

  for (s = 0, u = 0; system[s] != NULL || user[u] != NULL; )
    {
      char *ref;
      gs_strfreev char **parts = NULL;
      gboolean is_user;

      if (system[s] == NULL)
        is_user = TRUE;
      else if (user[u] == NULL)
        is_user = FALSE;
      else if (strcmp (system[s], user[u]) <= 0)
        is_user = FALSE;
      else
        is_user = TRUE;

      if (is_user)
        ref = user[u++];
      else
        ref = system[s++];

      parts = g_strsplit (ref, "/", -1);

      if (opt_show_details)
        {
          g_print ("%s/%s/%s\t", parts[1], parts[2], parts[3]);
          if (print_user && print_system)
            g_print ("%s", is_user ? "user" : "system");
          g_print ("\n");
        }
      else
        {
          if (last == NULL || strcmp (last, parts[1]) != 0)
            {
              g_print ("%s\n", parts[1]);
              g_clear_pointer (&last, g_free);
              last = g_strdup (parts[1]);
            }
        }
    }

  ret = TRUE;

out:

  return ret;
}

gboolean
xdg_app_builtin_list_runtimes (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_strfreev char **system = NULL;
  gs_strfreev char **user = NULL;

  context = g_option_context_new (" - List installed runtimes");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (!print_installed_refs ("runtime",
                             opt_system || (!opt_user && !opt_system),
                             opt_user || (!opt_user && !opt_system),
                             cancellable, error))
    goto out;

  ret = TRUE;

 out:

  if (context)
    g_option_context_free (context);

  return ret;
}

gboolean
xdg_app_builtin_list_apps (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;

  context = g_option_context_new (" - List installed applications");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (!print_installed_refs ("app",
                             opt_system || (!opt_user && !opt_system),
                             opt_user || (!opt_user && !opt_system),
                             cancellable, error))
    goto out;

  ret = TRUE;

 out:

  if (context)
    g_option_context_free (context);

  return ret;
}
