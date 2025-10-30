/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-instance.h"

#define FLATPAK_BUILTIN_KILL_N_RETRIES 5
#define FLATPAK_BUILTIN_KILL_RETRY_SLEEP_USEC (G_USEC_PER_SEC / 10)

static GOptionEntry options[] = {
  { NULL }
};

static gboolean
instance_equal (FlatpakInstance *a,
                FlatpakInstance *b)
{
  return g_strcmp0 (flatpak_instance_get_id (a),
                    flatpak_instance_get_id (b)) == 0;
}

static GPtrArray *
kill_instances (GPtrArray *kill_list)
{
  g_autoptr(GPtrArray) instances = flatpak_instance_get_all ();
  g_autoptr(GPtrArray) remaining =
    g_ptr_array_new_with_free_func (g_object_unref);

  for (size_t i = 0; i < kill_list->len; i++)
    {
      FlatpakInstance *to_kill = g_ptr_array_index (kill_list, i);
      pid_t pid;

      if (!g_ptr_array_find_with_equal_func (instances, to_kill,
                                             (GEqualFunc) instance_equal,
                                             NULL))
        {
          g_info ("Instance %s disappeared", flatpak_instance_get_id (to_kill));
          continue;
        }

      pid = flatpak_instance_get_child_pid (to_kill);
      if (pid != 0)
        {
          kill (pid, SIGKILL);
          g_info ("Instance %s killed", flatpak_instance_get_id (to_kill));
          continue;
        }

      g_ptr_array_add (remaining, g_object_ref (to_kill));
    }

  return g_steal_pointer (&remaining);
}

static gboolean
kill_id (const char  *id,
         GError     **error)
{
  g_autoptr(GPtrArray) instances = flatpak_instance_get_all ();
  g_autoptr(GPtrArray) kill_list =
    g_ptr_array_new_with_free_func (g_object_unref);

  for (size_t i = 0; i < instances->len; i++)
    {
      FlatpakInstance *instance = g_ptr_array_index (instances, i);

      if (g_strcmp0 (id, flatpak_instance_get_app (instance)) != 0 &&
          g_strcmp0 (id, flatpak_instance_get_id (instance)) != 0)
        continue;

      g_info ("Found instance %s to kill", flatpak_instance_get_id (instance));

      g_ptr_array_add (kill_list, g_object_ref (instance));
    }

  if (kill_list->len == 0)
    return flatpak_fail (error, _("%s is not running"), id);

  for (size_t i = 0; i < FLATPAK_BUILTIN_KILL_N_RETRIES && kill_list->len > 0; i++)
    {
      g_autoptr (GPtrArray) remaining = NULL;

      if (i > 0)
        g_usleep (FLATPAK_BUILTIN_KILL_RETRY_SLEEP_USEC);

      remaining = kill_instances (kill_list);
      g_clear_pointer (&kill_list, g_ptr_array_unref);
      kill_list = g_steal_pointer (&remaining);
    }

  return TRUE;
}

gboolean
flatpak_builtin_kill (int           argc,
                      char        **argv,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  const char *id;

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

  id = argv[1];

  return kill_id (id, error);
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

          const char *app_name = flatpak_instance_get_app (instance);
          if (app_name)
            flatpak_complete_word (completion, "%s ", app_name);

          flatpak_complete_word (completion, "%s ", flatpak_instance_get_id (instance));
        }
      break;

    default:
      break;
    }

  return TRUE;
}
