#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;
static char *opt_branch;
static gchar **opt_rest = NULL;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to install for", NULL },
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, "Branch to run", NULL },
  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_rest,
    "Special option that collects any remaining arguments for us" },
  { NULL }
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gs_free gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

#define _gs_unref_keyfile __attribute__ ((cleanup(gs_local_keyfile_unref)))

static char *
extract_unix_path_from_dbus_addres (const char *address)
{
  const char *path, *path_end;

  if (address == NULL)
    return NULL;

  if (!g_str_has_prefix (address, "unix:"))
    return NULL;

  path = strstr (address, "path=");
  if (path == NULL)
    return NULL;
  path += strlen ("path=");
  path_end = path;
  while (*path_end != 0 && *path_end != ',')
    path_end++;

  return g_strndup (path, path_end - path);
}

gboolean
xdg_app_builtin_run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_object XdgAppDir *user_dir = NULL;
  gs_unref_object XdgAppDir *system_dir = NULL;
  gs_unref_variant_builder GVariantBuilder *optbuilder = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *var = NULL;
  gs_unref_object GFile *var_tmp = NULL;
  gs_unref_object GFile *var_run = NULL;
  gs_unref_object GFile *app_deploy = NULL;
  gs_unref_object GFile *app_files = NULL;
  gs_unref_object GFile *runtime_deploy = NULL;
  gs_unref_object GFile *runtime_files = NULL;
  gs_unref_object GFile *metadata = NULL;
  gs_free char *metadata_contents = NULL;
  gs_free char *runtime = NULL;
  gs_free char *runtime_ref = NULL;
  gs_free char *app_ref = NULL;
  gs_free char *x11_socket = NULL;
  gs_free char *pulseaudio_socket = NULL;
  gs_free char *dbus_system_socket = NULL;
  gs_free char *dbus_session_socket = NULL;
  _gs_unref_keyfile GKeyFile *metakey = NULL;
  gs_free_error GError *my_error = NULL;
  gs_free_error GError *my_error2 = NULL;
  gs_unref_ptrarray GPtrArray *argv_array = NULL;
  gsize metadata_size;
  const char *app;
  const char *branch = "master";
  int i;

  context = g_option_context_new ("APP COMMAND - Run an app");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (g_strv_length (opt_rest) < 2)
    {
      usage_error (context, "APP and COMMAND must be specified", error);
      goto out;
    }

  app = opt_rest[0];

  if (opt_branch)
    branch = opt_branch;

  app_ref = xdg_app_build_app_ref (app, branch, opt_arch);

  user_dir = xdg_app_dir_get_user ();
  system_dir = xdg_app_dir_get_system ();

  app_deploy = xdg_app_dir_get_if_deployed (user_dir, app_ref, NULL, cancellable);
  if (app_deploy == NULL)
    app_deploy = xdg_app_dir_get_if_deployed (system_dir, app_ref, NULL, cancellable);
  if (app_deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "App %s branch %s not installed", app, branch);
      goto out;
    }

  metadata = g_file_get_child (app_deploy, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    goto out;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    goto out;

  runtime = g_key_file_get_string (metakey, "Application", "runtime", error);
  if (runtime == NULL)
    goto out;

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_deploy = xdg_app_dir_get_if_deployed (user_dir, runtime_ref, NULL, cancellable);
  if (runtime_deploy == NULL)
    runtime_deploy = xdg_app_dir_get_if_deployed (system_dir, runtime_ref, NULL, cancellable);
  if (runtime_deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Required runtime %s not installed", runtime);
      goto out;
    }

  if (!xdg_app_dir_ensure_path (user_dir, cancellable, error))
    goto out;

  var = xdg_app_dir_get_app_data (user_dir, app);
  if (!gs_file_ensure_directory (var, TRUE, cancellable, error))
    goto out;

  var_tmp = g_file_get_child (var, "tmp");
  var_run = g_file_get_child (var, "run");

  if (!g_file_make_symbolic_link (var_tmp, "/tmp", cancellable, &my_error) &&
      !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      g_propagate_error (error, my_error);
      my_error = NULL;
      goto out;
    }

  if (!g_file_make_symbolic_link (var_run, "/run", cancellable, &my_error2) &&
      !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      g_propagate_error (error, my_error2);
      my_error2 = NULL;
      goto out;
    }

  app_files = g_file_get_child (app_deploy, "files");
  runtime_files = g_file_get_child (runtime_deploy, "files");

  argv_array = g_ptr_array_new ();
  g_ptr_array_add (argv_array, HELPER);

  if (g_key_file_get_boolean (metakey, "Environment", "ipc", NULL))
    g_ptr_array_add (argv_array, "-i");

  if (g_key_file_get_boolean (metakey, "Environment", "host-fs", NULL))
    g_ptr_array_add (argv_array, "-f");

  if (g_key_file_get_boolean (metakey, "Environment", "homedir", NULL))
    g_ptr_array_add (argv_array, "-H");

  if (g_key_file_get_boolean (metakey, "Environment", "network", NULL))
    g_ptr_array_add (argv_array, "-n");

  if (g_key_file_get_boolean (metakey, "Environment", "x11", NULL))
    {
      const char *display = g_getenv ("DISPLAY");

      if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
        {
          const char *display_nr = &display[1];
          const char *display_nr_end = display_nr;
          gs_free char *d = NULL;

          while (g_ascii_isdigit (*display_nr_end))
            display_nr_end++;

          d = g_strndup (display_nr, display_nr_end - display_nr);
          x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

          g_ptr_array_add (argv_array, "-x");
          g_ptr_array_add (argv_array, x11_socket);
        }
    }
  else
    g_unsetenv ("DISPLAY");

  if (g_key_file_get_boolean (metakey, "Environment", "pulseaudio", NULL))
    {
      pulseaudio_socket = g_build_filename (g_get_user_data_dir(), "pulse/native", NULL);
      if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
        {
          g_ptr_array_add (argv_array, "-p");
          g_ptr_array_add (argv_array, pulseaudio_socket);
        }
    }

  if (g_key_file_get_boolean (metakey, "Environment", "system-dbus", NULL))
    {
      const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");

      dbus_system_socket = extract_unix_path_from_dbus_addres (dbus_address);
      if (dbus_system_socket == NULL &&
          g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
        {
          dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");
        }

      if (dbus_system_socket != NULL)
        {
          g_ptr_array_add (argv_array, "-D");
          g_ptr_array_add (argv_array, dbus_system_socket);
        }
    }

  if (g_key_file_get_boolean (metakey, "Environment", "session-dbus", NULL))
    {
      const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");

      dbus_session_socket = extract_unix_path_from_dbus_addres (dbus_address);
      if (dbus_session_socket != NULL)
        {
          g_ptr_array_add (argv_array, "-d");
          g_ptr_array_add (argv_array, dbus_session_socket);
        }
    }

  g_ptr_array_add (argv_array, "-a");
  g_ptr_array_add (argv_array, (char *)gs_file_get_path_cached (app_files));
  g_ptr_array_add (argv_array, "-v");
  g_ptr_array_add (argv_array, (char *)gs_file_get_path_cached (var));
  g_ptr_array_add (argv_array, (char *)gs_file_get_path_cached (runtime_files));

  for (i = 1; opt_rest[i] != NULL; i++)
    g_ptr_array_add (argv_array, opt_rest[i]);

  g_ptr_array_add (argv_array, NULL);

  if (!execv (HELPER, (char **)argv_array->pdata))
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start app");
      goto out;
    }

  /* Not actually reached... */
  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
