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
#include <polkit/polkit.h>

#include "xdg-app-dbus.h"
#include "xdg-app-dir.h"
#include "lib/xdg-app-error.h"

static PolkitAuthority *authority = NULL;

#ifndef glib_autoptr_cleanup_PolkitAuthorizationResult
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PolkitAuthorizationResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PolkitDetails, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PolkitSubject, g_object_unref)
#endif

static gboolean
handle_deploy (XdgAppSystemHelper    *object,
               GDBusMethodInvocation *invocation,
               const gchar           *arg_repo_path,
               guint32                arg_flags,
               const gchar           *arg_ref,
               const gchar           *arg_origin,
               const gchar *const    *arg_subpaths)
{
  g_autoptr(XdgAppDir) system = xdg_app_dir_get_system ();
  g_autoptr(GFile) path = g_file_new_for_path (arg_repo_path);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(PolkitSubject) subject = NULL;
  g_autoptr(PolkitDetails) details = NULL;
  gboolean is_update;
  g_autoptr(GMainContext) main_context = NULL;

  if ((arg_flags & ~XDG_APP_HELPER_DEPLOY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~XDG_APP_HELPER_DEPLOY_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_query_exists (path, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Path does not exist");
      return TRUE;
    }

  is_update = (arg_flags & XDG_APP_HELPER_DEPLOY_FLAGS_UPDATE) != 0;

  deploy_dir = xdg_app_dir_get_if_deployed (system, arg_ref,
                                            NULL, NULL);

  if (deploy_dir)
    {
      g_autofree char *real_origin = NULL;
      if (!is_update)
        {
          /* Can't install already installed app */
          g_dbus_method_invocation_return_error (invocation, XDG_APP_ERROR, XDG_APP_ERROR_ALREADY_INSTALLED,
                                                 "%s is already installed", arg_ref);
          return TRUE;
        }

      real_origin = xdg_app_dir_get_origin (system, arg_ref, NULL, NULL);
      if (g_strcmp0 (real_origin, arg_origin) != 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Wrong origin %s for update", arg_origin);
          return TRUE;
        }
    }
  else if (!deploy_dir && is_update)
    {
      /* Can't update not installed app */
      g_dbus_method_invocation_return_error (invocation, XDG_APP_ERROR, XDG_APP_ERROR_ALREADY_INSTALLED,
                                             "%s is not installed", arg_ref);
      return TRUE;
    }

  if (!xdg_app_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  /* Work around ostree-pull spinning the default main context for the sync calls */
  main_context = g_main_context_new ();
  g_main_context_push_thread_default (main_context);

  if (!xdg_app_dir_pull_untrusted_local (system, arg_repo_path,
                                         arg_origin,
                                         arg_ref,
                                         (char **) arg_subpaths,
                                         NULL,
                                         NULL, &error))
    {
      g_main_context_pop_thread_default (main_context);
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Error pulling from repo: %s", error->message);
      return TRUE;
    }

  g_main_context_pop_thread_default (main_context);

  if (is_update)
    {
      /* TODO: This doesn't support a custom subpath */
      if (!xdg_app_dir_deploy_update (system, arg_ref,
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
                                       (char **) arg_subpaths,
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


static gboolean
xdg_app_authorize_method_handler (GDBusInterfaceSkeleton *interface,
                                  GDBusMethodInvocation  *invocation,
                                  gpointer                user_data)
{
  const gchar *method_name;
  GVariant *parameters;
  gboolean authorized;
  const gchar *sender;
  const gchar *action;

  g_autoptr(PolkitSubject) subject = NULL;
  g_autoptr(PolkitDetails) details = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(PolkitAuthorizationResult) result = NULL;

  authorized = FALSE;

  method_name = g_dbus_method_invocation_get_method_name (invocation);
  parameters = g_dbus_method_invocation_get_parameters (invocation);

  sender = g_dbus_method_invocation_get_sender (invocation);
  subject = polkit_system_bus_name_new (sender);

  if (g_strcmp0 (method_name, "Deploy") == 0)
    {
      const char *ref, *origin;
      guint32 flags;
      gboolean is_update;
      gboolean is_app;

      g_variant_get_child (parameters, 1, "u", &flags);
      g_variant_get_child (parameters, 2, "&s", &ref);
      g_variant_get_child (parameters, 3, "&s", &origin);

      is_update = (flags & XDG_APP_HELPER_DEPLOY_FLAGS_UPDATE) != 0;
      is_app = g_str_has_prefix (ref, "app/");

      if (is_update)
        {
          if (is_app)
            action = "org.freedesktop.XdgApp.app-update";
          else
            action = "org.freedesktop.XdgApp.runtime-update";
        }
      else
        {
          if (is_app)
            action = "org.freedesktop.XdgApp.app-install";
          else
            action = "org.freedesktop.XdgApp.runtime-install";
        }

      details = polkit_details_new ();
      polkit_details_insert (details, "origin", origin);
      polkit_details_insert (details, "ref", ref);

      result = polkit_authority_check_authorization_sync (authority, subject,
                                                          action, details,
                                                          POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                          NULL, &error);
      if (result == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }

  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_PERMISSION_DENIED,
                                             "Operation not permitted");
    }

  return authorized;
}


static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  XdgAppSystemHelper *helper;
  GError *error = NULL;

  helper = xdg_app_system_helper_skeleton_new ();

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (helper),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  g_signal_connect (helper, "handle-deploy", G_CALLBACK (handle_deploy), NULL);

  g_signal_connect (helper, "g-authorize-method",
                    G_CALLBACK (xdg_app_authorize_method_handler),
                    NULL);

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

  g_autoptr(GError) error = NULL;

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  authority = polkit_authority_get_sync (NULL, &error);
  if (authority == NULL)
    {
      g_printerr ("Can't get polkit authority: %s\n", error->message);
      return 1;
    }

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

  return 0;
}
