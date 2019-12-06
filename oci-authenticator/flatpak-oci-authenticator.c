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

#include <json-glib/json-glib.h>
#include "flatpak-oci-registry-private.h"
#include "flatpak-utils-http-private.h"
#include "flatpak-auth-private.h"
#include "flatpak-dbus-generated.h"

FlatpakAuthenticator *authenticator;
static GMainLoop *main_loop = NULL;
static guint name_owner_id = 0;
static gboolean no_idle_exit = FALSE;
static SoupSession *http_session = NULL;

#define IDLE_TIMEOUT_SECS 10 * 60

static void
skeleton_died_cb (gpointer data)
{
  g_debug ("skeleton finalized, exiting");
  g_main_loop_quit (main_loop);
}

static gboolean
unref_skeleton_in_timeout_cb (gpointer user_data)
{
  static gboolean unreffed = FALSE;

  g_debug ("unreffing authenticator main ref");
  if (!unreffed)
    {
      g_object_unref (authenticator);
      unreffed = TRUE;
    }

  return G_SOURCE_REMOVE;
}

static void
unref_skeleton_in_timeout (void)
{
  if (name_owner_id)
    g_bus_unown_name (name_owner_id);
  name_owner_id = 0;

  /* After we've lost the name or idled we drop the main ref on the authenticator
     so that we'll exit when it drops to zero. However, if there are
     outstanding calls these will keep the refcount up during the
     execution of them. We do the unref on a timeout to make sure
     we're completely draining the queue of (stale) requests. */
  g_timeout_add (500, unref_skeleton_in_timeout_cb, NULL);
}

static gboolean
idle_timeout_cb (gpointer user_data)
{
  if (name_owner_id)
    {
      g_debug ("Idle - unowning name");
      unref_skeleton_in_timeout ();
    }
  return G_SOURCE_REMOVE;
}

static void
schedule_idle_callback (void)
{
  static guint idle_timeout_id = 0;

  if (!no_idle_exit)
    {
      if (idle_timeout_id != 0)
        g_source_remove (idle_timeout_id);

      idle_timeout_id = g_timeout_add_seconds (IDLE_TIMEOUT_SECS, idle_timeout_cb, NULL);
    }
}

static gboolean
handle_request_ref_tokens_close (FlatpakAuthenticatorRequest *object,
                                 GDBusMethodInvocation *invocation,
                                 gpointer           user_data)
{
  g_debug ("handlling Request.Close");

  return TRUE;
}

static char *
get_token_for_ref (FlatpakOciRegistry *registry,
                   GVariant *ref_data,
                   const char *basic_auth,
                   GError **error)
{
  g_autofree char *oci_digest = NULL;
  const char *ref, *commit, *oci_repository;
  g_autoptr(GVariant) data = NULL;
  gint32 token_type;

  g_variant_get (ref_data, "(&s&si@a{sv})", &ref, &commit, &token_type, &data);

  if (!g_variant_lookup (data, "summary.xa.oci-repository", "&s", &oci_repository))
    {
      flatpak_fail (error, "Not a oci remote, missing summary.xa.oci-repository");
      return NULL;
    }

  oci_digest = g_strconcat ("sha256:", commit, NULL);

  return flatpak_oci_registry_get_token (registry, oci_repository, oci_digest, basic_auth, NULL, error);
}

/* Note: This runs on a thread, so we can just block */
static gboolean
handle_request_ref_tokens (FlatpakAuthenticator *authenticator,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_handle_token,
                           GVariant *arg_authenticator_options,
                           const gchar *arg_remote,
                           const gchar *arg_remote_uri,
                           GVariant *arg_refs,
                           GVariant *arg_extra_data,
                           const gchar *arg_parent_window)
{
  g_autofree char *request_path = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(AutoFlatpakAuthenticatorRequest) request = NULL;
  const char *auth = NULL;
  const char *oci_registry_uri = NULL;
  gsize n_refs, i;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  GVariantBuilder tokens;
  GVariantBuilder results;
  g_autofree char *sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  g_debug ("handling Authenticator.RequestRefTokens");

  if (!g_variant_lookup (arg_authenticator_options, "auth", "&s", &auth))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No auth configured");
      return TRUE;
    }

  if (!g_variant_lookup (arg_extra_data, "xa.oci-registry-uri", "&s", &oci_registry_uri))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Not a OCI remote");
      return TRUE;
    }

  request_path = flatpak_auth_create_request_path (g_dbus_method_invocation_get_sender (invocation),
                                                   arg_handle_token, NULL);
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

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_request_ref_tokens_close), NULL);

  flatpak_authenticator_complete_request_ref_tokens (authenticator, invocation, request_path);

  registry = flatpak_oci_registry_new (oci_registry_uri, FALSE, -1, NULL, &error);
  if (registry == NULL)
    {
      g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&results, "{sv}", "error-message", g_variant_new_string (error->message));
      flatpak_auth_request_emit_response (request, sender,
                                          FLATPAK_AUTH_RESPONSE_ERROR,
                                          g_variant_builder_end (&results));
      return TRUE;
    }

  g_variant_builder_init (&tokens, G_VARIANT_TYPE ("a{sas}"));

  n_refs = g_variant_n_children (arg_refs);
  for (i = 0; i < n_refs; i++)
    {
      g_autoptr(GVariant) ref_data = g_variant_get_child_value (arg_refs, i);
      char *for_refs_strv[2] = { NULL, NULL};
      g_autofree char *token = NULL;

      token = get_token_for_ref (registry, ref_data, auth, &error);
      if (token == NULL)
        {
          g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
          g_variant_builder_add (&results, "{sv}", "error-message", g_variant_new_string (error->message));
          flatpak_auth_request_emit_response (request, sender,
                                              FLATPAK_AUTH_RESPONSE_ERROR,
                                              g_variant_builder_end (&results));
          return TRUE;
        }

      g_variant_get_child (ref_data, 0, "&s", &for_refs_strv[0]);
      g_variant_builder_add (&tokens, "{s^as}", token, for_refs_strv);
    }

  g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&results, "{sv}", "tokens", g_variant_builder_end (&tokens));

  g_debug ("emiting OK response");
  flatpak_auth_request_emit_response (request, sender,
                                      FLATPAK_AUTH_RESPONSE_OK,
                                      g_variant_builder_end (&results));

  return TRUE;
}

static gboolean
flatpak_authorize_method_handler (GDBusInterfaceSkeleton *interface,
                                  GDBusMethodInvocation  *invocation,
                                  gpointer                user_data)

{
  /* Ensure we don't idle exit */
  schedule_idle_callback ();

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

  g_object_set_data_full (G_OBJECT (authenticator), "track-alive", GINT_TO_POINTER (42), skeleton_died_cb);

  g_signal_connect (authenticator, "handle-request-ref-tokens", G_CALLBACK (handle_request_ref_tokens), NULL);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (authenticator),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  /* This is only used for idle tracking atm */
  g_signal_connect (authenticator, "g-authorize-method",
                    G_CALLBACK (flatpak_authorize_method_handler),
                    NULL);

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
    { "no-idle-exit", 0, 0, G_OPTION_ARG_NONE, &no_idle_exit,  "Don't exit when idle.", NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  replace = FALSE;
  opt_verbose = FALSE;

  g_option_context_set_summary (context, "Flatpak authenticator");
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

  g_debug ("Started flatpak-authenticator");

  http_session = flatpak_create_soup_session (PACKAGE_STRING);

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
                                  "org.flatpak.Authenticator.Oci",
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  return 0;
}
