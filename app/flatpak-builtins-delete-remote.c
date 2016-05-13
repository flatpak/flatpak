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
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"

static gboolean opt_force;

static GOptionEntry delete_options[] = {
  { "force", 0, 0, G_OPTION_ARG_NONE, &opt_force, "Remove remote even if in use",  },
  { NULL }
};


gboolean
flatpak_builtin_delete_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  const char *remote_name;

  context = g_option_context_new ("NAME - Delete a remote repository");

  g_option_context_add_main_entries (context, delete_options, NULL);

  if (!flatpak_option_context_parse (context, NULL, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "NAME must be specified", error);

  remote_name = argv[1];

  if (!flatpak_dir_remove_remote (dir, opt_force, remote_name,
                                  cancellable, error))
    return FALSE;

  return TRUE;
}
