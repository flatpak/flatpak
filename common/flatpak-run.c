/*
 * Copyright © 2014 Red Hat, Inc
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

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <grp.h>
#include <unistd.h>
#include <gio/gunixfdlist.h>

#ifdef ENABLE_SECCOMP
#include <seccomp.h>
#endif

#ifdef ENABLE_XAUTH
#include <X11/Xauth.h>
#endif

#include <glib/gi18n.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-run.h"
#include "flatpak-proxy.h"
#include "flatpak-utils.h"
#include "flatpak-dir.h"
#include "flatpak-systemd-dbus.h"
#include "flatpak-document-dbus.h"
#include "lib/flatpak-error.h"

#define DEFAULT_SHELL "/bin/sh"

static gboolean
add_dbus_proxy_args (GPtrArray *argv_array,
                     GPtrArray *session_dbus_proxy_argv,
                     gboolean   enable_session_logging,
                     GPtrArray *system_dbus_proxy_argv,
                     gboolean   enable_system_logging,
                     GPtrArray *a11y_dbus_proxy_argv,
                     gboolean   enable_a11y_logging,
                     int        sync_fds[2],
                     const char *app_info_path,
                     GError   **error);

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

#ifdef ENABLE_XAUTH
static gboolean
auth_streq (char *str,
            char *au_str,
            int   au_len)
{
  return au_len == strlen (str) && memcmp (str, au_str, au_len) == 0;
}

static gboolean
xauth_entry_should_propagate (Xauth *xa,
                              char  *hostname,
                              char  *number)
{
  /* ensure entry isn't for remote access */
  if (xa->family != FamilyLocal && xa->family != FamilyWild)
    return FALSE;

  /* ensure entry is for this machine */
  if (xa->family == FamilyLocal && !auth_streq (hostname, xa->address, xa->address_length))
    return FALSE;

  /* ensure entry is for this session */
  if (xa->number != NULL && !auth_streq (number, xa->number, xa->number_length))
    return FALSE;

  return TRUE;
}

static void
write_xauth (char *number, FILE *output)
{
  Xauth *xa, local_xa;
  char *filename;
  FILE *f;
  struct utsname unames;

  if (uname (&unames))
    {
      g_warning ("uname failed");
      return;
    }

  filename = XauFileName ();
  f = fopen (filename, "rb");
  if (f == NULL)
    return;

  while (TRUE)
    {
      xa = XauReadAuth (f);
      if (xa == NULL)
        break;
      if (xauth_entry_should_propagate (xa, unames.nodename, number))
        {
          local_xa = *xa;
          if (local_xa.number)
            {
              local_xa.number = "99";
              local_xa.number_length = 2;
            }

          if (!XauWriteAuth (output, &local_xa))
            g_warning ("xauth write error");
        }

      XauDisposeAuth (xa);
    }

  fclose (f);
}
#endif /* ENABLE_XAUTH */

static void
add_args (GPtrArray *argv_array, ...)
{
  va_list args;
  const gchar *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, const gchar *)))
    g_ptr_array_add (argv_array, g_strdup (arg));
  va_end (args);
}

static void
add_args_data_fd (GPtrArray *argv_array,
                  GArray    *fd_array,
                  const char *op,
                  int fd,
                  const char *path_optional)
{
  g_autofree char *fd_str = g_strdup_printf ("%d", fd);
  if (fd_array)
    g_array_append_val (fd_array, fd);

  add_args (argv_array,
            op, fd_str, path_optional,
            NULL);
}

static void
flatpak_run_add_x11_args (FlatpakBwrap *bwrap,
                          gboolean allowed)
{
  g_autofree char *x11_socket = NULL;
  const char *display;

  /* Always cover /tmp/.X11-unix, that way we never see the host one in case
   * we have access to the host /tmp. If you request X access we'll put the right
   * thing in this anyway.
   */
  flatpak_bwrap_add_args (bwrap,
                          "--tmpfs", "/tmp/.X11-unix",
                          NULL);

  if (!allowed)
    {
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
      return;
    }

  g_debug ("Allowing x11 access");

  display = g_getenv ("DISPLAY");
  if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
    {
      const char *display_nr = &display[1];
      const char *display_nr_end = display_nr;
      g_autofree char *d = NULL;

      while (g_ascii_isdigit (*display_nr_end))
        display_nr_end++;

      d = g_strndup (display_nr, display_nr_end - display_nr);
      x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

      flatpak_bwrap_add_args (bwrap,
                              "--bind", x11_socket, "/tmp/.X11-unix/X99",
                              NULL);
      flatpak_bwrap_set_env (bwrap, "DISPLAY", ":99.0", TRUE);

#ifdef ENABLE_XAUTH
      g_auto(GLnxTmpfile) xauth_tmpf  = { 0, };

      if (glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &xauth_tmpf, NULL))
        {
          FILE *output = fdopen (xauth_tmpf.fd, "wb");
          if (output != NULL)
            {
              /* fd is now owned by output, steal it from the tmpfile */
              int tmp_fd = dup (glnx_steal_fd (&xauth_tmpf.fd));
              if (tmp_fd != -1)
                {
                  g_autofree char *dest = g_strdup_printf ("/run/user/%d/Xauthority", getuid ());

                  write_xauth (d, output);
                  flatpak_bwrap_add_args_data_fd (bwrap, "--bind-data", tmp_fd, dest);

                  flatpak_bwrap_set_env (bwrap, "XAUTHORITY", dest, TRUE);
                }

              fclose (output);

              if (tmp_fd != -1)
                lseek (tmp_fd, 0, SEEK_SET);
            }
        }
#endif
    }
  else
    {
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
    }

}

static void
flatpak_run_add_wayland_args (FlatpakBwrap *bwrap)
{
  const char *wayland_display;
  g_autofree char *wayland_socket = NULL;
  g_autofree char *sandbox_wayland_socket = NULL;

  wayland_display = g_getenv ("WAYLAND_DISPLAY");
  if (!wayland_display)
    wayland_display = "wayland-0";

  wayland_socket = g_build_filename (g_get_user_runtime_dir (), wayland_display, NULL);
  sandbox_wayland_socket = g_strdup_printf ("/run/user/%d/%s", getuid (), wayland_display);

  if (g_file_test (wayland_socket, G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--bind", wayland_socket, sandbox_wayland_socket,
                              NULL);
    }
}

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
      g_debug ("Pulseaudio user configuration file '%s': %s", path, my_error->message);
      return NULL;
    }

  data_stream = g_data_input_stream_new (G_INPUT_STREAM(input_stream));

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
          g_autofree char *rec_path = g_strdup (line+9);
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
                  g_debug("Found pulseaudio socket from configuration file '%s': %s", path, tokens[1]);
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
  const char* pulse_clientconfig;
  char *pulse_server;
  g_autofree char *pulse_user_config = NULL;

  pulse_server = g_strdup (g_getenv ("PULSE_SERVER"));
  if (pulse_server)
    return pulse_server;

  pulse_clientconfig = g_getenv ("PULSE_CLIENTCONFIG");
  if (pulse_clientconfig)
    return flatpak_run_get_pulseaudio_server_user_config (pulse_clientconfig);

  pulse_user_config = g_build_filename (g_get_user_config_dir(), "pulse/client.conf", NULL);
  pulse_server = flatpak_run_get_pulseaudio_server_user_config (pulse_user_config);
  if (pulse_server)
    return pulse_server;

  pulse_server = flatpak_run_get_pulseaudio_server_user_config ("/etc/pulse/client.conf");
  if (pulse_server)
    return pulse_server;

  return NULL;
}

static char *
flatpak_run_parse_pulse_server (const char *value)
{
  g_auto(GStrv) servers = g_strsplit (value, " ", 0);
  gsize i;

  for (i = 0; servers[i] != NULL; i++)
    {
      const char *server = servers[i];
      if (g_str_has_prefix (server, "{"))
        {
          const char* closing = strstr (server, "}");
          if (closing == NULL)
            continue;
          server = closing + 1;
        }
      if (g_str_has_prefix (server, "unix:"))
        return g_strdup (server+5);
    }

  return NULL;
}

static void
flatpak_run_add_pulseaudio_args (FlatpakBwrap *bwrap)
{
  g_autofree char *pulseaudio_server = flatpak_run_get_pulseaudio_server ();
  g_autofree char *pulseaudio_socket = NULL;

  if (pulseaudio_server)
    pulseaudio_socket = flatpak_run_parse_pulse_server (pulseaudio_server);

  if (!pulseaudio_socket)
    pulseaudio_socket = g_build_filename (g_get_user_runtime_dir (), "pulse/native", NULL);

  flatpak_bwrap_unset_env (bwrap, "PULSE_SERVER");

  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      gboolean share_shm = FALSE; /* TODO: When do we add this? */
      g_autofree char *client_config = g_strdup_printf ("enable-shm=%s\n", share_shm ? "yes" : "no");
      g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/pulse/native", getuid ());
      g_autofree char *pulse_server = g_strdup_printf ("unix:/run/user/%d/pulse/native", getuid ());
      g_autofree char *config_path = g_strdup_printf ("/run/user/%d/pulse/config", getuid ());

      /* FIXME - error handling */
      if (!flatpak_bwrap_add_args_data (bwrap, "pulseaudio", client_config, -1, config_path, NULL))
        return;

      flatpak_bwrap_add_args (bwrap,
                              "--bind", pulseaudio_socket, sandbox_socket_path,
                              NULL);

      flatpak_bwrap_set_env (bwrap, "PULSE_SERVER", pulse_server, TRUE);
      flatpak_bwrap_set_env (bwrap, "PULSE_CLIENTCONFIG", config_path, TRUE);
    }
  else
    g_debug ("Could not find pulseaudio socket");
}

static void
flatpak_run_add_journal_args (FlatpakBwrap *bwrap)
{
  g_autofree char *journal_socket_socket = g_strdup ("/run/systemd/journal/socket");
  g_autofree char *journal_stdout_socket = g_strdup ("/run/systemd/journal/stdout");

  if (g_file_test (journal_socket_socket, G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--bind", journal_socket_socket, journal_socket_socket,
                              NULL);
    }
  if (g_file_test (journal_stdout_socket, G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--bind", journal_stdout_socket, journal_stdout_socket,
                              NULL);
    }
}

static char *
create_proxy_socket (char *template)
{
  g_autofree char *proxy_socket_dir = g_build_filename (g_get_user_runtime_dir (), ".dbus-proxy", NULL);
  g_autofree char *proxy_socket = g_build_filename (proxy_socket_dir, template, NULL);
  int fd;

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, proxy_socket_dir, 0755, NULL, NULL))
    return NULL;

  fd = g_mkstemp (proxy_socket);
  if (fd == -1)
    return NULL;

  close (fd);

  return g_steal_pointer (&proxy_socket);
}

static gboolean
flatpak_run_add_system_dbus_args (FlatpakContext *context,
                                  FlatpakBwrap   *bwrap,
                                  GPtrArray      *dbus_proxy_argv,
                                  gboolean        unrestricted)
{
  const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");
  g_autofree char *real_dbus_address = NULL;
  g_autofree char *dbus_system_socket = NULL;

  if (dbus_address != NULL)
    dbus_system_socket = extract_unix_path_from_dbus_address (dbus_address);
  else if (g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");

  if (dbus_system_socket != NULL && unrestricted)
    {
      flatpak_bwrap_add_args (bwrap,
                              "--bind", dbus_system_socket, "/run/dbus/system_bus_socket",
                              NULL);
      flatpak_bwrap_set_env (bwrap, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  else if (dbus_proxy_argv &&
           flatpak_context_get_needs_system_bus_proxy (context))
    {
      g_autofree char *proxy_socket = create_proxy_socket ("system-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      if (dbus_address)
        real_dbus_address = g_strdup (dbus_address);
      else
        real_dbus_address = g_strdup_printf ("unix:path=%s", dbus_system_socket);

      g_ptr_array_add (dbus_proxy_argv, g_strdup (real_dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));


      flatpak_bwrap_add_args (bwrap,
                              "--bind", proxy_socket, "/run/dbus/system_bus_socket",
                              NULL);
      flatpak_bwrap_set_env (bwrap, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  return FALSE;
}

static gboolean
flatpak_run_add_session_dbus_args (FlatpakBwrap *bwrap,
                                   GPtrArray *dbus_proxy_argv,
                                   gboolean   unrestricted)
{
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  char *dbus_session_socket = NULL;
  g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/bus", getuid ());
  g_autofree char *sandbox_dbus_address = g_strdup_printf ("unix:path=/run/user/%d/bus", getuid ());

  if (dbus_address == NULL)
    return FALSE;

  dbus_session_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_session_socket != NULL && unrestricted)
    {
      flatpak_bwrap_add_args (bwrap,
                              "--bind", dbus_session_socket, sandbox_socket_path,
                              NULL);
      flatpak_bwrap_set_env (bwrap, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }
  else if (dbus_proxy_argv && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("session-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      g_ptr_array_add (dbus_proxy_argv, g_strdup (dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));

      flatpak_bwrap_add_args (bwrap,
                              "--bind", proxy_socket, sandbox_socket_path,
                              NULL);
      flatpak_bwrap_set_env (bwrap, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }

  return FALSE;
}

static int
flatpak_extension_compare_by_path (gconstpointer  _a,
                                   gconstpointer  _b)
{
  const FlatpakExtension *a = _a;
  const FlatpakExtension *b = _b;

  return g_strcmp0 (a->directory, b->directory);
}

gboolean
flatpak_run_add_extension_args (FlatpakBwrap   *bwrap,
                                GKeyFile     *metakey,
                                const char   *full_ref,
                                gboolean      use_ld_so_cache,
                                char        **extensions_out,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_auto(GStrv) parts = NULL;
  g_autoptr(GString) used_extensions = g_string_new ("");
  gboolean is_app;
  GList *extensions, *path_sorted_extensions, *l;
  g_autoptr(GString) ld_library_path = g_string_new ("");
  int count = 0;
  g_autoptr(GHashTable) mounted_tmpfs =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) created_symlink =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    return flatpak_fail (error, "Failed to determine parts from ref: %s", full_ref);

  is_app = strcmp (parts[0], "app") == 0;

  extensions = flatpak_list_extensions (metakey,
                                        parts[2], parts[3]);

  /* First we apply all the bindings, they are sorted alphabetically in order for parent directory
     to be mounted before child directories */
  path_sorted_extensions = g_list_copy (extensions);
  path_sorted_extensions = g_list_sort (path_sorted_extensions, flatpak_extension_compare_by_path);

  for (l = path_sorted_extensions; l != NULL; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autofree char *directory = g_build_filename (is_app ? "/app" : "/usr", ext->directory, NULL);
      g_autofree char *full_directory = g_build_filename (directory, ext->subdir_suffix, NULL);
      g_autofree char *ref = g_build_filename (full_directory, ".ref", NULL);
      g_autofree char *real_ref = g_build_filename (ext->files_path, ext->directory, ".ref", NULL);

      if (ext->needs_tmpfs)
        {
          g_autofree char *parent = g_path_get_dirname (directory);

          if (g_hash_table_lookup (mounted_tmpfs, parent) == NULL)
            {
              flatpak_bwrap_add_args (bwrap,
                                      "--tmpfs", parent,
                                      NULL);
              g_hash_table_insert (mounted_tmpfs, g_steal_pointer (&parent), "mounted");
            }
        }

      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", ext->files_path, full_directory,
                              NULL);

      if (g_file_test (real_ref, G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--lock-file", ref,
                                NULL);
    }

  g_list_free (path_sorted_extensions);

  /* Then apply library directories and file merging, in extension prio order */

  for (l = extensions; l != NULL; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autofree char *directory = g_build_filename (is_app ? "/app" : "/usr", ext->directory, NULL);
      g_autofree char *full_directory = g_build_filename (directory, ext->subdir_suffix, NULL);
      int i;

      if (used_extensions->len > 0)
        g_string_append (used_extensions, ";");
      g_string_append (used_extensions, ext->installed_id);
      g_string_append (used_extensions, "=");
      if (ext->commit != NULL)
        g_string_append (used_extensions, ext->commit);
      else
        g_string_append (used_extensions, "local");

      if (ext->add_ld_path)
        {
          g_autofree char *ld_path = g_build_filename (full_directory, ext->add_ld_path, NULL);

          if (use_ld_so_cache)
            {
              g_autofree char *contents = g_strconcat (ld_path, "\n", NULL);
              /* We prepend app or runtime and a counter in order to get the include order correct for the conf files */
              g_autofree char *ld_so_conf_file = g_strdup_printf ("%s-%03d-%s.conf", parts[0], ++count, ext->installed_id);
              g_autofree char *ld_so_conf_file_path = g_build_filename ("/run/flatpak/ld.so.conf.d", ld_so_conf_file, NULL);

              if (!flatpak_bwrap_add_args_data (bwrap, "ld-so-conf",
                                                contents, -1, ld_so_conf_file_path, error))
                return FALSE;
            }
          else
            {
              if (ld_library_path->len != 0)
                g_string_append (ld_library_path, ":");
              g_string_append (ld_library_path, ld_path);
            }
        }

      for (i = 0; ext->merge_dirs != NULL && ext->merge_dirs[i] != NULL; i++)
        {
          g_autofree char *parent = g_path_get_dirname (directory);
          g_autofree char *merge_dir = g_build_filename (parent, ext->merge_dirs[i], NULL);
          g_autofree char *source_dir = g_build_filename (ext->files_path, ext->merge_dirs[i], NULL);
          g_auto(GLnxDirFdIterator) source_iter = { 0 };
          struct dirent *dent;

          if (glnx_dirfd_iterator_init_at (AT_FDCWD, source_dir, TRUE, &source_iter, NULL))
            {
              while (glnx_dirfd_iterator_next_dent (&source_iter, &dent, NULL, NULL) && dent != NULL)
                {
                  g_autofree char *symlink_path = g_build_filename (merge_dir, dent->d_name, NULL);
                  /* Only create the first, because extensions are listed in prio order */
                  if (g_hash_table_lookup (created_symlink, symlink_path) == NULL)
                    {
                      g_autofree char *symlink = g_build_filename (directory, ext->merge_dirs[i], dent->d_name, NULL);
                      flatpak_bwrap_add_args (bwrap,
                                              "--symlink", symlink, symlink_path,
                                              NULL);
                      g_hash_table_insert (created_symlink, g_steal_pointer (&symlink_path), "created");
                    }
                }
            }
        }
    }

  g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);

  if (ld_library_path->len != 0)
    {
      const gchar *old_ld_path = g_environ_getenv (bwrap->envp, "LD_LIBRARY_PATH");

      if (old_ld_path != NULL && *old_ld_path != 0)
        {
          if (is_app)
            {
              g_string_append (ld_library_path, ":");
              g_string_append (ld_library_path, old_ld_path);
            }
          else
            {
              g_string_prepend (ld_library_path, ":");
              g_string_prepend (ld_library_path, old_ld_path);
            }
        }

      flatpak_bwrap_set_env (bwrap, "LD_LIBRARY_PATH", ld_library_path->str , TRUE);
    }

  if (extensions_out)
    *extensions_out = g_string_free (g_steal_pointer (&used_extensions), FALSE);

  return TRUE;
}

gboolean
flatpak_run_add_environment_args (FlatpakBwrap   *bwrap,
                                  const char     *app_info_path,
                                  FlatpakRunFlags flags,
                                  const char     *app_id,
                                  FlatpakContext *context,
                                  GFile          *app_id_dir,
                                  FlatpakExports **exports_out,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  gboolean unrestricted_session_bus;
  gboolean unrestricted_system_bus;
  g_autoptr(GError) my_error = NULL;
  g_autoptr(FlatpakExports) exports = NULL;
  g_autoptr(GPtrArray) session_bus_proxy_argv = NULL;
  g_autoptr(GPtrArray) system_bus_proxy_argv = NULL;
  g_autoptr(GPtrArray) a11y_bus_proxy_argv = NULL;
  int sync_fds[2] = {-1, -1};

  if ((flags & FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY) == 0)
    session_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
  if ((flags & FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY) == 0)
    system_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);

 if ((context->shares & FLATPAK_CONTEXT_SHARED_IPC) == 0)
    {
      g_debug ("Disallowing ipc access");
      flatpak_bwrap_add_args (bwrap, "--unshare-ipc", NULL);
    }

  if ((context->shares & FLATPAK_CONTEXT_SHARED_NETWORK) == 0)
    {
      g_debug ("Disallowing network access");
      flatpak_bwrap_add_args (bwrap, "--unshare-net", NULL);
    }

  if (context->devices & FLATPAK_CONTEXT_DEVICE_ALL)
    {
      flatpak_bwrap_add_args (bwrap,
                              "--dev-bind", "/dev", "/dev",
                              NULL);
    }
  else
    {
      flatpak_bwrap_add_args (bwrap,
                              "--dev", "/dev",
                              NULL);
      if (context->devices & FLATPAK_CONTEXT_DEVICE_DRI)
        {
          g_debug ("Allowing dri access");
          int i;
          char *dri_devices[] = {
            "/dev/dri",
            /* mali */
            "/dev/mali",
            "/dev/mali0",
            "/dev/umplock",
            /* nvidia */
            "/dev/nvidiactl",
            "/dev/nvidia0",
            "/dev/nvidia-modeset",
          };

          for (i = 0; i < G_N_ELEMENTS(dri_devices); i++)
            {
              if (g_file_test (dri_devices[i], G_FILE_TEST_EXISTS))
                flatpak_bwrap_add_args (bwrap, "--dev-bind", dri_devices[i], dri_devices[i], NULL);
            }
        }

      if (context->devices & FLATPAK_CONTEXT_DEVICE_KVM)
        {
          g_debug ("Allowing kvm access");
          if (g_file_test ("/dev/kvm", G_FILE_TEST_EXISTS))
            flatpak_bwrap_add_args (bwrap, "--dev-bind", "/dev/kvm", "/dev/kvm", NULL);
        }
    }

  flatpak_context_append_bwrap_filesystem (context, bwrap, app_id, app_id_dir, &exports);

  flatpak_run_add_x11_args (bwrap,
                            (context->sockets & FLATPAK_CONTEXT_SOCKET_X11) != 0);

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_WAYLAND)
    {
      g_debug ("Allowing wayland access");
      flatpak_run_add_wayland_args (bwrap);
    }

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_PULSEAUDIO)
    {
      g_debug ("Allowing pulseaudio access");
      flatpak_run_add_pulseaudio_args (bwrap);
    }

  unrestricted_session_bus = (context->sockets & FLATPAK_CONTEXT_SOCKET_SESSION_BUS) != 0;
  if (unrestricted_session_bus)
    g_debug ("Allowing session-dbus access");
  if (flatpak_run_add_session_dbus_args (bwrap, session_bus_proxy_argv, unrestricted_session_bus) &&
      !unrestricted_session_bus && session_bus_proxy_argv)
    flatpak_context_add_bus_filters (context, app_id, TRUE, session_bus_proxy_argv);

  unrestricted_system_bus = (context->sockets & FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS) != 0;
  if (unrestricted_system_bus)
    g_debug ("Allowing system-dbus access");
  if (flatpak_run_add_system_dbus_args (context, bwrap, system_bus_proxy_argv,
                                        unrestricted_system_bus) &&
      !unrestricted_system_bus && system_bus_proxy_argv)
    flatpak_context_add_bus_filters (context, app_id, FALSE, system_bus_proxy_argv);

  if ((flags & FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY) == 0)
    {
      g_autoptr(GDBusConnection) session_bus = NULL;
      g_autofree char *a11y_address = NULL;

      session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
      if (session_bus)
        {
          g_autoptr(GError) local_error = NULL;
          g_autoptr(GDBusMessage) reply = NULL;
          g_autoptr(GDBusMessage) msg =
            g_dbus_message_new_method_call ("org.a11y.Bus",
                                            "/org/a11y/bus",
                                            "org.a11y.Bus",
                                            "GetAddress");
          g_dbus_message_set_body (msg, g_variant_new ("()"));
          reply =
            g_dbus_connection_send_message_with_reply_sync (session_bus, msg,
                                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                            30000,
                                                            NULL,
                                                            NULL,
                                                            NULL);
          if (reply)
            {
              if (g_dbus_message_to_gerror (reply, &local_error))
                {
                  if (!g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
                    g_message ("Can't find a11y bus: %s", local_error->message);
                }
              else
                {
                  g_variant_get (g_dbus_message_get_body (reply),
                                 "(s)", &a11y_address);
                }
            }
        }

      if (a11y_address)
        {
          g_autofree char *proxy_socket = create_proxy_socket ("a11y-bus-proxy-XXXXXX");
          if (proxy_socket)
            {
              g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/at-spi-bus", getuid ());
              g_autofree char *sandbox_dbus_address = g_strdup_printf ("unix:path=/run/user/%d/at-spi-bus", getuid ());

              a11y_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);

              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup (a11y_address));
              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup (proxy_socket));
              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup ("--filter"));
              g_ptr_array_add (a11y_bus_proxy_argv, g_strdup ("--sloppy-names"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.Socket.Embed@/org/a11y/atspi/accessible/root"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.Socket.Unembed@/org/a11y/atspi/accessible/root"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.Registry.GetRegisteredEvents@/org/a11y/atspi/registry"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.GetKeystrokeListeners@/org/a11y/atspi/registry/deviceeventcontroller"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.GetDeviceEventListeners@/org/a11y/atspi/registry/deviceeventcontroller"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.NotifyListenersSync@/org/a11y/atspi/registry/deviceeventcontroller"));
              g_ptr_array_add (a11y_bus_proxy_argv,
                               g_strdup ("--filter=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.NotifyListenersAsync@/org/a11y/atspi/registry/deviceeventcontroller"));

              flatpak_bwrap_add_args (bwrap,
                                      "--bind", proxy_socket, sandbox_socket_path,
                                      NULL);
              flatpak_bwrap_set_env (bwrap, "AT_SPI_BUS_ADDRESS", sandbox_dbus_address, TRUE);
            }
        }
    }

  if (g_environ_getenv (bwrap->envp, "LD_LIBRARY_PATH") != NULL)
    {
      /* LD_LIBRARY_PATH is overridden for setuid helper, so pass it as cmdline arg */
      flatpak_bwrap_add_args (bwrap,
                              "--setenv", "LD_LIBRARY_PATH", g_environ_getenv (bwrap->envp, "LD_LIBRARY_PATH"),
                              NULL);
      flatpak_bwrap_unset_env (bwrap, "LD_LIBRARY_PATH");
    }

  /* Must run this before spawning the dbus proxy, to ensure it
     ends up in the app cgroup */
  if (!flatpak_run_in_transient_unit (app_id, &my_error))
    {
      /* We still run along even if we don't get a cgroup, as nothing
         really depends on it. Its just nice to have */
      g_debug ("Failed to run in transient scope: %s", my_error->message);
      g_clear_error (&my_error);
    }

  if (!add_dbus_proxy_args (bwrap->argv,
                            session_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_SESSION_BUS) != 0,
                            system_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS) != 0,
                            a11y_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_A11Y_BUS) != 0,
                            sync_fds, app_info_path, error))
    return FALSE;

  if (sync_fds[1] != -1)
    close (sync_fds[1]);

  if (exports_out)
    *exports_out = g_steal_pointer (&exports);

  return TRUE;
}

typedef struct {
  const char *env;
  const char *val;
} ExportData;

static const ExportData default_exports[] = {
  {"PATH", "/app/bin:/usr/bin"},
  /* We always want to unset LD_LIBRARY_PATH to avoid inheriting weird
   * dependencies from the host. But if not using ld.so.cache this is
   * later set. */
  {"LD_LIBRARY_PATH", NULL},
  {"XDG_CONFIG_DIRS", "/app/etc/xdg:/etc/xdg"},
  {"XDG_DATA_DIRS", "/app/share:/usr/share"},
  {"SHELL", "/bin/sh"},
  {"TMPDIR", NULL}, /* Unset TMPDIR as it may not exist in the sandbox */

  /* Some env vars are common enough and will affect the sandbox badly
     if set on the host. We clear these always. */
  {"PYTHONPATH", NULL},
  {"PERLLIB", NULL},
  {"PERL5LIB", NULL},
  {"XCURSOR_PATH", NULL},
};

static const ExportData no_ld_so_cache_exports[] = {
  {"LD_LIBRARY_PATH", "/app/lib"},
};

static const ExportData devel_exports[] = {
  {"ACLOCAL_PATH", "/app/share/aclocal"},
  {"C_INCLUDE_PATH", "/app/include"},
  {"CPLUS_INCLUDE_PATH", "/app/include"},
  {"LDFLAGS", "-L/app/lib "},
  {"PKG_CONFIG_PATH", "/app/lib/pkgconfig:/app/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"},
  {"LC_ALL", "en_US.utf8"},
};

static void
add_exports (GPtrArray *env_array,
             const ExportData *exports,
             gsize n_exports)
{
  int i;

  for (i = 0; i < n_exports; i++)
    {
      if (exports[i].val)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", exports[i].env, exports[i].val));
    }
}

char **
flatpak_run_get_minimal_env (gboolean devel, gboolean use_ld_so_cache)
{
  GPtrArray *env_array;
  static const char * const copy[] = {
    "PWD",
    "GDMSESSION",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "DESKTOP_SESSION",
    "EMAIL_ADDRESS",
    "HOME",
    "HOSTNAME",
    "LOGNAME",
    "REAL_NAME",
    "TERM",
    "USER",
    "USERNAME",
  };
  static const char * const copy_nodevel[] = {
    "LANG",
    "LANGUAGE",
    "LC_ALL",
    "LC_ADDRESS",
    "LC_COLLATE",
    "LC_CTYPE",
    "LC_IDENTIFICATION",
    "LC_MEASUREMENT",
    "LC_MESSAGES",
    "LC_MONETARY",
    "LC_NAME",
    "LC_NUMERIC",
    "LC_PAPER",
    "LC_TELEPHONE",
    "LC_TIME",
  };
  int i;

  env_array = g_ptr_array_new_with_free_func (g_free);

  add_exports (env_array, default_exports, G_N_ELEMENTS (default_exports));

  if (!use_ld_so_cache)
    add_exports (env_array, no_ld_so_cache_exports, G_N_ELEMENTS (no_ld_so_cache_exports));

  if (devel)
    add_exports (env_array, devel_exports, G_N_ELEMENTS (devel_exports));

  for (i = 0; i < G_N_ELEMENTS (copy); i++)
    {
      const char *current = g_getenv (copy[i]);
      if (current)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy[i], current));
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS (copy_nodevel); i++)
        {
          const char *current = g_getenv (copy_nodevel[i]);
          if (current)
            g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy_nodevel[i], current));
        }
    }

  g_ptr_array_add (env_array, NULL);
  return (char **) g_ptr_array_free (env_array, FALSE);
}

static char **
apply_exports (char **envp,
               const ExportData *exports,
               gsize n_exports)
{
  int i;

  for (i = 0; i < n_exports; i++)
    {
      const char *value = exports[i].val;

      if (value)
        envp = g_environ_setenv (envp, exports[i].env, value, TRUE);
      else
        envp = g_environ_unsetenv (envp, exports[i].env);
    }

  return envp;
}

void
flatpak_run_apply_env_default (FlatpakBwrap *bwrap, gboolean use_ld_so_cache)
{
  bwrap->envp = apply_exports (bwrap->envp, default_exports, G_N_ELEMENTS (default_exports));

  if (!use_ld_so_cache)
    bwrap->envp = apply_exports (bwrap->envp, no_ld_so_cache_exports, G_N_ELEMENTS (no_ld_so_cache_exports));
}

void
flatpak_run_apply_env_appid (FlatpakBwrap *bwrap,
                             GFile *app_dir)
{
  g_autoptr(GFile) app_dir_data = NULL;
  g_autoptr(GFile) app_dir_config = NULL;
  g_autoptr(GFile) app_dir_cache = NULL;

  app_dir_data = g_file_get_child (app_dir, "data");
  app_dir_config = g_file_get_child (app_dir, "config");
  app_dir_cache = g_file_get_child (app_dir, "cache");
  flatpak_bwrap_set_env (bwrap, "XDG_DATA_HOME", flatpak_file_get_path_cached (app_dir_data), TRUE);
  flatpak_bwrap_set_env (bwrap, "XDG_CONFIG_HOME", flatpak_file_get_path_cached (app_dir_config), TRUE);
  flatpak_bwrap_set_env (bwrap, "XDG_CACHE_HOME", flatpak_file_get_path_cached (app_dir_cache), TRUE);
}

void
flatpak_run_apply_env_vars (FlatpakBwrap *bwrap, FlatpakContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *var = key;
      const char *val = value;

      if (val && val[0] != 0)
        flatpak_bwrap_set_env (bwrap, var, val, TRUE);
      else
        flatpak_bwrap_unset_env (bwrap, var);
    }
}

GFile *
flatpak_get_data_dir (const char *app_id)
{
  g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) var_app = g_file_resolve_relative_path (home, ".var/app");

  return g_file_get_child (var_app, app_id);
}

GFile *
flatpak_ensure_data_dir (const char   *app_id,
                         GCancellable *cancellable,
                         GError      **error)
{
  g_autoptr(GFile) dir = flatpak_get_data_dir (app_id);
  g_autoptr(GFile) data_dir = g_file_get_child (dir, "data");
  g_autoptr(GFile) cache_dir = g_file_get_child (dir, "cache");
  g_autoptr(GFile) fontconfig_cache_dir = g_file_get_child (cache_dir, "fontconfig");
  g_autoptr(GFile) tmp_dir = g_file_get_child (cache_dir, "tmp");
  g_autoptr(GFile) config_dir = g_file_get_child (dir, "config");

  if (!flatpak_mkdir_p (data_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (cache_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (fontconfig_cache_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (tmp_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (config_dir, cancellable, error))
    return NULL;

  return g_object_ref (dir);
}

struct JobData
{
  char      *job;
  GMainLoop *main_loop;
};

static void
job_removed_cb (SystemdManager *manager,
                guint32         id,
                char           *job,
                char           *unit,
                char           *result,
                struct JobData *data)
{
  if (strcmp (job, data->job) == 0)
    g_main_loop_quit (data->main_loop);
}

gboolean
flatpak_run_in_transient_unit (const char *appid, GError **error)
{
  g_autoptr(GDBusConnection) conn = NULL;
  g_autofree char *path = NULL;
  g_autofree char *address = NULL;
  g_autofree char *name = NULL;
  g_autofree char *job = NULL;
  SystemdManager *manager = NULL;
  GVariantBuilder builder;
  GVariant *properties = NULL;
  GVariant *aux = NULL;
  guint32 pid;
  GMainContext *main_context = NULL;
  GMainLoop *main_loop = NULL;
  struct JobData data;
  gboolean res = FALSE;

  path = g_strdup_printf ("/run/user/%d/systemd/private", getuid ());

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return flatpak_fail (error,
                         "No systemd user session available, cgroups not available");

  main_context = g_main_context_new ();
  main_loop = g_main_loop_new (main_context, FALSE);

  g_main_context_push_thread_default (main_context);

  address = g_strconcat ("unix:path=", path, NULL);

  conn = g_dbus_connection_new_for_address_sync (address,
                                                 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                 NULL,
                                                 NULL, error);
  if (!conn)
    goto out;

  manager = systemd_manager_proxy_new_sync (conn,
                                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                            NULL,
                                            "/org/freedesktop/systemd1",
                                            NULL, error);
  if (!manager)
    goto out;

  name = g_strdup_printf ("flatpak-%s-%d.scope", appid, getpid ());

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sv)"));

  pid = getpid ();
  g_variant_builder_add (&builder, "(sv)",
                         "PIDs",
                         g_variant_new_fixed_array (G_VARIANT_TYPE ("u"),
                                                    &pid, 1, sizeof (guint32))
                        );

  properties = g_variant_builder_end (&builder);

  aux = g_variant_new_array (G_VARIANT_TYPE ("(sa(sv))"), NULL, 0);

  if (!systemd_manager_call_start_transient_unit_sync (manager,
                                                       name,
                                                       "fail",
                                                       properties,
                                                       aux,
                                                       &job,
                                                       NULL,
                                                       error))
    goto out;

  data.job = job;
  data.main_loop = main_loop;
  g_signal_connect (manager, "job-removed", G_CALLBACK (job_removed_cb), &data);

  g_main_loop_run (main_loop);

  res = TRUE;

out:
  if (main_context)
    {
      g_main_context_pop_thread_default (main_context);
      g_main_context_unref (main_context);
    }
  if (main_loop)
    g_main_loop_unref (main_loop);
  if (manager)
    g_object_unref (manager);

  return res;
}

static void
add_font_path_args (FlatpakBwrap *bwrap)
{
  g_autoptr(GFile) home = NULL;
  g_autoptr(GFile) user_font1 = NULL;
  g_autoptr(GFile) user_font2 = NULL;
  g_autoptr(GFile) user_font_cache = NULL;
  g_auto(GStrv) system_cache_dirs = NULL;
  gboolean found_cache = FALSE;
  int i;

  if (g_file_test (SYSTEM_FONTS_DIR, G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", SYSTEM_FONTS_DIR, "/run/host/fonts",
                              NULL);
    }

  system_cache_dirs = g_strsplit (SYSTEM_FONT_CACHE_DIRS, ":", 0);
  for (i = 0; system_cache_dirs[i] != NULL; i++)
    {
      if (g_file_test (system_cache_dirs[i], G_FILE_TEST_EXISTS))
        {
          flatpak_bwrap_add_args (bwrap,
                                  "--ro-bind", system_cache_dirs[i], "/run/host/fonts-cache",
                                  NULL);
          found_cache = TRUE;
          break;
        }
    }

  if (!found_cache)
    {
      /* We ensure these directories are never writable, or fontconfig
         will use them to write the default cache */
      flatpak_bwrap_add_args (bwrap,
                              "--tmpfs", "/run/host/fonts-cache",
                              "--remount-ro", "/run/host/fonts-cache",
                              NULL);
    }

  home = g_file_new_for_path (g_get_home_dir ());
  user_font1 = g_file_resolve_relative_path (home, ".local/share/fonts");
  user_font2 = g_file_resolve_relative_path (home, ".fonts");

  if (g_file_query_exists (user_font1, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", flatpak_file_get_path_cached (user_font1), "/run/host/user-fonts",
                              NULL);
    }
  else if (g_file_query_exists (user_font2, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", flatpak_file_get_path_cached (user_font2), "/run/host/user-fonts",
                              NULL);
    }

  user_font_cache = g_file_resolve_relative_path (home, ".cache/fontconfig");
  if (g_file_query_exists (user_font_cache, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", flatpak_file_get_path_cached (user_font_cache), "/run/host/user-fonts-cache",
                              NULL);
    }
  else
    {
      /* We ensure these directories are never writable, or fontconfig
         will use them to write the default cache */
      flatpak_bwrap_add_args (bwrap,
                              "--tmpfs", "/run/host/user-fonts-cache",
                              "--remount-ro", "/run/host/user-fonts-cache",
                              NULL);
    }
}

static void
add_icon_path_args (FlatpakBwrap *bwrap)
{
  if (g_file_test ("/usr/share/icons", G_FILE_TEST_IS_DIR))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", "/usr/share/icons", "/run/host/share/icons",
                              NULL);
    }
}

static void
add_default_permissions (FlatpakContext *app_context)
{
  flatpak_context_set_session_bus_policy (app_context,
                                          "org.freedesktop.portal.*",
                                          FLATPAK_POLICY_TALK);
}

FlatpakContext *
flatpak_app_compute_permissions (GKeyFile *app_metadata,
                                 GKeyFile *runtime_metadata,
                                 GError  **error)
{
  g_autoptr(FlatpakContext) app_context = NULL;

  app_context = flatpak_context_new ();

  add_default_permissions (app_context);

  if (runtime_metadata != NULL &&
      !flatpak_context_load_metadata (app_context, runtime_metadata, error))
    return NULL;

  if (app_metadata != NULL &&
      !flatpak_context_load_metadata (app_context, app_metadata, error))
    return NULL;

  return g_steal_pointer (&app_context);
}

gboolean
flatpak_run_add_app_info_args (FlatpakBwrap   *bwrap,
                               GFile          *app_files,
                               GVariant       *app_deploy_data,
                               const char     *app_extensions,
                               GFile          *runtime_files,
                               GVariant       *runtime_deploy_data,
                               const char     *runtime_extensions,
                               const char     *app_id,
                               const char     *app_branch,
                               const char     *runtime_ref,
                               FlatpakContext *final_app_context,
                               char          **app_info_path_out,
                               GError        **error)
{
  g_autofree char *tmp_path = NULL;
  int fd, fd2;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *runtime_path = NULL;
  g_autofree char *old_dest = g_strdup_printf ("/run/user/%d/flatpak-info", getuid ());
  const char *group;

  fd = g_file_open_tmp ("flatpak-context-XXXXXX", &tmp_path, NULL);
  if (fd < 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open flatpak-info temp file: %s"), g_strerror (errsv));
      return FALSE;
    }

  close (fd);

  keyfile = g_key_file_new ();

  if (app_files)
    group = FLATPAK_METADATA_GROUP_APPLICATION;
  else
    group = FLATPAK_METADATA_GROUP_RUNTIME;

  g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_NAME, app_id);
  g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_RUNTIME,
                         runtime_ref);

  if (app_files)
    {
      g_autofree char *app_path = g_file_get_path (app_files);
      g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                             FLATPAK_METADATA_KEY_APP_PATH, app_path);
    }
  if (app_deploy_data)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_APP_COMMIT, flatpak_deploy_data_get_commit (app_deploy_data));
  if (app_extensions && *app_extensions != 0)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_APP_EXTENSIONS, app_extensions);
  runtime_path = g_file_get_path (runtime_files);
  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_RUNTIME_PATH, runtime_path);
  if (runtime_deploy_data)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_RUNTIME_COMMIT, flatpak_deploy_data_get_commit (runtime_deploy_data));
  if (runtime_extensions && *runtime_extensions != 0)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_RUNTIME_EXTENSIONS, runtime_extensions);
  if (app_branch != NULL)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_BRANCH, app_branch);

  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_FLATPAK_VERSION, PACKAGE_VERSION);

  if ((final_app_context->sockets & FLATPAK_CONTEXT_SOCKET_SESSION_BUS) == 0)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_SESSION_BUS_PROXY, TRUE);

  if ((final_app_context->sockets & FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS) == 0)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_SYSTEM_BUS_PROXY, TRUE);

  flatpak_context_save_metadata (final_app_context, TRUE, keyfile);

  if (!g_key_file_save_to_file (keyfile, tmp_path, error))
    return FALSE;

  /* We want to create a file on /.flatpak-info that the app cannot modify, which
     we do by creating a read-only bind mount. This way one can openat()
     /proc/$pid/root, and if that succeeds use openat via that to find the
     unfakable .flatpak-info file. However, there is a tiny race in that if
     you manage to open /proc/$pid/root, but then the pid dies, then
     every mount but the root is unmounted in the namespace, so the
     .flatpak-info will be empty. We fix this by first creating a real file
     with the real info in, then bind-mounting on top of that, the same info.
     This way even if the bind-mount is unmounted we can find the real data.
  */

  fd = open (tmp_path, O_RDONLY);
  if (fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open temp file: %s"), g_strerror (errsv));
      return FALSE;
    }

  fd2 = open (tmp_path, O_RDONLY);
  if (fd2 == -1)
    {
      close (fd);
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open temp file: %s"), g_strerror (errsv));
      return FALSE;
    }

  unlink (tmp_path);

  flatpak_bwrap_add_args_data_fd (bwrap,
                                  "--file", fd, "/.flatpak-info");
  flatpak_bwrap_add_args_data_fd (bwrap,
                                  "--ro-bind-data", fd2, "/.flatpak-info");
  flatpak_bwrap_add_args (bwrap,
                          "--symlink", "../../../.flatpak-info", old_dest,
                          NULL);

  if (app_info_path_out != NULL)
    *app_info_path_out = g_strdup_printf ("/proc/self/fd/%d", fd);

  return TRUE;
}

static void
add_monitor_path_args (gboolean use_session_helper,
                       FlatpakBwrap *bwrap)
{
  g_autoptr(AutoFlatpakSessionHelper) session_helper = NULL;
  g_autofree char *monitor_path = NULL;

  if (use_session_helper)
    {
      session_helper =
        flatpak_session_helper_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                       "org.freedesktop.Flatpak",
                                                       "/org/freedesktop/Flatpak/SessionHelper",
                                                       NULL, NULL);
    }

  if (session_helper &&
      flatpak_session_helper_call_request_monitor_sync (session_helper,
                                                        &monitor_path,
                                                        NULL, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", monitor_path, "/run/host/monitor",
                              "--symlink", "/run/host/monitor/localtime", "/etc/localtime",
                              "--symlink", "/run/host/monitor/resolv.conf", "/etc/resolv.conf",
                              "--symlink", "/run/host/monitor/host.conf", "/etc/host.conf",
                              "--symlink", "/run/host/monitor/hosts", "/etc/hosts",
                              NULL);
    }
  else
    {
      /* /etc/localtime and /etc/resolv.conf can not exist (or be symlinks to
       * non-existing targets), in which case we don't want to attempt to create
       * bogus symlinks or bind mounts, as that will cause flatpak run to fail.
       */
      if (g_file_test ("/etc/localtime", G_FILE_TEST_EXISTS))
        {
          char localtime[PATH_MAX + 1];
          ssize_t symlink_size;
          gboolean is_reachable = FALSE;

          symlink_size = readlink ("/etc/localtime", localtime, sizeof (localtime) - 1);
          if (symlink_size > 0)
            {
              g_autoptr(GFile) base_file = NULL;
              g_autoptr(GFile) target_file = NULL;
              g_autofree char *target_canonical = NULL;

              /* readlink() does not append a null byte to the buffer. */
              localtime[symlink_size] = 0;

              base_file = g_file_new_for_path ("/etc");
              target_file = g_file_resolve_relative_path (base_file, localtime);
              target_canonical = g_file_get_path (target_file);

              is_reachable = g_str_has_prefix (target_canonical, "/usr/");
            }

          if (is_reachable)
            {
              flatpak_bwrap_add_args (bwrap,
                                      "--symlink", localtime, "/etc/localtime",
                                      NULL);
            }
          else
            {
              flatpak_bwrap_add_args (bwrap,
                                      "--ro-bind", "/etc/localtime", "/etc/localtime",
                                      NULL);
            }
        }

      if (g_file_test ("/etc/resolv.conf", G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--ro-bind", "/etc/resolv.conf", "/etc/resolv.conf",
                                NULL);
      if (g_file_test ("/etc/host.conf", G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--ro-bind", "/etc/host.conf", "/etc/host.conf",
                                NULL);
      if (g_file_test ("/etc/hosts", G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--ro-bind", "/etc/hosts", "/etc/hosts",
                                NULL);
    }
}

static void
add_document_portal_args (FlatpakBwrap *bwrap,
                          const char  *app_id,
                          char       **out_mount_path)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autofree char *doc_mount_path = NULL;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session_bus)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GDBusMessage) reply = NULL;
      g_autoptr(GDBusMessage) msg =
        g_dbus_message_new_method_call ("org.freedesktop.portal.Documents",
                                        "/org/freedesktop/portal/documents",
                                        "org.freedesktop.portal.Documents",
                                        "GetMountPoint");
      g_dbus_message_set_body (msg, g_variant_new ("()"));
      reply =
        g_dbus_connection_send_message_with_reply_sync (session_bus, msg,
                                                        G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                        30000,
                                                        NULL,
                                                        NULL,
                                                        NULL);
      if (reply)
        {
          if (g_dbus_message_to_gerror (reply, &local_error))
            {
              if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
                g_debug ("Document portal not available, not mounting /run/user/%d/doc", getuid ());
              else
                g_message ("Can't get document portal: %s", local_error->message);
            }
          else
            {
              g_autofree char *src_path = NULL;
              g_autofree char *dst_path = NULL;
              g_variant_get (g_dbus_message_get_body (reply),
                             "(^ay)", &doc_mount_path);

              src_path = g_strdup_printf ("%s/by-app/%s",
                                          doc_mount_path, app_id);
              dst_path = g_strdup_printf ("/run/user/%d/doc", getuid ());
              flatpak_bwrap_add_args (bwrap, "--bind", src_path, dst_path, NULL);
            }
        }
    }

  *out_mount_path = g_steal_pointer (&doc_mount_path);
}

static gchar *
join_args (GPtrArray *argv_array, gsize *len_out)
{
  gchar *string;
  gchar *ptr;
  gint i;
  gsize len = 0;

  for (i = 0; i < argv_array->len && argv_array->pdata[i] != NULL; i++)
    len +=  strlen (argv_array->pdata[i]) + 1;

  string = g_new (gchar, len);
  *string = 0;
  ptr = string;
  for (i = 0; i < argv_array->len && argv_array->pdata[i] != NULL; i++)
    ptr = g_stpcpy (ptr, argv_array->pdata[i]) + 1;

  *len_out = len;
  return string;
}

typedef struct {
  int sync_fd;
  int app_info_fd;
  int bwrap_args_fd;
} DbusProxySpawnData;

static void
dbus_spawn_child_setup (gpointer user_data)
{
  DbusProxySpawnData *data = user_data;

  /* Unset CLOEXEC */
  fcntl (data->sync_fd, F_SETFD, 0);
  fcntl (data->app_info_fd, F_SETFD, 0);
  fcntl (data->bwrap_args_fd, F_SETFD, 0);
}

/* This wraps the argv in a bwrap call, primary to allow the
   command to be run with a proper /.flatpak-info with data
   taken from app_info_fd */
static gboolean
prepend_bwrap_argv_wrapper (GPtrArray *argv,
                            int app_info_fd,
                            int *bwrap_fd_out,
                            GError **error)
{
  int i = 0;
  g_auto(GLnxDirFdIterator) dir_iter = { 0 };
  struct dirent *dent;
  g_autoptr(GPtrArray) bwrap_args = g_ptr_array_new_with_free_func (g_free);
  gsize bwrap_args_len;
  g_auto(GLnxTmpfile) args_tmpf  = { 0, };
  g_autofree char *bwrap_args_data = NULL;
  g_autofree char *proxy_socket_dir = g_build_filename (g_get_user_runtime_dir (), ".dbus-proxy/", NULL);

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, "/", FALSE, &dir_iter, error))
    return FALSE;

  while (TRUE)
    {
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dir_iter, &dent, NULL, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (strcmp (dent->d_name, ".flatpak-info") == 0)
        continue;

      if (dent->d_type == DT_DIR)
        {
          if (strcmp (dent->d_name, "tmp") == 0 ||
              strcmp (dent->d_name, "var") == 0 ||
              strcmp (dent->d_name, "run") == 0)
            g_ptr_array_add (bwrap_args, g_strdup ("--bind"));
          else
            g_ptr_array_add (bwrap_args, g_strdup ("--ro-bind"));
          g_ptr_array_add (bwrap_args, g_strconcat ("/", dent->d_name, NULL));
          g_ptr_array_add (bwrap_args, g_strconcat ("/", dent->d_name, NULL));
        }
      else if (dent->d_type == DT_LNK)
        {
          ssize_t symlink_size;
          char path_buffer[PATH_MAX + 1];

          symlink_size = readlinkat (dir_iter.fd, dent->d_name, path_buffer, sizeof (path_buffer) - 1);
          if (symlink_size < 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
          path_buffer[symlink_size] = 0;

          g_ptr_array_add (bwrap_args, g_strdup ("--symlink"));
          g_ptr_array_add (bwrap_args, g_strdup (path_buffer));
          g_ptr_array_add (bwrap_args, g_strconcat ("/", dent->d_name, NULL));
        }
    }

  g_ptr_array_add (bwrap_args, g_strdup ("--bind"));
  g_ptr_array_add (bwrap_args, g_strdup (proxy_socket_dir));
  g_ptr_array_add (bwrap_args, g_strdup (proxy_socket_dir));

  /* This is a file rather than a bind mount, because it will then
     not be unmounted from the namespace when the namespace dies. */
  g_ptr_array_add (bwrap_args, g_strdup ("--file"));
  g_ptr_array_add (bwrap_args, g_strdup_printf ("%d", app_info_fd));
  g_ptr_array_add (bwrap_args, g_strdup ("/.flatpak-info"));
  g_ptr_array_add (bwrap_args, NULL);

  {
    g_autofree char *commandline = flatpak_quote_argv ((const char **) bwrap_args->pdata);
    flatpak_debug2 ("bwrap args '%s'", commandline);
  }

  bwrap_args_data = join_args (bwrap_args, &bwrap_args_len);
  if (!flatpak_buffer_to_sealed_memfd_or_tmpfile (&args_tmpf, "bwrap-args", bwrap_args_data, bwrap_args_len, error))
    return FALSE;

  g_ptr_array_insert (argv, i++, g_strdup (flatpak_get_bwrap ()));
  g_ptr_array_insert (argv, i++, g_strdup ("--args"));
  g_ptr_array_insert (argv, i++, g_strdup_printf ("%d", args_tmpf.fd));

  *bwrap_fd_out = glnx_steal_fd (&args_tmpf.fd);
  return TRUE;
}

static gboolean
has_args (GPtrArray *args)
{
  return args != NULL && args->len > 0;
}

static void
append_proxy_args (GPtrArray *dbus_proxy_argv,
                   GPtrArray *args,
                   gboolean   enable_logging)
{
  if (has_args (args))
    {
      int i;

      for (i = 0; i < args->len; i++)
        g_ptr_array_add (dbus_proxy_argv, g_strdup (args->pdata[i]));

      if (enable_logging)
        g_ptr_array_add (dbus_proxy_argv, g_strdup ("--log"));
    }
}

static gboolean
add_dbus_proxy_args (GPtrArray *argv_array,
                     GPtrArray *session_dbus_proxy_argv,
                     gboolean   enable_session_logging,
                     GPtrArray *system_dbus_proxy_argv,
                     gboolean   enable_system_logging,
                     GPtrArray *a11y_dbus_proxy_argv,
                     gboolean   enable_a11y_logging,
                     int        sync_fds[2],
                     const char *app_info_path,
                     GError   **error)
{
  char x = 'x';
  const char *proxy;
  g_autofree char *commandline = NULL;
  DbusProxySpawnData spawn_data;
  glnx_autofd int app_info_fd = -1;
  glnx_autofd int bwrap_args_fd = -1;
  g_autoptr(GPtrArray) dbus_proxy_argv = NULL;

  if (!has_args (session_dbus_proxy_argv) &&
      !has_args (system_dbus_proxy_argv) &&
      !has_args (a11y_dbus_proxy_argv))
    return TRUE;

  if (sync_fds[0] == -1)
    {
      access ("sync_fds", 0);
      if (pipe (sync_fds) < 0)
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               _("Unable to create sync pipe"));
          return FALSE;
        }

      add_args_data_fd (argv_array, NULL,
                        "--sync-fd", sync_fds[0], NULL);
    }

  proxy = g_getenv ("FLATPAK_DBUSPROXY");
  if (proxy == NULL)
    proxy = DBUSPROXY;

  dbus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy));
  g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--fd=%d", sync_fds[1]));

  append_proxy_args (dbus_proxy_argv, session_dbus_proxy_argv, enable_session_logging);
  append_proxy_args (dbus_proxy_argv, system_dbus_proxy_argv, enable_system_logging);
  append_proxy_args (dbus_proxy_argv, a11y_dbus_proxy_argv, enable_a11y_logging);

  g_ptr_array_add (dbus_proxy_argv, NULL); /* NULL terminate */

  app_info_fd = open (app_info_path, O_RDONLY);
  if (app_info_fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open app info file: %s"), g_strerror (errsv));
      return FALSE;
    }

  if (!prepend_bwrap_argv_wrapper (dbus_proxy_argv, app_info_fd, &bwrap_args_fd, error))
    return FALSE;

  commandline = flatpak_quote_argv ((const char **) dbus_proxy_argv->pdata);
  flatpak_debug2 ("Running '%s'", commandline);

  spawn_data.sync_fd = sync_fds[1];
  spawn_data.app_info_fd = app_info_fd;
  spawn_data.bwrap_args_fd = bwrap_args_fd;
  if (!g_spawn_async (NULL,
                      (char **) dbus_proxy_argv->pdata,
                      NULL,
                      G_SPAWN_SEARCH_PATH,
                      dbus_spawn_child_setup,
                      &spawn_data,
                      NULL, error))
    {
      close (sync_fds[0]);
      close (sync_fds[1]);
      return FALSE;
    }

  /* Sync with proxy, i.e. wait until its listening on the sockets */
  if (read (sync_fds[0], &x, 1) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to sync with dbus proxy"));

      close (sync_fds[0]);
      close (sync_fds[1]);
      return FALSE;
    }

  return TRUE;
}

#ifdef ENABLE_SECCOMP
static const uint32_t seccomp_x86_64_extra_arches[] = { SCMP_ARCH_X86, 0, };

#ifdef SCMP_ARCH_AARCH64
static const uint32_t seccomp_aarch64_extra_arches[] = { SCMP_ARCH_ARM, 0 };
#endif

static inline void
cleanup_seccomp (void *p)
{
  scmp_filter_ctx *pp = (scmp_filter_ctx *) p;

  if (*pp)
    seccomp_release (*pp);
}

static gboolean
setup_seccomp (FlatpakBwrap *bwrap,
               const char *arch,
               gulong      allowed_personality,
               gboolean    multiarch,
               gboolean    devel,
               GError    **error)
{
  __attribute__((cleanup (cleanup_seccomp))) scmp_filter_ctx seccomp = NULL;

  /**** BEGIN NOTE ON CODE SHARING
   *
   * There are today a number of different Linux container
   * implementations.  That will likely continue for long into the
   * future.  But we can still try to share code, and it's important
   * to do so because it affects what library and application writers
   * can do, and we should support code portability between different
   * container tools.
   *
   * This syscall blacklist is copied from linux-user-chroot, which was in turn
   * clearly influenced by the Sandstorm.io blacklist.
   *
   * If you make any changes here, I suggest sending the changes along
   * to other sandbox maintainers.  Using the libseccomp list is also
   * an appropriate venue:
   * https://groups.google.com/forum/#!topic/libseccomp
   *
   * A non-exhaustive list of links to container tooling that might
   * want to share this blacklist:
   *
   *  https://github.com/sandstorm-io/sandstorm
   *    in src/sandstorm/supervisor.c++
   *  http://cgit.freedesktop.org/xdg-app/xdg-app/
   *    in common/flatpak-run.c
   *  https://git.gnome.org/browse/linux-user-chroot
   *    in src/setup-seccomp.c
   *
   **** END NOTE ON CODE SHARING
   */
  struct
  {
    int                  scall;
    struct scmp_arg_cmp *arg;
  } syscall_blacklist[] = {
    /* Block dmesg */
    {SCMP_SYS (syslog)},
    /* Useless old syscall */
    {SCMP_SYS (uselib)},
    /* Don't allow you to switch to bsd emulation or whatnot */
    {SCMP_SYS (personality), &SCMP_A0(SCMP_CMP_NE, allowed_personality)},
    /* Don't allow disabling accounting */
    {SCMP_SYS (acct)},
    /* 16-bit code is unnecessary in the sandbox, and modify_ldt is a
       historic source of interesting information leaks. */
    {SCMP_SYS (modify_ldt)},
    /* Don't allow reading current quota use */
    {SCMP_SYS (quotactl)},

    /* Don't allow access to the kernel keyring */
    {SCMP_SYS (add_key)},
    {SCMP_SYS (keyctl)},
    {SCMP_SYS (request_key)},

    /* Scary VM/NUMA ops */
    {SCMP_SYS (move_pages)},
    {SCMP_SYS (mbind)},
    {SCMP_SYS (get_mempolicy)},
    {SCMP_SYS (set_mempolicy)},
    {SCMP_SYS (migrate_pages)},

    /* Don't allow subnamespace setups: */
    {SCMP_SYS (unshare)},
    {SCMP_SYS (mount)},
    {SCMP_SYS (pivot_root)},
    {SCMP_SYS (clone), &SCMP_A0 (SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)},

    /* Don't allow faking input to the controlling tty (CVE-2017-5226) */
    {SCMP_SYS (ioctl), &SCMP_A1(SCMP_CMP_EQ, (int)TIOCSTI)},
  };

  struct
  {
    int                  scall;
    struct scmp_arg_cmp *arg;
  } syscall_nondevel_blacklist[] = {
    /* Profiling operations; we expect these to be done by tools from outside
     * the sandbox.  In particular perf has been the source of many CVEs.
     */
    {SCMP_SYS (perf_event_open)},
    {SCMP_SYS (ptrace)}
  };
  /* Blacklist all but unix, inet, inet6 and netlink */
  int socket_family_blacklist[] = {
    AF_AX25,
    AF_IPX,
    AF_APPLETALK,
    AF_NETROM,
    AF_BRIDGE,
    AF_ATMPVC,
    AF_X25,
    AF_ROSE,
    AF_DECnet,
    AF_NETBEUI,
    AF_SECURITY,
    AF_KEY,
    AF_NETLINK + 1, /* Last gets CMP_GE, so order is important */
  };
  int i, r;
  g_auto(GLnxTmpfile) seccomp_tmpf  = { 0, };

  seccomp = seccomp_init (SCMP_ACT_ALLOW);
  if (!seccomp)
    return flatpak_fail (error, "Initialize seccomp failed");

  if (arch != NULL)
    {
      uint32_t arch_id = 0;
      const uint32_t *extra_arches = NULL;

      if (strcmp (arch, "i386") == 0)
        {
          arch_id = SCMP_ARCH_X86;
        }
      else if (strcmp (arch, "x86_64") == 0)
        {
          arch_id = SCMP_ARCH_X86_64;
          extra_arches = seccomp_x86_64_extra_arches;
        }
      else if (strcmp (arch, "arm") == 0)
        {
          arch_id = SCMP_ARCH_ARM;
        }
#ifdef SCMP_ARCH_AARCH64
      else if (strcmp (arch, "aarch64") == 0)
        {
          arch_id = SCMP_ARCH_AARCH64;
          extra_arches = seccomp_aarch64_extra_arches;
        }
#endif

      /* We only really need to handle arches on multiarch systems.
       * If only one arch is supported the default is fine */
      if (arch_id != 0)
        {
          /* This *adds* the target arch, instead of replacing the
             native one. This is not ideal, because we'd like to only
             allow the target arch, but we can't really disallow the
             native arch at this point, because then bubblewrap
             couldn't continue running. */
          r = seccomp_arch_add (seccomp, arch_id);
          if (r < 0 && r != -EEXIST)
            return flatpak_fail (error, "Failed to add architecture to seccomp filter");

          if (multiarch && extra_arches != NULL)
            {
              unsigned i;
              for (i = 0; extra_arches[i] != 0; i++)
                {
                  r = seccomp_arch_add (seccomp, extra_arches[i]);
                  if (r < 0 && r != -EEXIST)
                    return flatpak_fail (error, "Failed to add multiarch architecture to seccomp filter");
                }
            }
        }
    }

  /* TODO: Should we filter the kernel keyring syscalls in some way?
   * We do want them to be used by desktop apps, but they could also perhaps
   * leak system stuff or secrets from other apps.
   */

  for (i = 0; i < G_N_ELEMENTS (syscall_blacklist); i++)
    {
      int scall = syscall_blacklist[i].scall;
      if (syscall_blacklist[i].arg)
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 1, *syscall_blacklist[i].arg);
      else
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 0);
      if (r < 0 && r == -EFAULT /* unknown syscall */)
        return flatpak_fail (error, "Failed to block syscall %d", scall);
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS (syscall_nondevel_blacklist); i++)
        {
          int scall = syscall_nondevel_blacklist[i].scall;
          if (syscall_nondevel_blacklist[i].arg)
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 1, *syscall_nondevel_blacklist[i].arg);
          else
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (EPERM), scall, 0);

          if (r < 0 && r == -EFAULT /* unknown syscall */)
            return flatpak_fail (error, "Failed to block syscall %d", scall);
        }
    }

  /* Socket filtering doesn't work on e.g. i386, so ignore failures here
   * However, we need to user seccomp_rule_add_exact to avoid libseccomp doing
   * something else: https://github.com/seccomp/libseccomp/issues/8 */
  for (i = 0; i < G_N_ELEMENTS (socket_family_blacklist); i++)
    {
      int family = socket_family_blacklist[i];
      if (i == G_N_ELEMENTS (socket_family_blacklist) - 1)
        seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_GE, family));
      else
        seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_EQ, family));
    }

  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &seccomp_tmpf, error))
    return FALSE;

  if (seccomp_export_bpf (seccomp, seccomp_tmpf.fd) != 0)
    return flatpak_fail (error, "Failed to export bpf");

  lseek (seccomp_tmpf.fd, 0, SEEK_SET);

  flatpak_bwrap_add_args_data_fd (bwrap,
                                  "--seccomp", glnx_steal_fd (&seccomp_tmpf.fd), NULL);

  return TRUE;
}
#endif

static void
flatpak_run_setup_usr_links (FlatpakBwrap *bwrap,
                             GFile          *runtime_files)
{
  const char *usr_links[] = {"lib", "lib32", "lib64", "bin", "sbin"};
  int i;

  if (runtime_files == NULL)
    return;

  for (i = 0; i < G_N_ELEMENTS (usr_links); i++)
    {
      const char *subdir = usr_links[i];
      g_autoptr(GFile) runtime_subdir = g_file_get_child (runtime_files, subdir);
      if (g_file_query_exists (runtime_subdir, NULL))
        {
          g_autofree char *link = g_strconcat ("usr/", subdir, NULL);
          g_autofree char *dest = g_strconcat ("/", subdir, NULL);
          flatpak_bwrap_add_args (bwrap,
                                  "--symlink", link, dest,
                                  NULL);
        }
    }
}

gboolean
flatpak_run_setup_base_argv (FlatpakBwrap   *bwrap,
                             GFile          *runtime_files,
                             GFile          *app_id_dir,
                             const char     *arch,
                             FlatpakRunFlags flags,
                             GError        **error)
{
  g_autofree char *run_dir = g_strdup_printf ("/run/user/%d", getuid ());
  g_autofree char *passwd_contents = NULL;
  g_autofree char *group_contents = NULL;
  struct group *g = getgrgid (getgid ());
  gulong pers;

  g_autoptr(GFile) etc = NULL;

  passwd_contents = g_strdup_printf ("%s:x:%d:%d:%s:%s:%s\n"
                                     "nfsnobody:x:65534:65534:Unmapped user:/:/sbin/nologin\n",
                                     g_get_user_name (),
                                     getuid (), getgid (),
                                     g_get_real_name (),
                                     g_get_home_dir (),
                                     DEFAULT_SHELL);

  group_contents = g_strdup_printf ("%s:x:%d:%s\n"
                                    "nfsnobody:x:65534:\n",
                                    g->gr_name,
                                    getgid (), g_get_user_name ());

  flatpak_bwrap_add_args (bwrap,
                          "--unshare-pid",
                          "--proc", "/proc",
                          "--dir", "/tmp",
                          "--dir", "/var/tmp",
                          "--dir", "/run/host",
                          "--dir", run_dir,
                          "--setenv", "XDG_RUNTIME_DIR", run_dir,
                          "--symlink", "../run", "/var/run",
                          "--ro-bind", "/sys/block", "/sys/block",
                          "--ro-bind", "/sys/bus", "/sys/bus",
                          "--ro-bind", "/sys/class", "/sys/class",
                          "--ro-bind", "/sys/dev", "/sys/dev",
                          "--ro-bind", "/sys/devices", "/sys/devices",
                          NULL);

  if (flags & FLATPAK_RUN_FLAG_DIE_WITH_PARENT)
    flatpak_bwrap_add_args (bwrap,
                            "--die-with-parent",
                            NULL);

  if (flags & FLATPAK_RUN_FLAG_WRITABLE_ETC)
    flatpak_bwrap_add_args (bwrap,
                            "--dir", "/usr/etc",
                            "--symlink", "usr/etc", "/etc",
                            NULL);

  if (!flatpak_bwrap_add_args_data (bwrap, "passwd", passwd_contents, -1, "/etc/passwd", error))
    return FALSE;

  if (!flatpak_bwrap_add_args_data (bwrap, "group", group_contents, -1, "/etc/group", error))
    return FALSE;

  if (g_file_test ("/etc/machine-id", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap, "--ro-bind", "/etc/machine-id", "/etc/machine-id", NULL);
  else if (g_file_test ("/var/lib/dbus/machine-id", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap, "--ro-bind", "/var/lib/dbus/machine-id", "/etc/machine-id", NULL);

  if (runtime_files)
    etc = g_file_get_child (runtime_files, "etc");
  if (etc != NULL &&
      (flags & FLATPAK_RUN_FLAG_WRITABLE_ETC) == 0 &&
      g_file_query_exists (etc, NULL))
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
      struct dirent *dent;
      char path_buffer[PATH_MAX + 1];
      ssize_t symlink_size;
      gboolean inited;

      inited = glnx_dirfd_iterator_init_at (AT_FDCWD, flatpak_file_get_path_cached (etc), FALSE, &dfd_iter, NULL);

      while (inited)
        {
          g_autofree char *src = NULL;
          g_autofree char *dest = NULL;

          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, NULL) || dent == NULL)
            break;

          if (strcmp (dent->d_name, "passwd") == 0 ||
              strcmp (dent->d_name, "group") == 0 ||
              strcmp (dent->d_name, "machine-id") == 0 ||
              strcmp (dent->d_name, "resolv.conf") == 0 ||
              strcmp (dent->d_name, "host.conf") == 0 ||
              strcmp (dent->d_name, "hosts") == 0 ||
              strcmp (dent->d_name, "localtime") == 0)
            continue;

          src = g_build_filename (flatpak_file_get_path_cached (etc), dent->d_name, NULL);
          dest = g_build_filename ("/etc", dent->d_name, NULL);
          if (dent->d_type == DT_LNK)
            {
              symlink_size = readlinkat (dfd_iter.fd, dent->d_name, path_buffer, sizeof (path_buffer) - 1);
              if (symlink_size < 0)
                {
                  glnx_set_error_from_errno (error);
                  return FALSE;
                }
              path_buffer[symlink_size] = 0;
              flatpak_bwrap_add_args (bwrap, "--symlink", path_buffer, dest, NULL);
            }
          else
            {
              flatpak_bwrap_add_args (bwrap, "--bind", src, dest, NULL);
            }
        }
    }

  if (app_id_dir != NULL)
    {
      g_autoptr(GFile) app_cache_dir = g_file_get_child (app_id_dir, "cache");
      g_autoptr(GFile) app_tmp_dir = g_file_get_child (app_cache_dir, "tmp");
      g_autoptr(GFile) app_data_dir = g_file_get_child (app_id_dir, "data");
      g_autoptr(GFile) app_config_dir = g_file_get_child (app_id_dir, "config");

      flatpak_bwrap_add_args (bwrap,
                              /* These are nice to have as a fixed path */
                              "--bind", flatpak_file_get_path_cached (app_cache_dir), "/var/cache",
                              "--bind", flatpak_file_get_path_cached (app_data_dir), "/var/data",
                              "--bind", flatpak_file_get_path_cached (app_config_dir), "/var/config",
                              "--bind", flatpak_file_get_path_cached (app_tmp_dir), "/var/tmp",
                              NULL);
    }

  flatpak_run_setup_usr_links (bwrap, runtime_files);

  pers = PER_LINUX;

  if ((flags & FLATPAK_RUN_FLAG_SET_PERSONALITY) &&
      flatpak_is_linux32_arch (arch))
    {
      g_debug ("Setting personality linux32");
      pers = PER_LINUX32;
    }

  /* Always set the personallity, and clear all weird flags */
  personality (pers);

#ifdef ENABLE_SECCOMP
  if (!setup_seccomp (bwrap,
                      arch,
                      pers,
                      (flags & FLATPAK_RUN_FLAG_MULTIARCH) != 0,
                      (flags & FLATPAK_RUN_FLAG_DEVEL) != 0,
                      error))
    return FALSE;
#endif

  if ((flags & FLATPAK_RUN_FLAG_WRITABLE_ETC) == 0)
    add_monitor_path_args ((flags & FLATPAK_RUN_FLAG_NO_SESSION_HELPER) == 0, bwrap);

  return TRUE;
}

/* Unset FD_CLOEXEC on the array of fds passed in @user_data */
static void
child_setup (gpointer user_data)
{
  GArray *fd_array = user_data;
  int i;

  /* If no fd_array was specified, don't care. */
  if (fd_array == NULL)
    return;

  /* Otherwise, mark not - close-on-exec all the fds in the array */
  for (i = 0; i < fd_array->len; i++)
    {
      int fd = g_array_index (fd_array, int, i);

      /* We also seek all fds to the start, because this lets
         us use the same fd_array multiple times */
      if (lseek (fd, 0, SEEK_SET) < 0)
        g_printerr ("lseek error in child setup");

      fcntl (fd, F_SETFD, 0);
    }
}

static gboolean
forward_file (XdpDbusDocuments  *documents,
              const char        *app_id,
              const char        *file,
              char             **out_doc_id,
              GError           **error)
{
  int fd, fd_id;
  g_autofree char *doc_id = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  const char *perms[] = { "read", "write", NULL };

  fd = open (file, O_PATH | O_CLOEXEC);
  if (fd == -1)
    return flatpak_fail (error, "Failed to open '%s'", file);

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  if (!xdp_dbus_documents_call_add_sync (documents,
                                         g_variant_new ("h", fd_id),
                                         TRUE, /* reuse */
                                         FALSE, /* not persistent */
                                         fd_list,
                                         &doc_id,
                                         NULL,
                                         NULL,
                                         error))
    return FALSE;

  if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                       doc_id,
                                                       app_id,
                                                       perms,
                                                       NULL,
                                                       error))
    return FALSE;

  *out_doc_id = g_steal_pointer (&doc_id);

  return TRUE;
}

static gboolean
add_rest_args (const char  *app_id,
               FlatpakExports *exports,
               gboolean     file_forwarding,
               const char  *doc_mount_path,
               GPtrArray   *argv_array,
               char        *args[],
               int          n_args,
               GError     **error)
{
  g_autoptr(XdpDbusDocuments) documents = NULL;
  gboolean forwarding = FALSE;
  gboolean forwarding_uri = FALSE;
  gboolean can_forward = TRUE;
  int i;

  if (file_forwarding && doc_mount_path == NULL)
    {
      g_message ("Can't get document portal mount path");
      can_forward = FALSE;
    }
  else if (file_forwarding)
    {
      g_autoptr(GError) local_error = NULL;

      documents = xdp_dbus_documents_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION, 0,
                                                             "org.freedesktop.portal.Documents",
                                                             "/org/freedesktop/portal/documents",
                                                             NULL,
                                                             &local_error);
      if (documents == NULL)
        {
          g_message ("Can't get document portal: %s", local_error->message);
          can_forward = FALSE;
        }
    }

  for (i = 0; i < n_args; i++)
    {
      g_autoptr(GFile) file = NULL;

      if (file_forwarding &&
          (strcmp (args[i], "@@") == 0 ||
           strcmp (args[i], "@@u") == 0))
        {
          forwarding_uri = strcmp (args[i], "@@u") == 0;
          forwarding = !forwarding;
          continue;
        }

      if (can_forward && forwarding)
        {
          if (forwarding_uri)
            {
              if (g_str_has_prefix (args[i], "file:"))
                file = g_file_new_for_uri (args[i]);
              else if (G_IS_DIR_SEPARATOR(args[i][0]))
                file = g_file_new_for_path (args[i]);
            }
          else
            file = g_file_new_for_path (args[i]);
        }

      if (file && !flatpak_exports_path_is_visible (exports,
                                                    flatpak_file_get_path_cached (file)))
        {
          g_autofree char *doc_id = NULL;
          g_autofree char *basename = NULL;
          char *doc_path;
          if (!forward_file (documents, app_id, flatpak_file_get_path_cached (file),
                             &doc_id, error))
            return FALSE;

          basename = g_file_get_basename (file);
          doc_path = g_build_filename (doc_mount_path, doc_id, basename, NULL);

          if (forwarding_uri)
            {
              g_autofree char *path = doc_path;
              doc_path = g_filename_to_uri (path, NULL, NULL);
              /* This should never fail */
              g_assert (doc_path != NULL);
            }

          g_debug ("Forwarding file '%s' as '%s' to %s", args[i], doc_path, app_id);
          g_ptr_array_add (argv_array, doc_path);
        }
      else
        g_ptr_array_add (argv_array, g_strdup (args[i]));
    }

  return TRUE;
}

FlatpakContext *
flatpak_context_load_for_deploy (FlatpakDeploy *deploy,
                                 GError        **error)
{
  g_autoptr(FlatpakContext) context = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_autoptr(GKeyFile) metakey = NULL;

  metakey = flatpak_deploy_get_metadata (deploy);
  context = flatpak_app_compute_permissions (metakey, NULL, error);
  if (context == NULL)
    return NULL;

  overrides = flatpak_deploy_get_overrides (deploy);
  flatpak_context_merge (context, overrides);

  return g_steal_pointer (&context);
}

FlatpakContext *
flatpak_context_load_for_app (const char     *app_id,
                              GError        **error)
{
  g_autofree char *app_ref = NULL;
  g_autoptr(FlatpakDeploy) app_deploy = NULL;

  app_ref = flatpak_find_current_ref (app_id, NULL, error);
  if (app_ref == NULL)
    return NULL;

  app_deploy = flatpak_find_deploy_for_ref (app_ref, NULL, error);
  if (app_deploy == NULL)
    return NULL;

  return flatpak_context_load_for_deploy (app_deploy, error);
}

static char *
calculate_ld_cache_checksum (GVariant *app_deploy_data,
                             GVariant *runtime_deploy_data,
                             const char *app_extensions,
                             const char *runtime_extensions)
{
  g_autoptr(GChecksum) ld_so_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (app_deploy_data)
    g_checksum_update (ld_so_checksum, (guchar *)flatpak_deploy_data_get_commit (app_deploy_data), -1);
  g_checksum_update (ld_so_checksum, (guchar *)flatpak_deploy_data_get_commit (runtime_deploy_data), -1);
  if (app_extensions)
    g_checksum_update (ld_so_checksum, (guchar *)app_extensions, -1);
  if (runtime_extensions)
    g_checksum_update (ld_so_checksum, (guchar *)runtime_extensions, -1);

  return g_strdup (g_checksum_get_string (ld_so_checksum));
}

static gboolean
add_ld_so_conf (FlatpakBwrap   *bwrap,
                GError        **error)
{
  const char *contents =
    "include /run/flatpak/ld.so.conf.d/app-*.conf\n"
    "include /app/etc/ld.so.conf\n"
    "/app/lib\n"
    "include /run/flatpak/ld.so.conf.d/runtime-*.conf\n";

  return flatpak_bwrap_add_args_data (bwrap, "ld-so-conf",
                                      contents, -1, "/etc/ld.so.conf", error);
}

static int
regenerate_ld_cache (GPtrArray      *base_argv_array,
                     GArray         *base_fd_array,
                     GFile          *app_id_dir,
                     const char     *checksum,
                     GFile          *runtime_files,
                     gboolean        generate_ld_so_conf,
                     GCancellable   *cancellable,
                     GError        **error)
{
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  g_autoptr(GArray) combined_fd_array = NULL;
  g_autoptr(GFile) ld_so_cache = NULL;
  g_autofree char *sandbox_cache_path = NULL;
  g_auto(GStrv) minimal_envp = NULL;
  g_autofree char *commandline = NULL;
  int exit_status;
  glnx_autofd int ld_so_fd = -1;
  g_autoptr(GFile) ld_so_dir = NULL;

  if (app_id_dir)
    ld_so_dir = g_file_get_child (app_id_dir, ".ld.so");
  else
    {
      g_autoptr(GFile) base_dir = g_file_new_for_path (g_get_user_cache_dir ());
      ld_so_dir = g_file_resolve_relative_path (base_dir, "flatpak/ld.so");
    }

  ld_so_cache = g_file_get_child (ld_so_dir, checksum);
  ld_so_fd = open (flatpak_file_get_path_cached (ld_so_cache), O_RDONLY);
  if (ld_so_fd >= 0)
    return glnx_steal_fd (&ld_so_fd);

  g_debug ("Regenerating ld.so.cache %s", flatpak_file_get_path_cached (ld_so_cache));

  if (!flatpak_mkdir_p (ld_so_dir, cancellable, error))
    return FALSE;

  minimal_envp = flatpak_run_get_minimal_env (FALSE, FALSE);
  bwrap = flatpak_bwrap_new (minimal_envp);
  flatpak_bwrap_add_args (bwrap, flatpak_get_bwrap (), NULL);

  flatpak_bwrap_append_args (bwrap, base_argv_array);

  flatpak_run_setup_usr_links (bwrap, runtime_files);

  if (generate_ld_so_conf)
    {
      if (!add_ld_so_conf (bwrap, error))
        return -1;
    }
  else
    flatpak_bwrap_add_args (bwrap,
                            "--symlink", "../usr/etc/ld.so.conf", "/etc/ld.so.conf",
                            NULL);

  sandbox_cache_path = g_build_filename ("/run/ld-so-cache-dir", checksum, NULL);

  flatpak_bwrap_add_args (bwrap,
                          "--unshare-pid",
                          "--unshare-ipc",
                          "--unshare-net",
                          "--proc", "/proc",
                          "--dev", "/dev",
                          "--bind", flatpak_file_get_path_cached (ld_so_dir), "/run/ld-so-cache-dir",
                          "ldconfig", "-X", "-C", sandbox_cache_path, NULL);

  g_ptr_array_add (bwrap->argv, NULL);

  commandline = flatpak_quote_argv ((const char **) bwrap->argv->pdata);
  flatpak_debug2 ("Running: '%s'", commandline);

  combined_fd_array = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_append_vals (combined_fd_array, base_fd_array->data, base_fd_array->len);
  g_array_append_vals (combined_fd_array, bwrap->fds->data, bwrap->fds->len);

  if (!g_spawn_sync (NULL,
                     (char **) bwrap->argv->pdata,
                     bwrap->envp,
                     G_SPAWN_SEARCH_PATH,
                     child_setup, combined_fd_array,
                     NULL, NULL,
                     &exit_status,
                     error))
    return -1;

  if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("ldconfig failed, exit status %d"), exit_status);
      return -1;
    }

  ld_so_fd = open (flatpak_file_get_path_cached (ld_so_cache), O_RDONLY);
  if (ld_so_fd < 0)
    {
      flatpak_fail (error, "Can't open generated ld.so.cache");
      return -1;
    }

  if (app_id_dir == NULL)
    {
      /* For runs without an app id dir we always regenerate the ld.so.cache */
      unlink (flatpak_file_get_path_cached (ld_so_cache));
    }
  else
    {
      g_autoptr(GFile) active = g_file_get_child (ld_so_dir, "active");

      /* For app-dirs we keep one checksum alive, by pointing the active symlink to it */

      if (!flatpak_switch_symlink_and_remove (flatpak_file_get_path_cached (active),
                                              checksum, error))
        return -1;
    }

  return glnx_steal_fd (&ld_so_fd);
}

gboolean
flatpak_run_app (const char     *app_ref,
                 FlatpakDeploy  *app_deploy,
                 FlatpakContext *extra_context,
                 const char     *custom_runtime,
                 const char     *custom_runtime_version,
                 FlatpakRunFlags flags,
                 const char     *custom_command,
                 char           *args[],
                 int             n_args,
                 GCancellable   *cancellable,
                 GError        **error)
{
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(GVariant) runtime_deploy_data = NULL;
  g_autoptr(GVariant) app_deploy_data = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) bin_ldconfig = NULL;
  g_autoptr(GFile) app_id_dir = NULL;
  g_autofree char *default_runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  g_auto(GLnxTmpfile) arg_tmpf = { 0, };
  g_autoptr(GPtrArray) real_argv_array = NULL;
  const char *command = "/bin/sh";
  g_autoptr(GError) my_error = NULL;
  g_auto(GStrv) runtime_parts = NULL;
  int i;
  g_autofree char *app_info_path = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_autoptr(FlatpakExports) exports = NULL;
  g_auto(GStrv) app_ref_parts = NULL;
  g_autofree char *commandline = NULL;
  int commandline_2_start;
  g_autofree char *commandline2 = NULL;
  g_autofree char *doc_mount_path = NULL;
  g_autofree char *app_extensions = NULL;
  g_autofree char *runtime_extensions = NULL;
  g_autofree char *checksum = NULL;
  int ld_so_fd = -1;
  g_autoptr(GFile) runtime_ld_so_conf = NULL;
  gboolean generate_ld_so_conf = TRUE;
  gboolean use_ld_so_cache = TRUE;
  struct stat s;

  app_ref_parts = flatpak_decompose_ref (app_ref, error);
  if (app_ref_parts == NULL)
    return FALSE;

  bwrap = flatpak_bwrap_new (NULL);

  if (app_deploy == NULL)
    {
      g_assert (g_str_has_prefix (app_ref, "runtime/"));
      default_runtime = g_strdup (app_ref + strlen ("runtime/"));
    }
  else
    {
      const gchar *key;

      app_deploy_data = flatpak_deploy_get_deploy_data (app_deploy, cancellable, error);
      if (app_deploy_data == NULL)
        return FALSE;

      if ((flags & FLATPAK_RUN_FLAG_DEVEL) != 0)
        key = FLATPAK_METADATA_KEY_SDK;
      else
        key = FLATPAK_METADATA_KEY_RUNTIME;

      metakey = flatpak_deploy_get_metadata (app_deploy);
      default_runtime = g_key_file_get_string (metakey,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               key, &my_error);
      if (my_error)
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }

  runtime_parts = g_strsplit (default_runtime, "/", 0);
  if (g_strv_length (runtime_parts) != 3)
    return flatpak_fail (error, "Wrong number of components in runtime %s", default_runtime);

  if (custom_runtime)
    {
      g_auto(GStrv) custom_runtime_parts = g_strsplit (custom_runtime, "/", 0);

      for (i = 0; i < 3 && custom_runtime_parts[i] != NULL; i++)
        {
          if (strlen (custom_runtime_parts[i]) > 0)
            {
              g_free (runtime_parts[i]);
              runtime_parts[i] = g_steal_pointer (&custom_runtime_parts[i]);
            }
        }
    }

  if (custom_runtime_version)
    {
      g_free (runtime_parts[2]);
      runtime_parts[2] = g_strdup (custom_runtime_version);
    }

  runtime_ref = flatpak_compose_ref (FALSE,
                                     runtime_parts[0],
                                     runtime_parts[2],
                                     runtime_parts[1],
                                     error);
  if (runtime_ref == NULL)
    return FALSE;

  runtime_deploy = flatpak_find_deploy_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    return FALSE;

  runtime_deploy_data = flatpak_deploy_get_deploy_data (runtime_deploy, cancellable, error);
  if (runtime_deploy_data == NULL)
    return FALSE;

  runtime_metakey = flatpak_deploy_get_metadata (runtime_deploy);

  app_context = flatpak_app_compute_permissions (metakey, runtime_metakey, error);
  if (app_context == NULL)
    return FALSE;

  if (app_deploy != NULL)
    {
      overrides = flatpak_deploy_get_overrides (app_deploy);
      flatpak_context_merge (app_context, overrides);
    }

  if (extra_context)
    flatpak_context_merge (app_context, extra_context);

  runtime_files = flatpak_deploy_get_files (runtime_deploy);
  bin_ldconfig = g_file_resolve_relative_path (runtime_files, "bin/ldconfig");
  if (!g_file_query_exists (bin_ldconfig, NULL))
    use_ld_so_cache = FALSE;

  if (app_deploy != NULL)
    {
      app_files = flatpak_deploy_get_files (app_deploy);
      if ((app_id_dir = flatpak_ensure_data_dir (app_ref_parts[1], cancellable, error)) == NULL)
        return FALSE;
    }

  flatpak_run_apply_env_default (bwrap, use_ld_so_cache);
  flatpak_run_apply_env_vars (bwrap, app_context);

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
                          "--lock-file", "/usr/.ref",
                          NULL);

  if (app_files != NULL)
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", flatpak_file_get_path_cached (app_files), "/app",
                            "--lock-file", "/app/.ref",
                            NULL);
  else
    flatpak_bwrap_add_args (bwrap,
                            "--dir", "/app",
                            NULL);

  if (metakey != NULL &&
      !flatpak_run_add_extension_args (bwrap, metakey, app_ref, use_ld_so_cache, &app_extensions, cancellable, error))
    return FALSE;

  if (!flatpak_run_add_extension_args (bwrap, runtime_metakey, runtime_ref, use_ld_so_cache, &runtime_extensions, cancellable, error))
    return FALSE;

  runtime_ld_so_conf = g_file_resolve_relative_path (runtime_files, "etc/ld.so.conf");
  if (lstat (flatpak_file_get_path_cached (runtime_ld_so_conf), &s) == 0)
    generate_ld_so_conf = S_ISREG (s.st_mode) && s.st_size == 0;

  /* At this point we have the minimal argv set up, with just the app, runtime and extensions.
     We can reuse this to generate the ld.so.cache (if needed) */
  if (use_ld_so_cache)
    {
      checksum = calculate_ld_cache_checksum (app_deploy_data, runtime_deploy_data,
					      app_extensions, runtime_extensions);
      ld_so_fd = regenerate_ld_cache (bwrap->argv,
				      bwrap->fds,
				      app_id_dir,
				      checksum,
				      runtime_files,
				      generate_ld_so_conf,
				      cancellable, error);
      if (ld_so_fd == -1)
	return FALSE;
      g_array_append_val (bwrap->fds, ld_so_fd);
    }

  if (flatpak_context_allows_features (app_context, FLATPAK_CONTEXT_FEATURE_DEVEL))
    flags |= FLATPAK_RUN_FLAG_DEVEL;

  if (flatpak_context_allows_features (app_context, FLATPAK_CONTEXT_FEATURE_MULTIARCH))
    flags |= FLATPAK_RUN_FLAG_MULTIARCH;

  if (!flatpak_run_setup_base_argv (bwrap, runtime_files, app_id_dir, app_ref_parts[2], flags, error))
    return FALSE;

  if (generate_ld_so_conf)
    {
      if (!add_ld_so_conf (bwrap, error))
        return FALSE;
    }

  if (ld_so_fd != -1)
    {
      /* Don't add to fd_array, its already there */
      add_args_data_fd (bwrap->argv, NULL, "--ro-bind-data", ld_so_fd, "/etc/ld.so.cache");
    }

  if (!flatpak_run_add_app_info_args (bwrap,
                                      app_files, app_deploy_data, app_extensions,
                                      runtime_files, runtime_deploy_data, runtime_extensions,
                                      app_ref_parts[1], app_ref_parts[3],
                                      runtime_ref, app_context, &app_info_path, error))
    return FALSE;

  add_document_portal_args (bwrap, app_ref_parts[1], &doc_mount_path);

  if (!flatpak_run_add_environment_args (bwrap, app_info_path, flags,
                                         app_ref_parts[1], app_context, app_id_dir, &exports, cancellable, error))
    return FALSE;

  flatpak_run_add_journal_args (bwrap);
  add_font_path_args (bwrap);
  add_icon_path_args (bwrap);

  flatpak_bwrap_add_args (bwrap,
                          /* Not in base, because we don't want this for flatpak build */
                          "--symlink", "/app/lib/debug/source", "/run/build",
                          "--symlink", "/usr/lib/debug/source", "/run/build-runtime",
                          NULL);

  if (custom_command)
    {
      command = custom_command;
    }
  else if (metakey)
    {
      default_command = g_key_file_get_string (metakey,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               FLATPAK_METADATA_KEY_COMMAND,
                                               &my_error);
      if (my_error)
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
      command = default_command;
    }

  real_argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (real_argv_array, g_strdup (flatpak_get_bwrap ()));

  {
    gsize len;
    g_autofree char *args = join_args (bwrap->argv, &len);

    if (!flatpak_buffer_to_sealed_memfd_or_tmpfile (&arg_tmpf, "bwrap-args", args, len, error))
      return FALSE;

    add_args_data_fd (real_argv_array, bwrap->fds,
                      "--args", glnx_steal_fd (&arg_tmpf.fd), NULL);
  }

  commandline_2_start = real_argv_array->len;

  g_ptr_array_add (real_argv_array, g_strdup (command));
  if (!add_rest_args (app_ref_parts[1], exports, (flags & FLATPAK_RUN_FLAG_FILE_FORWARDING) != 0,
                      doc_mount_path,
                      real_argv_array, args, n_args, error))
    return FALSE;

  g_ptr_array_add (real_argv_array, NULL);
  g_ptr_array_add (bwrap->argv, NULL);

  commandline = flatpak_quote_argv ((const char **) bwrap->argv->pdata);
  commandline2 = flatpak_quote_argv (((const char **) real_argv_array->pdata) + commandline_2_start);
  flatpak_debug2 ("Running '%s %s'", commandline, commandline2);

  if ((flags & FLATPAK_RUN_FLAG_BACKGROUND) != 0)
    {
      if (!g_spawn_async (NULL,
                          (char **) real_argv_array->pdata,
                          bwrap->envp,
                          G_SPAWN_SEARCH_PATH,
                          child_setup, bwrap->fds,
                          NULL,
                          error))
        return FALSE;
    }
  else
    {
      /* Ensure we unset O_CLOEXEC */
      child_setup (bwrap->fds);
      if (execvpe (flatpak_get_bwrap (), (char **) real_argv_array->pdata, bwrap->envp) == -1)
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               _("Unable to start app"));
          return FALSE;
        }
      /* Not actually reached... */
    }

  return TRUE;
}
