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

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-dbus-generated.h"
#include "flatpak-run-private.h"
#include "flatpak-instance.h"
#include <sys/capability.h>

static GOptionEntry options[] = {
  { NULL }
};

static void
drop_all_caps (void)
{
  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = { { 0 } };

  capset (&hdr, data);
}

gboolean
flatpak_builtin_enter (int           argc,
                       char        **argv,
                       GCancellable *cancellable,
                       GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  int rest_argv_start, rest_argc;
  const char *ns_name[] = { "user_base", "ipc", "net", "pid", "mnt", "user" };
  int ns_fd[G_N_ELEMENTS (ns_name)];
  ino_t user_base_ino = 0;
  char *pid_s;
  int pid, i;
  g_autofree char *environment_path = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_autoptr(GPtrArray) envp_array = NULL;
  g_autofree char *environment = NULL;
  gsize environment_len;
  char *e;
  g_autofree char *pulse_path = NULL;
  g_autofree char *session_bus_path = NULL;
  g_autofree char *xdg_runtime_dir = NULL;
  g_autofree char *stat_path = NULL;
  g_autofree char *root_path = NULL;
  g_autofree char *root_link = NULL;
  g_autofree char *cwd_path = NULL;
  g_autofree char *cwd_link = NULL;
  int status;
  struct stat stat_buf;
  uid_t uid;
  gid_t gid;
  g_autoptr(GPtrArray) instances = NULL;
  int j;

  context = g_option_context_new (_("INSTANCE COMMAND [ARGUMENT…] - Run a command inside a running sandbox"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  rest_argc = 0;
  for (i = 1; i < argc; i++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[i][0] != '-')
        {
          rest_argv_start = i;
          rest_argc = argc - i;
          argc = i;
          break;
        }
    }

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (rest_argc < 2)
    {
      usage_error (context, _("INSTANCE and COMMAND must be specified"), error);
      return FALSE;
    }

  pid_s = argv[rest_argv_start];
  pid = atoi (pid_s);

  /* Check to see if it matches some running instance, otherwise use
     as pid if it looks as a number. */
  instances = flatpak_instance_get_all ();
  for (j = 0; j < instances->len; j++)
    {
      FlatpakInstance *instance = (FlatpakInstance *) g_ptr_array_index (instances, j);

      if (pid == flatpak_instance_get_pid (instance) ||
          g_strcmp0 (pid_s, flatpak_instance_get_app (instance)) == 0 ||
          strcmp (pid_s, flatpak_instance_get_id (instance)) == 0)
        {
          pid = flatpak_instance_get_child_pid (instance);
          break;
        }
    }

  if (pid <= 0)
    return flatpak_fail (error, _("%s is neither a pid nor an application or instance ID"), pid_s);

  stat_path = g_strdup_printf ("/proc/%d/root", pid);
  if (stat (stat_path, &stat_buf))
    {
      if (errno == EACCES)
        return flatpak_fail (error, _("entering not supported (need unprivileged user namespaces, or sudo -E)"));
      return flatpak_fail (error, _("No such pid %s"), pid_s);
    }

  uid = stat_buf.st_uid;
  gid = stat_buf.st_gid;

  environment_path = g_strdup_printf ("/proc/%d/environ", pid);
  if (!g_file_get_contents (environment_path, &environment, &environment_len, error))
    return FALSE;

  cwd_path = g_strdup_printf ("/proc/%d/cwd", pid);
  cwd_link = glnx_readlinkat_malloc (-1, cwd_path, NULL, error);
  if (cwd_link == NULL)
    return glnx_prefix_error (error, _("Can't read cwd"));

  root_path = g_strdup_printf ("/proc/%d/root", pid);
  root_link = glnx_readlinkat_malloc (-1, root_path, NULL, error);
  if (root_link == NULL)
    return glnx_prefix_error (error, _("Can't read root"));

  for (i = 0; i < G_N_ELEMENTS (ns_name); i++)
    {
      g_autofree char *path = NULL;
      g_autofree char *self_path = NULL;
      struct stat path_stat, self_path_stat;

      if (strcmp (ns_name[i], "user_base") == 0)
        {
          /* We could use the NS_GET_USERNS ioctl instead of the .userns bind hack, but that would require >= 4.9 kernel */
          path = g_strdup_printf ("%s/run/.userns", root_path);
          self_path = g_strdup ("/proc/self/ns/user");
        }
      else
        {
          path = g_strdup_printf ("/proc/%d/ns/%s", pid, ns_name[i]);
          self_path = g_strdup_printf ("/proc/self/ns/%s", ns_name[i]);
        }

      if (stat (path, &path_stat) != 0)
        {
          if (errno == ENOENT)
            {
              /* If for whatever reason the namespace doesn't exist, skip it */
              ns_fd[i] = -1;
              continue;
            }
          return glnx_prefix_error (error, _("Invalid %s namespace for pid %d"), ns_name[i], pid);
        }

      if (strcmp (ns_name[i], "user") == 0 && path_stat.st_ino == user_base_ino)
        {
          /* bubblewrap did not create an intermediate user namespace */
          ns_fd[i] = -1;
          continue;
        }

      if (stat (self_path, &self_path_stat) != 0)
        return glnx_prefix_error (error, _("Invalid %s namespace for self"), ns_name[i]);

      if (self_path_stat.st_ino == path_stat.st_ino)
        {
          /* No need to setns to the same namespace, it will only fail */
          ns_fd[i] = -1;
          continue;
        }

      if (strcmp (ns_name[i], "user_base") == 0)
        user_base_ino = path_stat.st_ino;

      ns_fd[i] = open (path, O_RDONLY);
      if (ns_fd[i] == -1)
        return flatpak_fail (error, _("Can't open %s namespace: %s"), ns_name[i], g_strerror (errno));
    }

  for (i = 0; i < G_N_ELEMENTS (ns_fd); i++)
    {
      if (ns_fd[i] != -1)
        {
          if (setns (ns_fd[i], 0) == -1)
            {
              if (errno == EPERM)
                return flatpak_fail (error, _("entering not supported (need unprivileged user namespaces)"));
              return flatpak_fail (error, _("Can't enter %s namespace: %s"), ns_name[i], g_strerror (errno));
            }
          close (ns_fd[i]);
        }
    }

  if (chdir (cwd_link))
    return flatpak_fail (error, _("Can't chdir"));

  if (chroot (root_link))
    return flatpak_fail (error, _("Can't chroot"));

  if (setgid (gid))
    return flatpak_fail (error, _("Can't switch gid"));

  if (setuid (uid))
    return flatpak_fail (error, _("Can't switch uid"));

  drop_all_caps ();

  envp_array = g_ptr_array_new_with_free_func (g_free);
  for (e = environment; e < environment + environment_len; e = e + strlen (e) + 1)
    {
      if (*e != 0 &&
          !g_str_has_prefix (e, "DISPLAY=") &&
          !g_str_has_prefix (e, "PULSE_SERVER=") &&
          !g_str_has_prefix (e, "PULSE_CLIENTCONFIG=") &&
          !g_str_has_prefix (e, "XDG_RUNTIME_DIR=") &&
          !g_str_has_prefix (e, "DBUS_SYSTEM_BUS_ADDRESS=") &&
          !g_str_has_prefix (e, "DBUS_SESSION_BUS_ADDRESS="))
        {
          if (g_str_has_prefix (e, "_LD_LIBRARY_PATH="))
            e++;
          g_ptr_array_add (envp_array, g_strdup (e));
        }
    }

  xdg_runtime_dir = g_strdup_printf ("/run/user/%d", uid);
  g_ptr_array_add (envp_array, g_strdup_printf ("XDG_RUNTIME_DIR=%s", xdg_runtime_dir));

  if (g_file_test ("/tmp/.X11-unix/X99", G_FILE_TEST_EXISTS))
    g_ptr_array_add (envp_array, g_strdup ("DISPLAY=:99.0"));

  pulse_path = g_strdup_printf ("/run/user/%d/pulse/native", uid);
  if (g_file_test (pulse_path, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (envp_array, g_strdup_printf ("PULSE_SERVER=unix:%s", pulse_path));
      g_ptr_array_add (envp_array, g_strdup_printf ("PULSE_CLIENTCONFIG=/run/user/%d/pulse/config", uid));
    }

  session_bus_path = g_strdup_printf ("/run/user/%d/bus", uid);
  if (g_file_test (session_bus_path, G_FILE_TEST_EXISTS))
    g_ptr_array_add (envp_array, g_strdup_printf ("DBUS_SESSION_BUS_ADDRESS=unix:path=%s", session_bus_path));

  if (g_file_test ("/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    g_ptr_array_add (envp_array, g_strdup ("DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket"));

  if (getenv ("TERM"))
    g_ptr_array_add (envp_array, g_strdup_printf ("TERM=%s", getenv ("TERM")));

  g_ptr_array_add (envp_array, NULL);

  argv_array = g_ptr_array_new_with_free_func (g_free);
  for (i = 1; i < rest_argc; i++)
    g_ptr_array_add (argv_array, g_strdup (argv[rest_argv_start + i]));
  g_ptr_array_add (argv_array, NULL);

  signal (SIGINT, SIG_IGN);
  if (!g_spawn_sync (NULL, (char **) argv_array->pdata, (char **) envp_array->pdata,
                     G_SPAWN_SEARCH_PATH_FROM_ENVP | G_SPAWN_CHILD_INHERITS_STDIN,
                     NULL, NULL,
                     NULL, NULL,
                     &status, error))
    return FALSE;

  exit (status);
}

gboolean
flatpak_complete_enter (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1:
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      instances = flatpak_instance_get_all ();
      for (i = 0; i < instances->len; i++)
        {
          FlatpakInstance *instance = (FlatpakInstance *) g_ptr_array_index (instances, i);

          const char *app_name = flatpak_instance_get_app (instance);
          if (app_name)
            flatpak_complete_word (completion, "%s ", app_name);

          flatpak_complete_word (completion, "%s ", flatpak_instance_get_id (instance));
        }
      break;

    default:
      break;
    }

  return TRUE;
}
