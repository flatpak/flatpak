#include "config.h"

#include "xdg-app-utils.h"
#include "xdg-app-dir.h"

#include <glib.h>
#include "libgsystem.h"

#include <stdlib.h>
#include <sys/utsname.h>

const char *
xdg_app_get_arch (void)
{
  static struct utsname buf;
  static char *arch = NULL;

  if (arch == NULL)
    {
      if (uname (&buf))
        arch = "unknown";
      else
        arch = buf.machine;
    }

  return arch;
}

char *
xdg_app_build_untyped_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename (runtime, arch, branch, NULL);
}

char *
xdg_app_build_runtime_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename ("runtime", runtime, arch, branch, NULL);
}

char *
xdg_app_build_app_ref (const char *app,
                       const char *branch,
                       const char *arch)
{
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename ("app", app, arch, branch, NULL);
}

GFile *
xdg_app_find_deploy_dir_for_ref (const char *ref,
                                 GCancellable *cancellable,
                                 GError **error)
{
  gs_unref_object XdgAppDir *user_dir = NULL;
  gs_unref_object XdgAppDir *system_dir = NULL;
  GFile *deploy = NULL;

  user_dir = xdg_app_dir_get_user ();
  system_dir = xdg_app_dir_get_system ();

  deploy = xdg_app_dir_get_if_deployed (user_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    deploy = xdg_app_dir_get_if_deployed (system_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s not installed", ref);
      return NULL;
    }

  return deploy;

}
