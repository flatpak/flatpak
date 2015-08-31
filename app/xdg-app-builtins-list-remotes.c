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

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"

static gboolean opt_show_urls;

static GOptionEntry options[] = {
  { "show-urls", 0, 0, G_OPTION_ARG_NONE, &opt_show_urls, "Show remote URLs in list", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_list_remotes (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  g_autoptr(XdgAppDir) dir = NULL;
  g_auto(GStrv) remotes = NULL;
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
          g_autofree char *remote_url = NULL;

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
