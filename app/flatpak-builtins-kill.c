/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-instance.h"

static GOptionEntry options[] = {
  { NULL }
};

static gboolean
kill_instance (const char *id,
               GError    **error)
{
  g_autoptr(GPtrArray) instances = NULL;
  int j;
  int killed = 0;

  instances = flatpak_instance_get_all ();

  for (j = 0; j < instances->len; j++)
    {
      FlatpakInstance *instance = (FlatpakInstance *) g_ptr_array_index (instances, j);
      if (g_strcmp0 (id, flatpak_instance_get_app (instance)) == 0 ||
          strcmp (id, flatpak_instance_get_id (instance)) == 0)
        {
          pid_t pid = flatpak_instance_get_child_pid (instance);
          kill (pid, SIGKILL);
          killed++;
        }
    }

  g_debug ("Killed %d instances", killed);

  if (killed == 0)
    return flatpak_fail (error, _("%s is not running"), id);

  return TRUE;
}

gboolean
flatpak_builtin_kill (int           argc,
                      char        **argv,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  const char *instance;

  context = g_option_context_new (_("INSTANCE - Stop a running application"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc > 2)
    {
      usage_error (context, _("Extra arguments given"), error);
      return FALSE;
    }

  if (argc < 2)
    {
      usage_error (context, _("Must specify the app to kill"), error);
      return FALSE;
    }

  instance = argv[1];

  return kill_instance (instance, error);
}

gboolean
flatpak_complete_kill (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      instances = flatpak_instance_get_all ();
      for (i = 0; i < instances->len; i++)
        {
          FlatpakInstance *instance = (FlatpakInstance *) g_ptr_array_index (instances, i);
          flatpak_complete_word (completion, "%s ", flatpak_instance_get_app (instance));
          flatpak_complete_word (completion, "%s ", flatpak_instance_get_id (instance));
        }
      break;

    default:
      break;
    }

  return TRUE;
}
