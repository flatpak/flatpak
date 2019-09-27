#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include "portal/flatpak-portal-dbus.h"

GDBusConnection *connection;

const char *portal_name = "org.freedesktop.portal.Flatpak";
const char *portal_path = "/org/freedesktop/portal/Flatpak";

static PortalFlatpakUpdateMonitor *
create_monitor (PortalFlatpak *portal,
                GCallback update_available_cb,
                gpointer cb_data,
                GError **error)
{
  static int counter = 1;
  PortalFlatpakUpdateMonitor *monitor;
  g_autofree char *token = NULL;
  g_autofree char *monitor_path = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *monitor_handle = NULL;
  char *s;
  GVariantBuilder opt_builder;

  sender = g_strdup (g_dbus_connection_get_unique_name (connection) + 1);
  while ((s = strchr (sender, '.')) != NULL)
    *s = '_';

  token = g_strdup_printf ("test_token%d", counter++);

  monitor_path = g_strdup_printf ("/org/freedesktop/portal/Flatpak/update_monitor/%s/%s", sender, token);
  monitor = portal_flatpak_update_monitor_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_NONE,
                                                          portal_name, monitor_path,
                                                          NULL, error);
  if (monitor == NULL)
    return NULL;

  if (update_available_cb)
    g_signal_connect (monitor, "update-available", update_available_cb, cb_data);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "handle_token", g_variant_new_string (token));

  if (!portal_flatpak_call_create_update_monitor_sync (portal,
                                                       g_variant_builder_end (&opt_builder),
                                                       &monitor_handle,
                                                       NULL, error))
    return NULL;

  return monitor;
}

static void
update_available (PortalFlatpakUpdateMonitor *object,
                  GVariant *arg_update_info,
                  gpointer user_data)
{
  const char *running, *local, *remote;

  g_variant_lookup (arg_update_info, "running-commit", "&s", &running);
  g_variant_lookup (arg_update_info, "local-commit", "&s", &local);
  g_variant_lookup (arg_update_info, "remote-commit", "&s", &remote);

  g_print ("update_available running=%s local=%s remote=%s\n", running, local, remote);
}

static void
write_status (int res, int status_pipe)
{
  char c = res;
  write (status_pipe, &c, 1);
}

typedef int (*TestCallback) (PortalFlatpak *portal, int status_pipe);

static int
monitor_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakUpdateMonitor *monitor;
  GMainLoop *loop;

  monitor = create_monitor (portal,
                            (GCallback)update_available, NULL,
                            &error);
  if (monitor == NULL)
    {
      g_printerr ("Error creating monitor: %s\n", error->message);
      return 1;
    }

  /* Return 0, to indicate we've successfully started monitor */
  write_status (0, status_pipe);

  g_print ("Entering main loop waiting for updates\n");
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  return 0;
}

static int
run_test (int status_pipe, const char *pidfile, TestCallback test)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpak *portal;
  g_autofree char *pid = NULL;

  pid = g_strdup_printf ("%d", getpid ());
  if (!g_file_set_contents (pidfile, pid, -1, &error))
    {
      g_printerr ("Error creating pidfile: %s\n", error->message);
      return 1;
    }

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_printerr ("Error connecting: %s\n", error->message);
      return 1;
    }

  portal = portal_flatpak_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_NONE,
                                          portal_name, portal_path,
                                          NULL, &error);
  if (portal == NULL)
    {
      g_printerr ("Error creating proxy: %s\n", error->message);
      return 1;
    }

  return test (portal, status_pipe);
}


int
main (int argc, char *argv[])
{
  pid_t pid;
  int pipes[2];
  TestCallback test_callback;

  if (argc < 2)
    {
      g_printerr ("No test command specified");
      return 1;
    }

  if (strcmp (argv[1], "monitor") == 0)
    test_callback = monitor_test;
  else
    {
      g_printerr ("Unknown command %s specified", argv[1]);
      return 1;
    }

  if (pipe (pipes) != 0)
    {
      perror ("pipe:");
      return 1;
    }

  pid = fork();
  if (pid == -1)
    {
      perror ("pipe:");
      return 1;
    }

  if (pid != 0)
    {
      char c;

      /* parent */
      close (pipes[1]);
      if (read (pipes[0], &c, 1) != 1)
        return 1;

      return c;
    }
  else
    {
      int res;

      close (pipes[0]);

      res = run_test (pipes[1], argc > 2 ? argv[2] : "pid.out", test_callback);
      /* If this returned early we have some setup failure, report it */
      write_status (res, pipes[1]);
      exit (res);
    }
}
