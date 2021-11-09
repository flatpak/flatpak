#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include "portal/flatpak-portal.h"
#include "portal/flatpak-portal-dbus.h"

GDBusConnection *connection;

const char *portal_name = FLATPAK_PORTAL_BUS_NAME;
const char *portal_path = FLATPAK_PORTAL_PATH;

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

  monitor_path = g_strdup_printf ("%s/update_monitor/%s/%s", FLATPAK_PORTAL_PATH, sender, token);
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
  const ssize_t write_ret = write (status_pipe, &c, 1);
  if (write_ret < 0)
    g_printerr ("write_status() failed with %zd\n", write_ret);
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

typedef enum {
  PROGRESS_STATUS_RUNNING = 0,
  PROGRESS_STATUS_EMPTY   = 1,
  PROGRESS_STATUS_DONE    = 2,
  PROGRESS_STATUS_ERROR   = 3
} UpdateStatus;

typedef struct {
  GMainLoop *loop;
  int expected_end_status;
  int expected_n_ops;
  const char *expected_error;

  int expected_op;

  int exit_status;
} UpdateData;

static void
progress_cb (PortalFlatpakUpdateMonitor *object,
             GVariant *arg_info,
             UpdateData *data)
{
  guint32 op = 0;
  guint32 n_ops = 0;
  guint32 progress = 0;
  guint32 status = 0;
  const char *error = "";
  const char *error_message = "";

  g_variant_lookup (arg_info, "op", "u", &op);
  g_variant_lookup (arg_info, "n_ops", "u", &n_ops);
  g_variant_lookup (arg_info, "progress", "u", &progress);
  g_variant_lookup (arg_info, "status", "u", &status);
  g_variant_lookup (arg_info, "error", "&s", &error);
  g_variant_lookup (arg_info, "error_message", "&s", &error_message);

  g_print ("progress op=%d n_ops=%d progress=%d status=%d error=%s error_message='%s'\n", op, n_ops, progress, status, error, error_message);

  if (status == PROGRESS_STATUS_RUNNING)
    {
      if (n_ops != data->expected_n_ops)
        {
          g_printerr ("Unexpected number of ops: %d (expected %d)\n", n_ops, data->expected_n_ops);
          data->exit_status = 1;
          g_main_loop_quit (data->loop);
        }

      if (op == data->expected_op)
        {
          if (progress == 100)
            data->expected_op = op + 1;
        }
      else
        {
          g_printerr ("Unexpected op nr: %d (expected %d)\n", op, data->expected_op);
          data->exit_status = 1;
          g_main_loop_quit (data->loop);
        }
    }
  else
    {
      if (data->expected_end_status != status)
        {
          g_printerr ("Unexpected end status: %d (error %s: %s)\n", status, error, error_message);
          data->exit_status = 1;
        }
      else if (status == PROGRESS_STATUS_DONE)
        {
          if (data->expected_op != data->expected_n_ops)
            {
              g_printerr ("Unexpected number of ops seen: %d, should be %d\n", data->expected_op, data->expected_n_ops);
              data->exit_status = 1;
            }
        }
      else if (status == PROGRESS_STATUS_ERROR)
        {
          if (data->expected_error != NULL &&
              strcmp (data->expected_error, error) != 0)
            {
              g_printerr ("Unexpected error: %s, should be %s\n", error, data->expected_error);
              data->exit_status = 1;
            }
        }

      g_main_loop_quit (data->loop);
    }
}

static int
update_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakUpdateMonitor *monitor;
  GVariantBuilder opt_builder;
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  UpdateData data = { loop };

  monitor = create_monitor (portal, NULL, NULL, &error);
  if (monitor == NULL)
    {
      g_printerr ("Error creating monitor: %s\n", error->message);
      return 1;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_signal_connect (monitor, "progress", G_CALLBACK (progress_cb), &data);

  data.expected_end_status = PROGRESS_STATUS_DONE;
  data.expected_n_ops = 2;

  if (!portal_flatpak_update_monitor_call_update_sync (monitor, "",
                                                       g_variant_builder_end (&opt_builder),
                                                       NULL, &error))
    {
      g_printerr ("Error calling update: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  return data.exit_status;
}

static int
update_null_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakUpdateMonitor *monitor;
  GVariantBuilder opt_builder;
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  UpdateData data = { loop };

  monitor = create_monitor (portal, NULL, NULL, &error);
  if (monitor == NULL)
    {
      g_printerr ("Error creating monitor: %s\n", error->message);
      return 1;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_signal_connect (monitor, "progress", G_CALLBACK (progress_cb), &data);

  data.expected_end_status = PROGRESS_STATUS_EMPTY;
  data.expected_n_ops = 0;

  if (!portal_flatpak_update_monitor_call_update_sync (monitor, "",
                                                       g_variant_builder_end (&opt_builder),
                                                       NULL, &error))
    {
      g_printerr ("Error calling update: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  return data.exit_status;
}

static int
update_fail_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakUpdateMonitor *monitor;
  GVariantBuilder opt_builder;
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  UpdateData data = { loop };

  monitor = create_monitor (portal, NULL, NULL, &error);
  if (monitor == NULL)
    {
      g_printerr ("Error creating monitor: %s\n", error->message);
      return 1;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_signal_connect (monitor, "progress", G_CALLBACK (progress_cb), &data);

  data.expected_end_status = PROGRESS_STATUS_ERROR;
  data.expected_n_ops = 2;

  if (!portal_flatpak_update_monitor_call_update_sync (monitor, "",
                                                       g_variant_builder_end (&opt_builder),
                                                       NULL, &error))
    {
      g_printerr ("Error calling update: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  return data.exit_status;
}

static int
update_notsupp_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakUpdateMonitor *monitor;
  GVariantBuilder opt_builder;
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  UpdateData data = { loop };

  monitor = create_monitor (portal, NULL, NULL, &error);
  if (monitor == NULL)
    {
      g_printerr ("Error creating monitor: %s\n", error->message);
      return 1;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_signal_connect (monitor, "progress", G_CALLBACK (progress_cb), &data);

  data.expected_end_status = PROGRESS_STATUS_ERROR;
  data.expected_n_ops = 2;
  data.expected_error = "org.freedesktop.DBus.Error.NotSupported";

  if (!portal_flatpak_update_monitor_call_update_sync (monitor, "",
                                                       g_variant_builder_end (&opt_builder),
                                                       NULL, &error))
    {
      g_printerr ("Error calling update: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  return data.exit_status;
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
  else if (strcmp (argv[1], "update") == 0)
    test_callback = update_test;
  else if (strcmp (argv[1], "update-null") == 0)
    test_callback = update_null_test;
  else if (strcmp (argv[1], "update-fail") == 0)
    test_callback = update_fail_test;
  else if (strcmp (argv[1], "update-notsupp") == 0)
    test_callback = update_notsupp_test;
  else
    {
      g_printerr ("Unknown command %s specified\n", argv[1]);
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
