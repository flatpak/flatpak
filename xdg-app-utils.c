#include "config.h"

#include "xdg-app-utils.h"

#include <glib.h>

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
