#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include "flatpak-utils.h"

static GMainLoop *loop = NULL;

static gboolean opt_verbose;
static gboolean opt_replace;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace", NULL },
  { NULL }
};

static struct {
  const char *name;
  GDBusNodeInfo *info;
} portal_interfaces [] = {
  { "org.freedesktop.portal.FileChooser" },
  { NULL }
};

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    printf ("XDP: %s\n", message);
  else
    printf ("%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const gchar *string)
{
  int is_tty = isatty (1);
  const char *prefix = "";
  const char *suffix = "";
  if (is_tty)
    {
      prefix = "\x1b[31m\x1b[1m"; /* red, bold */
      suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
    }
  fprintf (stderr, "%serror: %s%s\n", prefix, suffix, string);
}

static void
backend_call_callback (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (source_object);
  GDBusMethodInvocation *invocation = user_data;
  GVariant *retval;
  GError *error = NULL;

  retval = g_dbus_connection_call_finish (connection, res, &error);

  if (retval == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
    }
  else
    g_dbus_method_invocation_return_value (invocation, retval);
}

typedef struct {
  char *dbus_name;
  char **interfaces;
  char **use_in;
  gboolean subscribed;
} PortalImplementation;

static GList *implementations = NULL;

static void
register_portal (GKeyFile *key)
{
  PortalImplementation *impl = g_new0 (PortalImplementation, 1);

  impl->dbus_name = g_key_file_get_string (key, "portal", "DBusName", NULL);
  impl->interfaces = g_key_file_get_string_list (key, "portal", "Interfaces", NULL, NULL);
  impl->use_in = g_key_file_get_string_list (key, "portal", "UseIn", NULL, NULL);

  implementations = g_list_prepend (implementations, impl);

  if (opt_verbose)
    {
      g_autofree char *uses = g_strjoinv (", ", impl->use_in);
      g_autofree char *ifaces = g_strjoinv (", ", impl->interfaces);
      g_debug ("portal implementation for %s supports %s", uses, ifaces);
    }
}

static void
load_installed_portals (void)
{
  const char *portal_dir = PKGDATADIR "/portals";
  g_autoptr(GFile) dir = g_file_new_for_path (portal_dir);
  g_autoptr(GFileEnumerator) enumerator = NULL;

  enumerator = g_file_enumerate_children (dir, "*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

  if (enumerator == NULL)
    return;

  while (TRUE)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (enumerator, NULL, NULL);
      g_autoptr(GFile) child = NULL;
      g_autofree char *path = NULL;
      g_autoptr(GKeyFile) keyfile = NULL;
      const char *name;
      g_autoptr(GError) error = NULL;

      if (info == NULL)
        break;

      name = g_file_info_get_name (info);

      if (!g_str_has_suffix (name, ".portal"))
        continue;

      child = g_file_enumerator_get_child (enumerator, info);
      path = g_file_get_path (child);

      keyfile = g_key_file_new ();

      if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, &error))
        {
          g_warning ("error loading %s/%s: %s", portal_dir, name, error->message);
          continue;
        }
      else
        g_debug ("loading %s/%s", portal_dir, name);

      register_portal (keyfile);
    }
}

static PortalImplementation *
find_portal (const char *interface)
{
  const char *desktops_str = g_getenv ("XDG_SESSION_DESKTOP");
  g_auto(GStrv) desktops = NULL;
  int i;
  GList *l;

  if (desktops_str == NULL)
    desktops_str = "";

  desktops = g_strsplit (desktops_str, ":", -1);

  for (i = 0; desktops[i] != NULL; i++)
    {
     for (l = implementations; l != NULL; l = l->next)
        {
          PortalImplementation *impl = l->data;

          if (!g_strv_contains ((const char **)impl->interfaces, interface))
            continue;

          if (!g_strv_contains ((const char **)impl->use_in, desktops[i]))
            return impl;
        }
    }

  /* Fall back to *any* installed implementation */
  for (l = implementations; l != NULL; l = l->next)
    {
      PortalImplementation *impl = l->data;

      if (!g_strv_contains ((const char **)impl->interfaces, interface))
        continue;

      return impl;
    }

  return NULL;
}

static void
handle_backend_signal (GDBusConnection  *connection,
                       const gchar      *sender_name,
                       const gchar      *object_path,
                       const gchar      *interface_name,
                       const gchar      *signal_name,
                       GVariant         *parameters,
                       gpointer          user_data)
{
  GVariantBuilder b;
  const char *destination;
  gsize n_children, i;
  GError *error = NULL;
  char *real_interface_name;

  g_variant_get_child (parameters, 0, "&s", &destination);

  /* Strip out destination */
  n_children = g_variant_n_children (parameters);
  g_variant_builder_init (&b, G_VARIANT_TYPE_TUPLE);
  for (i = 1; i < n_children; i++)
    g_variant_builder_add_value (&b, g_variant_get_child_value (parameters, i));

  if (g_str_has_prefix (interface_name, "org.freedesktop.impl.portal."))
    real_interface_name = g_strconcat ("org.freedesktop.portal.", interface_name + strlen ("org.freedesktop.impl.portal."), NULL);
  else
    real_interface_name = g_strdup (interface_name);

  if (!g_dbus_connection_emit_signal (connection, destination,
                                      "/org/freedesktop/portal/desktop",
                                      real_interface_name,
                                      signal_name,
                                      g_variant_builder_end (&b),
                                      &error))
    {
      g_warning ("Error emitting signal: %s\n", error->message);
      g_error_free (error);
    }

  g_free (real_interface_name);
}

static void
got_app_id (GObject *source_object,
            GAsyncResult *res,
            gpointer user_data)
{
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (source_object);
  char *app_id;
  char *interface_name;
  GError *error = NULL;
  GVariant *params;
  GVariantBuilder b;
  gsize n_children, i;
  const char *real_iface;
  PortalImplementation *implementation;

  app_id = flatpak_invocation_lookup_app_id_finish (invocation, res, &error);
  if (app_id == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      return;
    }

  real_iface = g_dbus_method_invocation_get_interface_name (invocation);

  if (!g_str_has_prefix (real_iface, "org.freedesktop.portal."))
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR,
                                             G_IO_ERROR_NOT_SUPPORTED,
                                             "Interface %s is not supported by any implementation", real_iface);
      return;
    }

  implementation = find_portal (real_iface);
  if (implementation == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR,
                                             G_IO_ERROR_NOT_SUPPORTED,
                                             "Interface %s is not supported by any implementation", real_iface);
      return;
    }

  if (!implementation->subscribed)
    {
      g_dbus_connection_signal_subscribe (g_dbus_method_invocation_get_connection (invocation),
                                          implementation->dbus_name,
                                          NULL,
                                          NULL,
                                          "/org/freedesktop/portal/desktop",
                                          NULL,
                                          G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                          handle_backend_signal,
                                          implementation, NULL);
      implementation->subscribed = TRUE;
    }

  interface_name = g_strconcat ("org.freedesktop.impl.portal.", real_iface + strlen ("org.freedesktop.portal."), NULL);

  params = g_dbus_method_invocation_get_parameters (invocation);

  g_variant_builder_init (&b, G_VARIANT_TYPE_TUPLE);

  g_variant_builder_add_value (&b, g_variant_new_string (g_dbus_method_invocation_get_sender (invocation)));
  g_variant_builder_add_value (&b, g_variant_new_string (app_id));

  n_children = g_variant_n_children (params);

  for (i = 0; i < n_children; i++)
    g_variant_builder_add_value (&b, g_variant_get_child_value (params, i));

  g_dbus_connection_call (g_dbus_method_invocation_get_connection (invocation),
                          implementation->dbus_name,
                          g_dbus_method_invocation_get_object_path (invocation),
                          interface_name,
                          g_dbus_method_invocation_get_method_name (invocation),
                          g_variant_builder_end (&b),
                          NULL, // TODO: reply_type from method_info
                          G_DBUS_CALL_FLAGS_NONE,
                          G_MAXINT,
                          NULL,
                          backend_call_callback,
                          invocation);
  g_free (interface_name);
}

static void
method_call (GDBusConnection       *connection,
             const gchar           *sender,
             const gchar           *object_path,
             const gchar           *interface_name,
             const gchar           *method_name,
             GVariant              *parameters,
             GDBusMethodInvocation *invocation,
             gpointer               user_data)
{
  g_print ("method call %s %s\n", interface_name, method_name);
  flatpak_invocation_lookup_app_id (invocation, NULL,
                                    got_app_id, invocation);

}

static GDBusInterfaceVTable wrapper_vtable = {
  method_call,
  NULL,
  NULL,
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;
  guint id;
  int i;

  for (i = 0; portal_interfaces[i].name != NULL; i++)
    {
      GDBusInterfaceInfo *iface_info;
      iface_info =
        g_dbus_node_info_lookup_interface (portal_interfaces[i].info, portal_interfaces[i].name);
      g_assert (iface_info != NULL);

      id = g_dbus_connection_register_object (connection,
                                              "/org/freedesktop/portal/desktop",
                                              iface_info,
                                              &wrapper_vtable,
                                              NULL, NULL, &error);
      if (id == 0)
        {
          g_warning ("error registering object: %s\n", error->message);
          g_clear_error (&error);
        }
    }

  flatpak_connection_track_name_owners (connection);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("org.freedesktop.portal.desktop acquired");

}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_main_loop_quit (loop);
}

int
main (int argc, char *argv[])
{
  guint owner_id;
  g_autoptr(GError) error = NULL;
  GDBusConnection  *session_bus;
  GOptionContext *context;
  int i;

  setlocale (LC_ALL, "");

  /* Avoid even loading gvfs to avoid accidental confusion */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_printerr_handler (printerr_handler);

  context = g_option_context_new ("- desktop portal");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s", error->message);
      return 1;
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  load_installed_portals ();

  loop = g_main_loop_new (NULL, FALSE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s", error->message);
      return 2;
    }

  for (i = 0; portal_interfaces[i].name != NULL; i++)
    {
      g_autofree char *path = g_strdup_printf ("/org/freedesktop/portal/desktop/%s.xml", portal_interfaces[i].name);
      g_autoptr(GBytes) introspection_bytes = NULL;
      GDBusNodeInfo *info;

      introspection_bytes = g_resources_lookup_data (path, 0, NULL);
      g_assert (introspection_bytes != NULL);

      info = g_dbus_node_info_new_for_xml (g_bytes_get_data (introspection_bytes, NULL), NULL);
      g_assert (info != NULL);
      portal_interfaces[i].info = info;
    }

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.portal.Desktop",
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  return 0;
}
