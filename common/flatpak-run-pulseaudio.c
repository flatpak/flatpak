/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2019 Red Hat, Inc
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
#include "flatpak-run-pulseaudio-private.h"

#include "flatpak-utils-private.h"

/* Try to find a default server from a pulseaudio confguration file */
static char *
flatpak_run_get_pulseaudio_server_user_config (const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GFileInputStream) input_stream = NULL;
  g_autoptr(GDataInputStream) data_stream = NULL;
  size_t len;

  input_stream = g_file_read (file, NULL, &my_error);
  if (my_error)
    {
      g_info ("Pulseaudio user configuration file '%s': %s", path, my_error->message);
      return NULL;
    }

  data_stream = g_data_input_stream_new (G_INPUT_STREAM (input_stream));

  while (TRUE)
    {
      g_autofree char *line = g_data_input_stream_read_line (data_stream, &len, NULL, NULL);
      if (line == NULL)
        break;

      g_strchug (line);

      if ((*line  == '\0') || (*line == ';') || (*line == '#'))
        continue;

      if (g_str_has_prefix (line, ".include "))
        {
          g_autofree char *rec_path = g_strdup (line + 9);
          g_strstrip (rec_path);
          char *found = flatpak_run_get_pulseaudio_server_user_config (rec_path);
          if (found)
            return found;
        }
      else if (g_str_has_prefix (line, "["))
        {
          return NULL;
        }
      else
        {
          g_auto(GStrv) tokens = g_strsplit (line, "=", 2);

          if ((tokens[0] != NULL) && (tokens[1] != NULL))
            {
              g_strchomp (tokens[0]);
              if (strcmp ("default-server", tokens[0]) == 0)
                {
                  g_strstrip (tokens[1]);
                  g_info ("Found pulseaudio socket from configuration file '%s': %s", path, tokens[1]);
                  return g_strdup (tokens[1]);
                }
            }
        }
    }

  return NULL;
}

static char *
flatpak_run_get_pulseaudio_server (void)
{
  const char * pulse_clientconfig;
  char *pulse_server;
  g_autofree char *pulse_user_config = NULL;

  pulse_server = g_strdup (g_getenv ("PULSE_SERVER"));
  if (pulse_server)
    return pulse_server;

  pulse_clientconfig = g_getenv ("PULSE_CLIENTCONFIG");
  if (pulse_clientconfig)
    return flatpak_run_get_pulseaudio_server_user_config (pulse_clientconfig);

  pulse_user_config = g_build_filename (g_get_user_config_dir (), "pulse/client.conf", NULL);
  pulse_server = flatpak_run_get_pulseaudio_server_user_config (pulse_user_config);
  if (pulse_server)
    return pulse_server;

  pulse_server = flatpak_run_get_pulseaudio_server_user_config ("/etc/pulse/client.conf");
  if (pulse_server)
    return pulse_server;

  return NULL;
}

/*
 * Parse a PulseAudio server string, as documented on
 * https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/User/ServerStrings/.
 * Returns the first supported server address, or NULL if none are supported,
 * or NULL with @remote set if @value points to a remote server.
 */
static char *
flatpak_run_parse_pulse_server (const char *value,
                                gboolean   *remote)
{
  g_auto(GStrv) servers = g_strsplit (value, " ", 0);
  gsize i;

  for (i = 0; servers[i] != NULL; i++)
    {
      const char *server = servers[i];
      if (g_str_has_prefix (server, "{"))
        {
          /*
           * TODO: compare the value within {} to the local hostname and D-Bus machine ID,
           * and skip if it matches neither.
           */
          const char * closing = strstr (server, "}");
          if (closing == NULL)
            continue;
          server = closing + 1;
        }

      if (g_str_has_prefix (server, "unix:"))
        return g_strdup (server + 5);
      if (server[0] == '/')
        return g_strdup (server);

      if (g_str_has_prefix (server, "tcp:"))
        {
          *remote = TRUE;
          return NULL;
        }
    }

  return NULL;
}

/*
 * Get the machine ID as used by PulseAudio. This is the systemd/D-Bus
 * machine ID, or failing that, the hostname.
 */
static char *
flatpak_run_get_pulse_machine_id (void)
{
  static const char * const machine_ids[] =
  {
    "/etc/machine-id",
    "/var/lib/dbus/machine-id",
  };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (machine_ids); i++)
    {
      g_autofree char *ret = NULL;

      if (g_file_get_contents (machine_ids[i], &ret, NULL, NULL))
        {
          gsize j;

          g_strstrip (ret);

          for (j = 0; ret[j] != '\0'; j++)
            {
              if (!g_ascii_isxdigit (ret[j]))
                break;
            }

          if (ret[0] != '\0' && ret[j] == '\0')
            return g_steal_pointer (&ret);
        }
    }

  return g_strdup (g_get_host_name ());
}

/*
 * Get the directory used by PulseAudio for its configuration.
 */
static char *
flatpak_run_get_pulse_home (void)
{
  /* Legacy path ~/.pulse is tried first, for compatibility */
  {
    const char *parent = g_get_home_dir ();
    g_autofree char *ret = g_build_filename (parent, ".pulse", NULL);

    if (g_file_test (ret, G_FILE_TEST_IS_DIR))
      return g_steal_pointer (&ret);
  }

  /* The more modern path, usually ~/.config/pulse */
  {
    const char *parent = g_get_user_config_dir ();
    /* Usually ~/.config/pulse */
    g_autofree char *ret = g_build_filename (parent, "pulse", NULL);

    if (g_file_test (ret, G_FILE_TEST_IS_DIR))
      return g_steal_pointer (&ret);
  }

  return NULL;
}

/*
 * Get the runtime directory used by PulseAudio for its socket.
 */
static char *
flatpak_run_get_pulse_runtime_dir (void)
{
  const char *val = NULL;

  val = g_getenv ("PULSE_RUNTIME_PATH");

  if (val != NULL)
    return realpath (val, NULL);

  {
    const char *user_runtime_dir = g_get_user_runtime_dir ();

    if (user_runtime_dir != NULL)
      {
        g_autofree char *dir = g_build_filename (user_runtime_dir, "pulse", NULL);

        if (g_file_test (dir, G_FILE_TEST_IS_DIR))
          return realpath (dir, NULL);
      }
  }

  {
    g_autofree char *pulse_home = flatpak_run_get_pulse_home ();
    g_autofree char *machine_id = flatpak_run_get_pulse_machine_id ();

    if (pulse_home != NULL && machine_id != NULL)
      {
        /* This is usually a symlink, but we take its realpath() anyway */
        g_autofree char *dir = g_strdup_printf ("%s/%s-runtime", pulse_home, machine_id);

        if (g_file_test (dir, G_FILE_TEST_IS_DIR))
          return realpath (dir, NULL);
      }
  }

  return NULL;
}

void
flatpak_run_add_pulseaudio_args (FlatpakBwrap         *bwrap,
                                 FlatpakContextShares  shares)
{
  g_autofree char *pulseaudio_server = flatpak_run_get_pulseaudio_server ();
  g_autofree char *pulseaudio_socket = NULL;
  g_autofree char *pulse_runtime_dir = flatpak_run_get_pulse_runtime_dir ();
  gboolean remote = FALSE;

  if (pulseaudio_server)
    pulseaudio_socket = flatpak_run_parse_pulse_server (pulseaudio_server,
                                                        &remote);

  if (pulseaudio_socket == NULL && !remote)
    {
      pulseaudio_socket = g_build_filename (pulse_runtime_dir, "native", NULL);

      if (!g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
        g_clear_pointer (&pulseaudio_socket, g_free);
    }

  if (pulseaudio_socket == NULL && !remote)
    {
      pulseaudio_socket = realpath ("/var/run/pulse/native", NULL);

      if (pulseaudio_socket && !g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
        g_clear_pointer (&pulseaudio_socket, g_free);
    }

  flatpak_bwrap_unset_env (bwrap, "PULSE_SERVER");

  if (remote)
    {
      if ((shares & FLATPAK_CONTEXT_SHARED_NETWORK) == 0)
        {
          g_warning ("Remote PulseAudio server configured.");
          g_warning ("PulseAudio access will require --share=network permission.");
        }

      g_info ("Using remote PulseAudio server \"%s\"", pulseaudio_server);
      flatpak_bwrap_set_env (bwrap, "PULSE_SERVER", pulseaudio_server, TRUE);
    }
  else if (pulseaudio_socket && g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      static const char sandbox_socket_path[] = "/run/flatpak/pulse/native";
      static const char pulse_server[] = "unix:/run/flatpak/pulse/native";
      static const char config_path[] = "/run/flatpak/pulse/config";
      gboolean share_shm = FALSE; /* TODO: When do we add this? */
      g_autofree char *client_config = g_strdup_printf ("enable-shm=%s\n", share_shm ? "yes" : "no");

      /* FIXME - error handling */
      if (!flatpak_bwrap_add_args_data (bwrap, "pulseaudio", client_config, -1, config_path, NULL))
        return;

      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", pulseaudio_socket, sandbox_socket_path,
                              NULL);

      flatpak_bwrap_set_env (bwrap, "PULSE_SERVER", pulse_server, TRUE);
      flatpak_bwrap_set_env (bwrap, "PULSE_CLIENTCONFIG", config_path, TRUE);
      flatpak_bwrap_add_runtime_dir_member (bwrap, "pulse");
    }
  else
    g_info ("Could not find pulseaudio socket");

  /* Also allow ALSA access. This was added in 1.8, and is not ideally named. However,
   * since the practical permission of ALSA and PulseAudio are essentially the same, and
   * since we don't want to add more permissions for something we plan to replace with
   * portals/pipewire going forward we reinterpret pulseaudio to also mean ALSA.
   */
  if (!remote && g_file_test ("/dev/snd", G_FILE_TEST_IS_DIR))
    flatpak_bwrap_add_args (bwrap, "--dev-bind", "/dev/snd", "/dev/snd", NULL);
}
