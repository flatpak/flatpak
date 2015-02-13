#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#include <gio/gio.h>
#include "libgsystem.h"

#include "xdg-app-run.h"
#include "xdg-app-utils.h"

gboolean
xdg_app_run_verify_environment_keys (const char **keys,
				     GError **error)
{
  const char *key;
  const char *environment_keys[] = {
    "x11", "wayland", "ipc", "pulseaudio", "system-dbus", "session-dbus",
    "network", "host-fs", "homedir", NULL
  };

  if (keys == NULL)
    return TRUE;

  if ((key = g_strv_subset (environment_keys, keys)) != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Unknown Environment key %s", key);
      return FALSE;
    }

  return TRUE;
}


static char *
extract_unix_path_from_dbus_address (const char *address)
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

void
xdg_app_run_add_x11_args (GPtrArray *argv_array)
{
  char *x11_socket = NULL;
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

      g_ptr_array_add (argv_array, g_strdup ("-x"));
      g_ptr_array_add (argv_array, x11_socket);
    }
}

void
xdg_app_run_add_wayland_args (GPtrArray *argv_array)
{
  char *wayland_socket = g_build_filename (g_get_user_runtime_dir (), "wayland-0", NULL);
  if (g_file_test (wayland_socket, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (argv_array, g_strdup ("-y"));
      g_ptr_array_add (argv_array, wayland_socket);
    }
  else
    g_free (wayland_socket);
}

void
xdg_app_run_add_no_x11_args (GPtrArray *argv_array)
{
  g_unsetenv ("DISPLAY");
}

void
xdg_app_run_add_pulseaudio_args (GPtrArray *argv_array)
{
  char *pulseaudio_socket = g_build_filename (g_get_user_runtime_dir (), "pulse/native", NULL);
  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (argv_array, g_strdup ("-p"));
      g_ptr_array_add (argv_array, pulseaudio_socket);
    }
}

void
xdg_app_run_add_system_dbus_args (GPtrArray *argv_array)
{
  const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");
  char *dbus_system_socket = NULL;

  dbus_system_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_system_socket == NULL &&
      g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    {
      dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");
    }

  if (dbus_system_socket != NULL)
    {
      g_ptr_array_add (argv_array, g_strdup ("-D"));
      g_ptr_array_add (argv_array, dbus_system_socket);
    }
}

void
xdg_app_run_add_session_dbus_args (GPtrArray *argv_array)
{
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  char *dbus_session_socket = NULL;

  dbus_session_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_session_socket != NULL)
    {
      g_ptr_array_add (argv_array, g_strdup ("-d"));
      g_ptr_array_add (argv_array, dbus_session_socket);
    }
}

void
xdg_app_run_add_environment_args (GPtrArray *argv_array,
				  GKeyFile *metakey,
				  const char **allow,
				  const char **forbid)
{
  const char *no_opts[1] = { NULL };

  if (allow == NULL)
    allow = no_opts;

  if (forbid == NULL)
    forbid = no_opts;

  if ((g_key_file_get_boolean (metakey, "Environment", "ipc", NULL) || g_strv_contains (allow, "ipc")) &&
      !g_strv_contains (forbid, "ipc"))
    {
      g_debug ("Allowing ipc access");
      g_ptr_array_add (argv_array, g_strdup ("-i"));
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "host-fs", NULL) || g_strv_contains (allow, "nost-fs")) &&
      !g_strv_contains (forbid, "host-fs"))
    {
      g_debug ("Allowing host-fs access");
      g_ptr_array_add (argv_array, g_strdup ("-f"));
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "homedir", NULL) || g_strv_contains (allow, "homedir")) &&
      !g_strv_contains (forbid, "homedir"))
    {
      g_debug ("Allowing homedir access");
      g_ptr_array_add (argv_array, g_strdup ("-H"));
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "network", NULL) || g_strv_contains (allow, "network")) &&
      !g_strv_contains (forbid, "network"))
    {
      g_debug ("Allowing network access");
      g_ptr_array_add (argv_array, g_strdup ("-n"));
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "x11", NULL) || g_strv_contains (allow, "x11")) &&
      !g_strv_contains (forbid, "x11"))
    {
      g_debug ("Allowing x11 access");
      xdg_app_run_add_x11_args (argv_array);
    }
  else
    {
      xdg_app_run_add_no_x11_args (argv_array);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "wayland", NULL) || g_strv_contains (allow, "wayland")) &&
      !g_strv_contains (forbid, "wayland"))
    {
      g_debug ("Allowing wayland access");
      xdg_app_run_add_wayland_args (argv_array);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "pulseaudio", NULL) || g_strv_contains (allow, "pulseaudio")) &&
      !g_strv_contains (forbid, "pulseaudio"))
    {
      g_debug ("Allowing pulseaudio access");
      xdg_app_run_add_pulseaudio_args (argv_array);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "system-dbus", NULL) || g_strv_contains (allow, "system-dbus")) &&
      !g_strv_contains (forbid, "system-dbus"))
    {
      g_debug ("Allowing system-dbus access");
      xdg_app_run_add_system_dbus_args (argv_array);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "session-dbus", NULL) || g_strv_contains (allow, "session-dbus")) &&
      !g_strv_contains (forbid, "session-dbus"))
    {
      g_debug ("Allowing session-dbus access");
      xdg_app_run_add_session_dbus_args (argv_array);
    }
}
