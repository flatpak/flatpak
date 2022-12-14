#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>

static guint name_owner_id = 0;
static GMainLoop *main_loop;

static void
access_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  if (g_strcmp0 (method_name, "AccessDialog") == 0)
    {
      GVariantBuilder res_builder;

      /* Always allow */
      g_variant_builder_init (&res_builder, G_VARIANT_TYPE_VARDICT);
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(u@a{sv})",
                                                            0,
                                                            g_variant_builder_end (&res_builder)));
    }
  else
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "Method %s is not implemented on interface %s",
                                           method_name, interface_name);
}


static const GDBusArgInfo access_dialog_IN_ARG_handle = { -1, (gchar *) "handle", (gchar *) "o", NULL };
static const GDBusArgInfo access_dialog_IN_ARG_app_id = { -1, (gchar *) "app_id",  (gchar *) "s",  NULL };
static const GDBusArgInfo access_dialog_IN_ARG_parent_window = {  -1,  (gchar *) "parent_window",  (gchar *) "s",  NULL };
static const GDBusArgInfo access_dialog_IN_ARG_title = { -1,  (gchar *) "title",  (gchar *) "s",  NULL };
static const GDBusArgInfo access_dialog_IN_ARG_subtitle = {  -1,  (gchar *) "subtitle",  (gchar *) "s",  NULL };
static const GDBusArgInfo access_dialog_IN_ARG_body = {  -1,  (gchar *) "body",  (gchar *) "s",  NULL };
static const GDBusArgInfo access_dialog_IN_ARG_options = {  -1,  (gchar *) "options",  (gchar *) "a{sv}",  NULL };
static const GDBusArgInfo * const access_dialog_IN_ARG_pointers[] = {
  &access_dialog_IN_ARG_handle,
  &access_dialog_IN_ARG_app_id,
  &access_dialog_IN_ARG_parent_window,
  &access_dialog_IN_ARG_title,
  &access_dialog_IN_ARG_subtitle,
  &access_dialog_IN_ARG_body,
  &access_dialog_IN_ARG_options,
  NULL
};

static const GDBusArgInfo access_dialog_OUT_ARG_response = { -1, (gchar *) "response", (gchar *) "u", NULL };
static const GDBusArgInfo access_dialog_OUT_ARG_results = { -1, (gchar *) "results", (gchar *) "a{sv}", NULL };
static const GDBusArgInfo * const access_dialog_OUT_ARG_pointers[] = {
  &access_dialog_OUT_ARG_response,
  &access_dialog_OUT_ARG_results,
  NULL
};

static const GDBusMethodInfo access_dialog = {
 -1,
 (gchar *) "AccessDialog",
 (GDBusArgInfo **) &access_dialog_IN_ARG_pointers,
 (GDBusArgInfo **) &access_dialog_OUT_ARG_pointers,
 NULL
};

static const GDBusMethodInfo * const access_dialog_methods[] = {
  &access_dialog,
  NULL
};

static const GDBusInterfaceInfo access_interface_info = {
  -1,
  "org.freedesktop.impl.portal.Access",
  (GDBusMethodInfo **) &access_dialog_methods,
  (GDBusSignalInfo **) NULL,
  (GDBusPropertyInfo **) NULL,
  NULL,
};

static const GDBusInterfaceVTable access_vtable = {
  access_method_call, /* _method_call */
  NULL, /* _get_property */
  NULL  /* _set_property */
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;
  guint registration_id;

  g_info ("Bus acquired, creating skeleton");
  registration_id = g_dbus_connection_register_object (connection,
                                                       "/org/freedesktop/portal/desktop",
                                                       (GDBusInterfaceInfo *) &access_interface_info,
                                                       &access_vtable,
                                                       NULL, NULL, &error);
  g_assert_true (registration_id != 0);
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

int
main (int argc, char *argv[])
{
  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  "org.freedesktop.impl.portal.desktop.test",
                                  G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  return 0;
}
