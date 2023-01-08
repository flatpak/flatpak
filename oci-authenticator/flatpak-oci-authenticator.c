/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include <glib/gi18n-lib.h>
#include <json-glib/json-glib.h>
#include "flatpak-oci-registry-private.h"
#include "flatpak-utils-base-private.h"
#include "flatpak-utils-http-private.h"
#include "flatpak-auth-private.h"
#include "flatpak-dbus-generated.h"

FlatpakAuthenticator *authenticator;
static GMainLoop *main_loop = NULL;
static guint name_owner_id = 0;
static gboolean no_idle_exit = FALSE;
static FlatpakHttpSession *http_session = NULL;

#define IDLE_TIMEOUT_SECS 10 * 60

static void
skeleton_died_cb (gpointer data)
{
  g_info ("skeleton finalized, exiting");
  g_main_loop_quit (main_loop);
}

static gboolean
unref_skeleton_in_timeout_cb (gpointer user_data)
{
  static gboolean unreffed = FALSE;

  g_info ("unreffing authenticator main ref");
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
      g_info ("Idle - unowning name");
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

typedef struct {
  gboolean done;
  char *user;
  char *password;
  GCond cond;
  GMutex mutex;
} BasicAuthData;


G_LOCK_DEFINE (active_auth);
static GHashTable *active_auth;

static void
cancel_basic_auth (BasicAuthData *auth)
{
  g_mutex_lock (&auth->mutex);
  if (!auth->done)
    {
      auth->done = TRUE;
      g_cond_signal (&auth->cond);
    }
  g_mutex_unlock (&auth->mutex);
}

static gboolean
handle_request_ref_tokens_close (FlatpakAuthenticatorRequest *object,
                                 GDBusMethodInvocation *invocation,
                                 gpointer user_data)
{
  BasicAuthData *auth = user_data;

  g_info ("handlling Request.Close");

  flatpak_authenticator_request_complete_close (object, invocation);

  cancel_basic_auth (auth);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
add_auth_for_peer (const char *sender,
                   BasicAuthData *auth)
{
  GList *list;

  G_LOCK (active_auth);
  if (active_auth == NULL)
    active_auth = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  list = g_hash_table_lookup (active_auth, sender);
  list = g_list_prepend (list, auth);
  g_hash_table_insert (active_auth, g_strdup (sender), list);

  G_UNLOCK (active_auth);
}

static void
remove_auth_for_peer (const char *sender,
                      BasicAuthData *auth)
{
  GList *list;

  G_LOCK (active_auth);
  list = g_hash_table_lookup (active_auth, sender);
  list = g_list_remove (list, auth);
  g_hash_table_insert (active_auth, g_strdup (sender), list);

  G_UNLOCK (active_auth);
}

static gpointer
peer_died (const char *name)
{
  G_LOCK (active_auth);
  if (active_auth)
    {
      GList *active = g_hash_table_lookup (active_auth, name);
      if (active)
        {
          for (GList *l = active; l != NULL; l = l->next)
            {
              g_info ("Cancelling auth operation for dying peer %s", name);
              cancel_basic_auth (l->data);
            }
          g_list_free (active);
          g_hash_table_remove (active_auth, name);
        }
    }
  G_UNLOCK (active_auth);
  return NULL;
}

static gboolean
handle_request_ref_tokens_basic_auth_reply (FlatpakAuthenticatorRequest *object,
                                            GDBusMethodInvocation *invocation,
                                            const gchar *arg_user,
                                            const gchar *arg_password,
                                            GVariant *options,
                                            gpointer user_data)
{
  BasicAuthData *auth = user_data;

  g_info ("handlling Request.BasicAuthReply %s %s", arg_user, arg_password);

  flatpak_authenticator_request_complete_basic_auth_reply (object, invocation);

  g_mutex_lock (&auth->mutex);
  if (!auth->done)
    {
      auth->done = TRUE;
      auth->user = g_strdup (arg_user);
      auth->password = g_strdup (arg_password);
      g_cond_signal (&auth->cond);
    }
  g_mutex_unlock (&auth->mutex);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static char *
run_basic_auth (FlatpakAuthenticatorRequest *request,
                const char *sender,
                const char *realm,
                const char *previous_error)
{
  BasicAuthData auth = { FALSE };
  int id1, id2;
  g_autofree char *combined = NULL;
  g_autoptr(GVariant) options = NULL;
  GVariantBuilder options_builder;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE ("a{sv}"));

  if (previous_error)
    g_variant_builder_add (&options_builder, "{sv}", "previous-error", g_variant_new_string (previous_error));

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  g_cond_init (&auth.cond);
  g_mutex_init (&auth.mutex);

  g_mutex_lock (&auth.mutex);

  add_auth_for_peer (sender, &auth);

  id1 = g_signal_connect (request, "handle-close", G_CALLBACK (handle_request_ref_tokens_close), &auth);
  id2 = g_signal_connect (request, "handle-basic-auth-reply", G_CALLBACK (handle_request_ref_tokens_basic_auth_reply), &auth);

  flatpak_authenticator_request_emit_basic_auth (request, realm, options);

  while (!auth.done)
    g_cond_wait (&auth.cond, &auth.mutex);

  g_signal_handler_disconnect (request, id1);
  g_signal_handler_disconnect (request, id2);

  remove_auth_for_peer (sender, &auth);

  g_mutex_unlock (&auth.mutex);

  g_cond_clear (&auth.cond);
  g_mutex_clear (&auth.mutex);

  if (auth.user == NULL)
    return NULL;

  combined = g_strdup_printf ("%s:%s", auth.user, auth.password);
  g_free (auth.user);
  g_free (auth.password);

  return g_base64_encode ((guchar *)combined, strlen (combined));
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
      flatpak_fail (error, _("Not a oci remote, missing summary.xa.oci-repository"));
      return NULL;
    }

  oci_digest = g_strconcat ("sha256:", commit, NULL);

  return flatpak_oci_registry_get_token (registry, oci_repository, oci_digest, basic_auth, NULL, error);
}

static gboolean
cancel_request (FlatpakAuthenticatorRequest *request,
                const char *sender)
{
  GVariantBuilder results;

  g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
  flatpak_authenticator_request_emit_response (request,
                                               FLATPAK_AUTH_RESPONSE_CANCELLED,
                                               g_variant_builder_end (&results));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
error_request_raw (FlatpakAuthenticatorRequest *request,
                   const char *sender,
                   gint32 error_code,
                   const char *error_message)
{
  GVariantBuilder results;

  g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&results, "{sv}", "error-message", g_variant_new_string (error_message));
  g_variant_builder_add (&results, "{sv}", "error-code", g_variant_new_int32 (error_code));
  flatpak_authenticator_request_emit_response (request,
                                               FLATPAK_AUTH_RESPONSE_ERROR,
                                               g_variant_builder_end (&results));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
error_request (FlatpakAuthenticatorRequest *request,
               const char *sender,
               GError *error)
{
  int error_code = -1;

  if (error->domain == FLATPAK_ERROR)
    error_code = error->code;

  return error_request_raw (request, sender, error_code, error->message);
}

static char *
canonicalize_registry_uri (const char *oci_registry_uri)
{
  const char *slash;
  /* Skip http: part */
  while (*oci_registry_uri != 0 &&
         *oci_registry_uri != ':')
    oci_registry_uri++;

  if (*oci_registry_uri != 0)
    oci_registry_uri++;

  /* Skip slashes */
  while (*oci_registry_uri != 0 &&
         *oci_registry_uri == '/')
    oci_registry_uri++;

  slash = strchr (oci_registry_uri, '/');
  if (slash)
    return g_strndup (oci_registry_uri, slash - oci_registry_uri);
  else
    return g_strdup (oci_registry_uri);
}

static char *
lookup_auth_from_config_path (const char *oci_registry_uri,
                              const char *path)
{
  g_autofree char *data = NULL;
  g_autoptr(JsonNode) json = NULL;
  JsonObject *auths = NULL, *registry_auth = NULL;

  if (!g_file_get_contents (path, &data, NULL, NULL))
    return NULL;

  json = json_from_string (data, NULL);
  if (json == NULL)
    return NULL;
  if (json_object_has_member (json_node_get_object (json), "auths"))
    auths = json_object_get_object_member (json_node_get_object (json), "auths");
  if (auths)
    {
      if (json_object_has_member (auths, oci_registry_uri))
        registry_auth = json_object_get_object_member (auths, oci_registry_uri);
      if (registry_auth == NULL)
        {
          g_autofree char *canonical = canonicalize_registry_uri (oci_registry_uri);
          if (canonical && json_object_has_member (auths, canonical))
            registry_auth = json_object_get_object_member (auths, canonical);
        }
      if (registry_auth != NULL)
        {
          if (json_object_has_member (registry_auth, "auth"))
            {
              return g_strdup (json_object_get_string_member (registry_auth, "auth"));
            }
        }
    }

  return NULL;
}


static char *
lookup_auth_from_config (const char *oci_registry_uri)
{
  /* These are flatpak specific, but use same format as docker/skopeo: */
  g_autofree char *flatpak_user_path = g_build_filename (g_get_user_config_dir (), "flatpak/oci-auth.json", NULL);
  const char *flatpak_global_path = "/etc/flatpak/oci-auth.json";

  /* These are what skopeo & co use as per:
     https://github.com/containers/image/blob/HEAD/pkg/docker/config/config.go#L34
  */
  g_autofree char *user_container_path = g_build_filename (g_get_user_runtime_dir (), "containers/auth.json", NULL);
  g_autofree char *container_path = g_strdup_printf ("/run/containers/%d/auth.json", getuid ());
  g_autofree char *docker_path = g_build_filename (g_get_home_dir (), ".docker/config.json", NULL);

  char *auth;

  auth = lookup_auth_from_config_path (oci_registry_uri, flatpak_user_path);
  if (auth != NULL)
    return auth;
  auth = lookup_auth_from_config_path (oci_registry_uri, flatpak_global_path);
  if (auth != NULL)
    return auth;
  auth = lookup_auth_from_config_path (oci_registry_uri, user_container_path);
  if (auth != NULL)
    return auth;
  auth = lookup_auth_from_config_path (oci_registry_uri, container_path);
  if (auth != NULL)
    return auth;
  auth = lookup_auth_from_config_path (oci_registry_uri, docker_path);
  if (auth != NULL)
    return auth;

  return NULL;
}

/* Note: This runs on a thread, so we can just block */
static gboolean
handle_request_ref_tokens (FlatpakAuthenticator *f_authenticator,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_handle_token,
                           GVariant *arg_authenticator_options,
                           const gchar *arg_remote,
                           const gchar *arg_remote_uri,
                           GVariant *arg_refs,
                           GVariant *arg_options,
                           const gchar *arg_parent_window)
{
  g_autofree char *request_path = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GError) anon_error = NULL;
  g_autoptr(AutoFlatpakAuthenticatorRequest) request = NULL;
  const char *auth = NULL;
  gboolean have_auth;
  const char *oci_registry_uri = NULL;
  gsize n_refs, i;
  gboolean no_interaction = FALSE;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autofree char *first_token = NULL;
  GVariantBuilder tokens;
  GVariantBuilder results;
  g_autofree char *sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  g_info ("handling Authenticator.RequestRefTokens");

  g_variant_lookup (arg_authenticator_options, "auth", "&s", &auth);
  have_auth = auth != NULL;

  if (!g_variant_lookup (arg_options, "xa.oci-registry-uri", "&s", &oci_registry_uri))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             _("Not a OCI remote"));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  g_variant_lookup (arg_options, "no-interaction", "b", &no_interaction);

  request_path = flatpak_auth_create_request_path (g_dbus_method_invocation_get_sender (invocation),
                                                   arg_handle_token, NULL);
  if (request_path == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             _("Invalid token"));
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

  flatpak_authenticator_complete_request_ref_tokens (f_authenticator, invocation, request_path);

  registry = flatpak_oci_registry_new (oci_registry_uri, FALSE, -1, NULL, &error);
  if (registry == NULL)
    return error_request (request, sender, error);


  /* Look up credentials in config files */
  if (!have_auth)
    {
      g_info ("Looking for %s in auth info", oci_registry_uri);
      auth = lookup_auth_from_config (oci_registry_uri);
      have_auth = auth != NULL;
    }

  /* Try to see if we can get a token without presenting credentials */
  n_refs = g_variant_n_children (arg_refs);
  if (!have_auth && n_refs > 0)
    {
      g_autoptr(GVariant) ref_data = g_variant_get_child_value (arg_refs, 0);

      g_info ("Trying anonymous authentication");

      first_token = get_token_for_ref (registry, ref_data, NULL, &anon_error);
      if (first_token != NULL)
        have_auth = TRUE;
      else
        {
          if (g_error_matches (anon_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_AUTHORIZED))
            {
              g_info ("Anonymous authentication failed: %s", anon_error->message);

              /* Continue trying with authentication below */
            }
          else
            {
              /* We failed with some weird reason (network issue maybe?) and it is unlikely
               * that adding some authentication will fix it. It will just cause a bad UX like
               * described in #3753, so just return the error early.
               */
              return error_request (request, sender, anon_error);
            }
        }
    }

  /* Prompt the user for credentials */
  n_refs = g_variant_n_children (arg_refs);
  if (!have_auth && n_refs > 0 &&
      !no_interaction)
    {
      g_autoptr(GVariant) ref_data = g_variant_get_child_value (arg_refs, 0);

      g_info ("Trying user/password based authentication");

      while (auth == NULL)
        {
          g_autofree char *test_auth = NULL;

          test_auth = run_basic_auth (request, sender, oci_registry_uri, error ? error->message : NULL);

          if (test_auth == NULL)
            return cancel_request (request, sender);

          g_clear_error (&error);

          first_token = get_token_for_ref (registry, ref_data, test_auth, &error);
          if (first_token != NULL)
            {
              auth = g_steal_pointer (&test_auth);
              have_auth = TRUE;
            }
          else
            {
              if (!g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_AUTHORIZED))
                return error_request (request, sender, error);
              else
                {
                  g_info ("Auth failed getting token: %s", error->message);
                  /* Keep error for reporting below, or clear on next iteration start */
                }
            }
        }
    }

  if (!have_auth && n_refs > 0)
    return error_request (request, sender, error ? error : anon_error);

  g_variant_builder_init (&tokens, G_VARIANT_TYPE ("a{sas}"));

  for (i = 0; i < n_refs; i++)
    {
      g_autoptr(GVariant) ref_data = g_variant_get_child_value (arg_refs, i);
      char *for_refs_strv[2] = { NULL, NULL};
      g_autofree char *token = NULL;

      if (i == 0 && first_token != NULL)
        {
          token = g_steal_pointer (&first_token);
        }
      else
        {
          token = get_token_for_ref (registry, ref_data, auth, &error);
          if (token == NULL)
            return error_request (request, sender, error);
        }

      g_variant_get_child (ref_data, 0, "&s", &for_refs_strv[0]);
      g_variant_builder_add (&tokens, "{s^as}", token, for_refs_strv);
    }

  g_variant_builder_init (&results, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&results, "{sv}", "tokens", g_variant_builder_end (&tokens));

  g_info ("emitting OK response");
  flatpak_authenticator_request_emit_response (request,
                                               FLATPAK_AUTH_RESPONSE_OK,
                                               g_variant_builder_end (&results));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
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

  g_info ("Bus acquired, creating skeleton");

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
  g_info ("Name acquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_info ("Name lost");
}


static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      peer_died (name);
    }
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
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, message_handler, NULL);

  g_info ("Started flatpak-authenticator");

  http_session = flatpak_create_http_session (PACKAGE_STRING);

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

  g_dbus_connection_signal_subscribe (session_bus,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  return 0;
}
