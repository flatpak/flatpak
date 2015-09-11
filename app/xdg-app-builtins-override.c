/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"
#include "xdg-app-run.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
xdg_app_builtin_override (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  gboolean ret = FALSE;
  const char *app;
  g_autoptr(XdgAppContext) arg_context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(XdgAppContext) overrides = NULL;

  context = g_option_context_new ("APP - Override settings for application");

  arg_context = xdg_app_context_new ();
  g_option_context_add_group (context, xdg_app_context_get_options (arg_context));

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      usage_error (context, "APP must be specified", error);
      return FALSE;
    }

  app = argv[1];

  if (!xdg_app_is_valid_name (app))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid application name", app);
      return FALSE;
    }

  metakey = xdg_app_load_override_keyfile (app, xdg_app_dir_is_user (dir), error);
  if (metakey == NULL)
    return FALSE;

  overrides = xdg_app_context_new ();
  if (!xdg_app_context_load_metadata (overrides, metakey, error))
    return FALSE;

  xdg_app_context_merge (overrides, arg_context);

  xdg_app_context_save_metadata (overrides, metakey);

  if (!xdg_app_save_override_keyfile (metakey, app, xdg_app_dir_is_user (dir), error))
    return FALSE;

  ret = TRUE;

 out:
  /*  if (context)
      g_option_context_free (context);*/
  return ret;
}
