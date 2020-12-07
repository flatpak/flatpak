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
#include "flatpak-utils-base-private.h"
#include "flatpak-dbus-generated.h"

static GMainLoop *main_loop;
static guint name_owner_id = 0;
FlatpakAuthenticator *global_authenticator;

typedef struct {
  FlatpakAuthenticatorRequest *request;
  GSocketService *server;
  char **arg_refs;
} TokenRequestData;

static void
token_request_data_free (TokenRequestData *data)
{
  g_clear_object (&data->request);
  g_socket_service_stop  (data->server);
  g_clear_object (&data->server);
  g_strfreev (data->arg_refs);
  g_free (data);
}

static TokenRequestData *
token_request_data_new (FlatpakAuthenticatorRequest *request,
                        GSocketService *server,
                        const gchar *const *arg_refs)
{
  TokenRequestData *data = g_new0 (TokenRequestData, 1);
  data->request = g_object_ref (request);
  data->server = g_object_ref (server);
  data->arg_refs = g_strdupv ((char **)arg_refs);
  return data;
}

static char *
get_required_token (void)
{
  g_autofree char *required_token_file = NULL;
  g_autofree char *required_token = NULL;

  required_token_file = g_build_filename (g_get_user_runtime_dir (), "required-token", NULL);
  if (!g_file_get_contents (required_token_file, &required_token, NULL, NULL))
    required_token = g_strdup ("default-token");
  return g_steal_pointer (&required_token);
}

static void
write_request (char *str)
{
  g_autofree char *request_file = NULL;

  request_file = g_build_filename (g_get_user_runtime_dir (), "request", NULL);
  g_file_set_contents (request_file, str, -1, NULL);
  g_free (str);
}

static gboolean
requires_webflow (void)
{
  g_autofree char *require_webflow_file = g_build_filename (g_get_user_runtime_dir (), "require-webflow", NULL);

  return g_file_test (require_webflow_file, G_FILE_TEST_EXISTS);
}

static gboolean
request_webflow (void)
{
  g_autofree char *request_webflow_file = g_build_filename (g_get_user_runtime_dir (), "request-webflow", NULL);

  return g_file_test (request_webflow_file, G_FILE_TEST_EXISTS);
}

static void
finish_request_ref_tokens (TokenRequestData *data)
{
  g_autofree char *required_token = NULL;
  GVariantBuilder tokens;
  GVariantBuilder results;

  g_assert_true (data->request != NULL);

  required_token = get_required_token ();

  g_variant_builder_init (&tokens, G_VARIANT_TYPE ("a{sas}"));
  g_variant_builder_add (&tokens, "{s^as}", required_token, data->arg_refs);

  g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&results, "{sv}", "tokens", g_variant_builder_end (&tokens));

  g_debug ("emiting response");
  flatpak_authenticator_request_emit_response (data->request,
                                               FLATPAK_AUTH_RESPONSE_OK,
                                               g_variant_builder_end (&results));
}

static gboolean
http_incoming (GSocketService    *service,
               GSocketConnection *connection,
               GObject           *source_object,
               gpointer           user_data)
{
  TokenRequestData *data = user_data;
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));

  g_assert_true (data->request != NULL);

  /* For the test, just assume any connection is a valid use of the web flow */
  g_debug ("handling incomming http request");

  g_debug ("emiting webflow done");
  flatpak_authenticator_request_emit_webflow_done (data->request, options);

  finish_request_ref_tokens (data);

  token_request_data_free (data);

  return TRUE;
}

static gboolean
handle_request_close (FlatpakAuthenticatorRequest *object,
                      GDBusMethodInvocation *invocation,
                      gpointer           user_data)
{
  TokenRequestData *data = user_data;

  g_debug ("handle_request_close");

  flatpak_authenticator_request_complete_close (object, invocation);

  if (requires_webflow ())
    {
      GVariantBuilder results;

      g_debug ("Webflow was cancelled by client");

      g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
      flatpak_authenticator_request_emit_response (data->request,
                                                   FLATPAK_AUTH_RESPONSE_CANCELLED,
                                                   g_variant_builder_end (&results));
    }
  else
    {
      g_debug ("Ignored webflow cancel by client");
      finish_request_ref_tokens (data); /* Silently succeed anyway */
    }

  token_request_data_free (data);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_request_ref_tokens (FlatpakAuthenticator *authenticator,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_handle_token,
                           GVariant *arg_authenticator_option,
                           const gchar *arg_remote,
                           const gchar *arg_remote_uri,
                           GVariant *arg_refs,
                           GVariant *arg_options,
                           const gchar *arg_parent_window)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSocketService) server = NULL;
  g_autoptr(AutoFlatpakAuthenticatorRequest) request = NULL;
  g_autofree char *uri = NULL;
  g_autofree char *request_path = NULL;
  guint16 port;
  g_autoptr(GPtrArray) refs = NULL;
  gsize n_refs, i;
  g_autofree char *options_s = NULL;

  g_debug ("handling RequestRefTokens");

  options_s = g_variant_print (arg_options, FALSE);
  write_request (g_strdup_printf ("remote: %s\n"
                                  "uri: %s\n"
                                  "options: %s",
                                  arg_remote,
                                  arg_remote_uri,
                                  options_s));

  request_path = flatpak_auth_create_request_path (g_dbus_method_invocation_get_sender (invocation),
                                                   arg_handle_token, NULL);
  if (request_path == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid token");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request = flatpak_authenticator_request_skeleton_new ();
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         g_dbus_method_invocation_get_connection (invocation),
                                         request_path,
                                         &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  server = g_socket_service_new ();
  port = g_socket_listener_add_any_inet_port (G_SOCKET_LISTENER (server), NULL, &error);
  if (port == 0)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  refs = g_ptr_array_new_with_free_func (g_free);
  n_refs = g_variant_n_children (arg_refs);
  for (i = 0; i < n_refs; i++)
    {
      const char *ref, *commit;
      gint32 token_type;
      g_autoptr(GVariant) data = NULL;

      g_variant_get_child (arg_refs, i, "(&s&si@a{sv})", &ref, &commit, &token_type, &data);

      g_ptr_array_add (refs, g_strdup (ref));
    }
  g_ptr_array_add (refs, NULL);

  TokenRequestData *data;
  data = token_request_data_new (request, server, (const char *const*)refs->pdata);

  g_signal_connect (server, "incoming", (GCallback)http_incoming, data);
  g_signal_connect (request, "handle-close", G_CALLBACK (handle_request_close), data);

  flatpak_authenticator_complete_request_ref_tokens (authenticator, invocation, request_path);

  if (request_webflow ())
    {
      g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));
      uri = g_strdup_printf ("http://localhost:%d", (int)port);
      g_debug ("Requesting webflow %s", uri);
      flatpak_authenticator_request_emit_webflow (request, uri, options);
    }
  else
    {
      finish_request_ref_tokens (data);
      token_request_data_free (data);
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  g_debug ("Bus acquired, creating skeleton");

  g_dbus_connection_set_exit_on_close (connection, FALSE);

  global_authenticator = flatpak_authenticator_skeleton_new ();
  flatpak_authenticator_set_version (global_authenticator, 0);

  g_signal_connect (global_authenticator, "handle-request-ref-tokens", G_CALLBACK (handle_request_ref_tokens), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (global_authenticator),
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
