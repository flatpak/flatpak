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

static PortalFlatpakExtensionManager *
create_extension_manager (PortalFlatpak *portal,
                          GError **error)
{
  static int counter = 1;
  PortalFlatpakExtensionManager *manager;
  g_autofree char *token = NULL;
  g_autofree char *manager_path = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *manager_handle = NULL;
  char *s;
  GVariantBuilder opt_builder;

  sender = g_strdup (g_dbus_connection_get_unique_name (connection) + 1);
  while ((s = strchr (sender, '.')) != NULL)
    *s = '_';

  token = g_strdup_printf ("test_ext_token%d", counter++);

  manager_path = g_strdup_printf ("%s/extension_manager/%s/%s", FLATPAK_PORTAL_PATH, sender, token);
  manager = portal_flatpak_extension_manager_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_NONE,
                                                             portal_name, manager_path,
                                                             NULL, error);
  if (manager == NULL)
    return NULL;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "handle_token", g_variant_new_string (token));

  if (!portal_flatpak_call_create_extension_manager_sync (portal,
                                                          g_variant_builder_end (&opt_builder),
                                                          &manager_handle,
                                                          NULL, error))
    return NULL;

  return manager;
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

typedef enum {
  PROGRESS_STATUS_RUNNING = 0,
  PROGRESS_STATUS_EMPTY   = 1,
  PROGRESS_STATUS_DONE    = 2,
  PROGRESS_STATUS_ERROR   = 3
} ProgressStatus;

typedef struct {
  GMainLoop *loop;
  int expected_end_status;
  int exit_status;
  char *last_error;
  char *last_error_message;
} ExtensionOpData;

static void
extension_progress_cb (PortalFlatpakExtensionManager *object,
                       GVariant *arg_info,
                       ExtensionOpData *data)
{
  guint32 status = 0;
  const char *error = "";
  const char *error_message = "";

  g_variant_lookup (arg_info, "status", "u", &status);
  g_variant_lookup (arg_info, "error", "&s", &error);
  g_variant_lookup (arg_info, "error_message", "&s", &error_message);

  g_print ("extension_progress status=%d error=%s error_message='%s'\n",
           status, error, error_message);

  if (status != PROGRESS_STATUS_RUNNING)
    {
      g_free (data->last_error);
      g_free (data->last_error_message);
      data->last_error = g_strdup (error);
      data->last_error_message = g_strdup (error_message);

      if ((int) status != data->expected_end_status)
        {
          g_printerr ("Unexpected end status: %d (expected %d, error %s: %s)\n",
                      status, data->expected_end_status, error, error_message);
          data->exit_status = 1;
        }
      g_main_loop_quit (data->loop);
    }
}

static int
list_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakExtensionManager *manager;
  g_autoptr(GVariant) extensions = NULL;
  GVariantBuilder opt_builder;

  manager = create_extension_manager (portal, &error);
  if (manager == NULL)
    {
      g_printerr ("Error creating extension manager: %s\n", error->message);
      return 1;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (!portal_flatpak_extension_manager_call_list_extensions_sync (
        manager, g_variant_builder_end (&opt_builder),
        &extensions, NULL, &error))
    {
      g_printerr ("Error calling list extensions: %s\n", error->message);
      return 1;
    }

  g_print ("list_extensions returned %lu extensions\n",
           (unsigned long) g_variant_n_children (extensions));

  /* Print each extension with all fields for shell-level assertions */
  for (gsize i = 0; i < g_variant_n_children (extensions); i++)
    {
      g_autoptr(GVariant) ext = g_variant_get_child_value (extensions, i);
      const char *name = "";
      const char *arch = "";
      const char *branch = "";
      const char *commit = "";
      const char *origin = "";
      gboolean installed = FALSE;
      guint64 download_size = 0;
      guint64 installed_size = 0;

      g_variant_lookup (ext, "name", "&s", &name);
      g_variant_lookup (ext, "arch", "&s", &arch);
      g_variant_lookup (ext, "branch", "&s", &branch);
      g_variant_lookup (ext, "commit", "&s", &commit);
      g_variant_lookup (ext, "origin", "&s", &origin);
      g_variant_lookup (ext, "installed", "b", &installed);
      g_variant_lookup (ext, "download-size", "t", &download_size);
      g_variant_lookup (ext, "installed-size", "t", &installed_size);

      g_print ("  extension: name=%s arch=%s branch=%s installed=%d origin=%s commit=%s download-size=%lu installed-size=%lu\n",
               name, arch, branch, installed, origin, commit,
               (unsigned long) download_size, (unsigned long) installed_size);
    }

  return 0;
}

static int
install_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakExtensionManager *manager;
  GVariantBuilder opt_builder;
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  ExtensionOpData data = { loop, PROGRESS_STATUS_DONE, 0, NULL, NULL };
  const char *ref;

  manager = create_extension_manager (portal, &error);
  if (manager == NULL)
    {
      g_printerr ("Error creating extension manager: %s\n", error->message);
      return 1;
    }

  ref = g_getenv ("TEST_EXTENSION_REF");
  if (ref == NULL)
    {
      g_printerr ("TEST_EXTENSION_REF not set\n");
      return 1;
    }

  g_signal_connect (manager, "progress", G_CALLBACK (extension_progress_cb), &data);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (!portal_flatpak_extension_manager_call_install_sync (
        manager, ref, g_variant_builder_end (&opt_builder),
        NULL, &error))
    {
      g_printerr ("Error calling install: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  g_free (data.last_error);
  g_free (data.last_error_message);
  return data.exit_status;
}

static int
update_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakExtensionManager *manager;
  GVariantBuilder opt_builder;
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  ExtensionOpData data = { loop, PROGRESS_STATUS_DONE, 0, NULL, NULL };
  const char *ref;

  manager = create_extension_manager (portal, &error);
  if (manager == NULL)
    {
      g_printerr ("Error creating extension manager: %s\n", error->message);
      return 1;
    }

  ref = g_getenv ("TEST_EXTENSION_REF");
  if (ref == NULL)
    {
      g_printerr ("TEST_EXTENSION_REF not set\n");
      return 1;
    }

  g_signal_connect (manager, "progress", G_CALLBACK (extension_progress_cb), &data);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (!portal_flatpak_extension_manager_call_update_sync (
        manager, ref, g_variant_builder_end (&opt_builder),
        NULL, &error))
    {
      g_printerr ("Error calling update: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  g_free (data.last_error);
  g_free (data.last_error_message);
  return data.exit_status;
}

static int
uninstall_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakExtensionManager *manager;
  GVariantBuilder opt_builder;
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  ExtensionOpData data = { loop, PROGRESS_STATUS_DONE, 0, NULL, NULL };
  const char *ref;

  manager = create_extension_manager (portal, &error);
  if (manager == NULL)
    {
      g_printerr ("Error creating extension manager: %s\n", error->message);
      return 1;
    }

  ref = g_getenv ("TEST_EXTENSION_REF");
  if (ref == NULL)
    {
      g_printerr ("TEST_EXTENSION_REF not set\n");
      return 1;
    }

  g_signal_connect (manager, "progress", G_CALLBACK (extension_progress_cb), &data);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (!portal_flatpak_extension_manager_call_uninstall_sync (
        manager, ref, g_variant_builder_end (&opt_builder),
        NULL, &error))
    {
      g_printerr ("Error calling uninstall: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  g_free (data.last_error);
  g_free (data.last_error_message);
  return data.exit_status;
}

static int
install_bad_ref_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakExtensionManager *manager;
  GVariantBuilder opt_builder;

  manager = create_extension_manager (portal, &error);
  if (manager == NULL)
    {
      g_printerr ("Error creating extension manager: %s\n", error->message);
      return 1;
    }

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  /* Try to install a ref that doesn't belong to this app - should be rejected */
  if (portal_flatpak_extension_manager_call_install_sync (
        manager, "runtime/org.other.App.Extension/x86_64/stable",
        g_variant_builder_end (&opt_builder),
        NULL, &error))
    {
      g_printerr ("Expected install to be rejected, but it succeeded\n");
      return 1;
    }

  g_print ("install bad ref correctly rejected: %s\n", error->message);

  /* Verify error is an access denied error */
  if (!g_dbus_error_is_remote_error (error) ||
      !strstr (error->message, "does not belong to app"))
    {
      g_printerr ("Error message doesn't mention access denial: %s\n", error->message);
      return 1;
    }

  return 0;
}

static int
install_not_found_test (PortalFlatpak *portal, int status_pipe)
{
  g_autoptr(GError) error = NULL;
  PortalFlatpakExtensionManager *manager;
  GVariantBuilder opt_builder;
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  ExtensionOpData data = { loop, PROGRESS_STATUS_ERROR, 0, NULL, NULL };
  const char *app_id;
  g_autofree char *ref = NULL;

  manager = create_extension_manager (portal, &error);
  if (manager == NULL)
    {
      g_printerr ("Error creating extension manager: %s\n", error->message);
      return 1;
    }

  /* Use the app's own ID prefix but a non-existent extension */
  app_id = g_getenv ("TEST_APP_ID");
  if (app_id == NULL)
    app_id = "org.test.Hello";

  ref = g_strdup_printf ("runtime/%s.NonExistent/x86_64/stable", app_id);

  g_signal_connect (manager, "progress", G_CALLBACK (extension_progress_cb), &data);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (!portal_flatpak_extension_manager_call_install_sync (
        manager, ref, g_variant_builder_end (&opt_builder),
        NULL, &error))
    {
      g_printerr ("Error calling install: %s\n", error->message);
      return 1;
    }

  g_main_loop_run (loop);

  /* Verify we got the expected error details */
  if (data.exit_status == 0)
    {
      if (data.last_error == NULL ||
          !strstr (data.last_error, "FileNotFound"))
        {
          g_printerr ("Expected FileNotFound error, got: %s\n",
                      data.last_error ? data.last_error : "(null)");
          data.exit_status = 1;
        }
      else if (data.last_error_message == NULL ||
               !strstr (data.last_error_message, "not found in any configured remote"))
        {
          g_printerr ("Expected 'not found in any configured remote' message, got: %s\n",
                      data.last_error_message ? data.last_error_message : "(null)");
          data.exit_status = 1;
        }
    }

  g_free (data.last_error);
  g_free (data.last_error_message);
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

  if (strcmp (argv[1], "list") == 0)
    test_callback = list_test;
  else if (strcmp (argv[1], "install") == 0)
    test_callback = install_test;
  else if (strcmp (argv[1], "update") == 0)
    test_callback = update_test;
  else if (strcmp (argv[1], "uninstall") == 0)
    test_callback = uninstall_test;
  else if (strcmp (argv[1], "install-bad-ref") == 0)
    test_callback = install_bad_ref_test;
  else if (strcmp (argv[1], "install-not-found") == 0)
    test_callback = install_not_found_test;
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
      perror ("fork:");
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

      res = run_test (pipes[1], argc > 2 ? argv[2] : "ext-pid.out", test_callback);
      /* If this returned early we have some setup failure, report it */
      write_status (res, pipes[1]);
      exit (res);
    }
}
