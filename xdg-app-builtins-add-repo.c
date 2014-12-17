#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"

static gboolean opt_no_gpg_verify;
static gboolean opt_if_not_exists;

static GOptionEntry options[] = {
  { "no-gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_gpg_verify, "Disable GPG verification", NULL },
  { "if-not-exists", 0, 0, G_OPTION_ARG_NONE, &opt_if_not_exists, "Do nothing if the provided remote exists", NULL },
  { NULL }
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gs_free gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

gboolean
xdg_app_builtin_add_repo (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *basedir = NULL;
  gs_unref_variant_builder GVariantBuilder *optbuilder = NULL;
  const char *remote_name;
  const char *remote_url;

  context = g_option_context_new ("NAME URL - Add a remote repository");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &repo, &basedir, cancellable, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, "NAME and URL must be specified", error);
      goto out;
    }

  remote_name = argv[1];
  remote_url  = argv[2];

  optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  if (opt_no_gpg_verify)
    g_variant_builder_add (optbuilder, "{s@v}",
                           "gpg-verify",
                           g_variant_new_variant (g_variant_new_boolean (FALSE)));


  if (!ostree_repo_remote_change (repo, NULL,
                                  opt_if_not_exists ? OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS :
                                  OSTREE_REPO_REMOTE_CHANGE_ADD,
                                  remote_name, remote_url,
                                  g_variant_builder_end (optbuilder),
                                  cancellable, error))
    goto out;

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
