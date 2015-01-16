#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
xdg_app_builtin_uninstall_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  const char *runtime;
  const char *arch = NULL;
  const char *branch = NULL;
  gs_free char *ref = NULL;

  context = g_option_context_new ("RUNTIME [ARCH [BRANCH]] - Uninstall a runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "RUNTIME must be specified", error);
      goto out;
    }

  runtime  = argv[1];
  if (argc >= 3)
    arch = argv[2];
  if (argc >= 4)
    branch = argv[3];

  /* TODO: look for apps, require --force */

  ref = g_build_filename ("runtime", runtime, arch, branch, NULL);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Nothing to uninstall");
      goto out;
    }

  if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
    goto out;

  if (branch)
    {
      gs_unref_object GFile *db;

      db = deploy_base;
      deploy_base = g_file_get_parent (deploy_base);

      if (!g_file_delete (deploy_base, cancellable, NULL))
        goto done;
    }

  if (arch)
    {
      gs_unref_object GFile *db;

      db = deploy_base;
      deploy_base = g_file_get_parent (deploy_base);

      if (!g_file_delete (deploy_base, cancellable, NULL))
        goto done;
    }

 done:
  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}

gboolean
xdg_app_builtin_uninstall_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  const char *app;
  const char *arch = NULL;
  const char *branch = NULL;
  gs_free char *ref = NULL;
  GError *temp_error = NULL;

  context = g_option_context_new ("APP [ARCH [BRANCH]] - Uninstall an application");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "APP must be specified", error);
      goto out;
    }

  app = argv[1];
  if (argc >= 3)
    arch = argv[2];
  if (argc >= 4)
    branch = argv[3];

  ref = g_build_filename ("app", app, arch, branch, NULL);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Nothing to uninstall");
      goto out;
    }

  if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
    goto out;

  if (branch)
    {
      gs_unref_object GFile *db;

      db = deploy_base;
      deploy_base = g_file_get_parent (deploy_base);

      if (!g_file_delete (deploy_base, cancellable, NULL))
        goto done;
    }

  if (arch)
    {
      gs_unref_object GFile *db;
      gs_unref_object GFileEnumerator *dir_enum;
      gs_unref_object GFileInfo *child_info;

      db = deploy_base;
      deploy_base = g_file_get_parent (deploy_base);

      dir_enum = g_file_enumerate_children (deploy_base, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, error);
      if (!dir_enum)
        goto out;

      while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
        {
          const char *arch;

          arch = g_file_info_get_name (child_info);
          if (strcmp (arch, "data") == 0)
            continue;

          goto done;
        }
      if (temp_error != NULL)
        goto out;

      if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
        goto out;
    }

 done:
  ret = TRUE;

 out:
  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  if (context)
    g_option_context_free (context);
  return ret;
}
