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
#include <gio/gio.h>

#include "xdg-app-dbus.h"
#include "xdg-app-dir.h"

static GDBusNodeInfo *introspection_data = NULL;

static gboolean
handle_deploy (XdgAppSystemHelper *object,
               GDBusMethodInvocation *invocation,
               const gchar *arg_repo_path,
               guint32 arg_flags,
               const gchar *arg_ref,
               const gchar *arg_origin,
               const gchar *const *arg_subpaths)
{
  g_autoptr(XdgAppDir) system = xdg_app_dir_get_system ();
  g_autoptr(GFile) path = g_file_new_for_path (arg_repo_path);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  gboolean is_update;

  is_update = (arg_flags & XDG_APP_HELPER_DEPLOY_FLAGS_UPDATE) != 0;

  if ((arg_flags & ~XDG_APP_HELPER_DEPLOY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~XDG_APP_HELPER_DEPLOY_FLAGS_ALL));
    }

  if (!g_file_query_exists (path, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Path does not exist");
      return TRUE;
    }

  deploy_dir = xdg_app_dir_get_if_deployed (system, arg_ref,
                                            NULL, NULL);

  if (deploy_dir)
    {
      g_autofree char *real_origin = NULL;
      if (!is_update)
        {
          /* Can't install already installed app */
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s is already installed", arg_ref);
          return TRUE;
        }

      real_origin = xdg_app_dir_get_origin (system, arg_ref, NULL, NULL);
      if (real_origin == NULL || strcmp (real_origin, arg_origin) != 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Wrong origin %s for update", arg_origin);
          return TRUE;
        }
    }
  else if (!deploy_dir && is_update)
    {
      /* Can't update not installed app */
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "%s is not installed", arg_ref);
      return TRUE;
    }

  if (!xdg_app_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  if (!xdg_app_dir_pull_untrusted_local (system, arg_repo_path,
                                         arg_origin,
                                         arg_ref,
                                         (char **)arg_subpaths,
                                         NULL,
                                         NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Error pulling from repo: %s", error->message);
      return TRUE;
    }

  if (is_update)
    {
      /* TODO: This doesn't support a custom subpath */
      if (!xdg_app_dir_deploy_update (system, arg_ref, arg_origin,
                                       NULL,
                                      NULL, &error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Error deploying: %s", error->message);
          return TRUE;
        }
    }
  else
    {
      if (!xdg_app_dir_deploy_install (system, arg_ref, arg_origin,
                                       (char **)arg_subpaths,
                                       NULL, &error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Error deploying: %s", error->message);
          return TRUE;
        }
    }

  xdg_app_system_helper_complete_deploy (object, invocation);

  return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  XdgAppSystemHelper *helper;
  GError *error = NULL;

  helper = xdg_app_system_helper_skeleton_new ();

  g_signal_connect (helper, "handle-deploy", G_CALLBACK (handle_deploy), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         "/org/freedesktop/XdgApp/SystemHelper",
                                         &error))
    {
      g_warning ("error: %s\n", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  exit (1);
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;
  GMainLoop *loop;
  GBytes *introspection_bytes;

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  introspection_bytes = g_resources_lookup_data ("/org/freedesktop/XdgApp/org.freedesktop.XdgApp.xml", 0, NULL);
  g_assert (introspection_bytes != NULL);

  introspection_data = g_dbus_node_info_new_for_xml (g_bytes_get_data (introspection_bytes, NULL), NULL);

  owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                             "org.freedesktop.XdgApp.SystemHelper",
                             G_BUS_NAME_OWNER_FLAGS_NONE,
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  g_dbus_node_info_unref (introspection_data);

  return 0;
}
