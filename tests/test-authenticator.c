/*
 * Copyright © 2019 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"
#include <locale.h>

#include "flatpak-auth-private.h"
#include "flatpak-dbus-generated.h"

static GMainLoop *main_loop;
static guint name_owner_id = 0;
FlatpakAuthenticator *authenticator;

static gboolean
handle_request_ref_tokens (FlatpakAuthenticator *authenticator,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_handle_token,
                           GVariant *arg_authenticator_option,
                           const gchar *const *arg_refs)
{
  g_autoptr(GError) error = NULL;
  GVariantBuilder results;
  GVariantBuilder tokens;
  g_autofree char *required_token = NULL;
  g_autofree char *required_token_file = NULL;
  g_autofree char *request_path = flatpak_auth_create_request_path (g_dbus_method_invocation_get_sender (invocation),
                                                                    arg_handle_token,
                                                                    NULL);
  g_autoptr(AutoFlatpakAuthenticatorRequest) request = NULL;

  if (request_path == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid token");
      return TRUE;
    }

  request = flatpak_authenticator_request_skeleton_new ();

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         g_dbus_method_invocation_get_connection (invocation),
                                         request_path,
                                         &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  flatpak_authenticator_complete_request_ref_tokens (authenticator, invocation, request_path);

  g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));

  required_token_file = g_build_filename (g_get_user_runtime_dir (), "required-token", NULL);
  if (!g_file_get_contents (required_token_file, &required_token, NULL, NULL))
      required_token = g_strdup ("default-token");

  g_variant_builder_init (&tokens, G_VARIANT_TYPE ("a{sas}"));
  g_variant_builder_add (&tokens, "{s^as}", required_token, arg_refs);
  g_variant_builder_add (&results, "{sv}", "tokens", g_variant_builder_end (&tokens));

  flatpak_auth_request_emit_response (request,
                                      g_dbus_method_invocation_get_sender (invocation),
                                      FLATPAK_AUTH_RESPONSE_OK,
                                      g_variant_builder_end (&results));

  return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  g_debug ("Bus acquired, creating skeleton");

  g_dbus_connection_set_exit_on_close (connection, FALSE);

  authenticator = flatpak_authenticator_skeleton_new ();
  flatpak_authenticator_set_version (authenticator, 0);

  g_signal_connect (authenticator, "handle-request-ref-tokens", G_CALLBACK (handle_request_ref_tokens), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (authenticator),
                                         connection,
                                         FLATPAK_AUTHENTICATOR_OBJECT_PATH,
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("Name acquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("Name lost");
}


static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
main (int    argc,
      char **argv)
{
  gboolean replace;
  gboolean opt_verbose;
  GOptionContext *context;
  GDBusConnection *session_bus;
  GBusNameOwnerFlags flags;
  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose,  "Enable debug output.", NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  replace = FALSE;
  opt_verbose = FALSE;

  g_option_context_set_summary (context, "Flatpak portal");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      g_option_context_free (context);
      return 1;
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_debug ("Started test-authenticator");

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("Can't find bus: %s\n", error->message);
      return 1;
    }

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  "org.flatpak.Authenticator.test",
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  return 0;
}
