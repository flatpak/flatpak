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
#include <grp.h>

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
#include "flatpak-systemd-dbus.h"

#define DEFAULT_SHELL "/bin/sh"

typedef enum {
  FLATPAK_CONTEXT_SHARED_NETWORK   = 1 << 0,
  FLATPAK_CONTEXT_SHARED_IPC       = 1 << 1,
} FlatpakContextShares;

/* In numerical order of more privs */
typedef enum {
  FLATPAK_FILESYSTEM_MODE_READ_ONLY    = 1,
  FLATPAK_FILESYSTEM_MODE_READ_WRITE   = 2,
} FlatpakFilesystemMode;


/* Same order as enum */
const char *flatpak_context_shares[] = {
  "network",
  "ipc",
  NULL
};

typedef enum {
  FLATPAK_CONTEXT_SOCKET_X11         = 1 << 0,
  FLATPAK_CONTEXT_SOCKET_WAYLAND     = 1 << 1,
  FLATPAK_CONTEXT_SOCKET_PULSEAUDIO  = 1 << 2,
  FLATPAK_CONTEXT_SOCKET_SESSION_BUS = 1 << 3,
  FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS  = 1 << 4,
} FlatpakContextSockets;

/* Same order as enum */
const char *flatpak_context_sockets[] = {
  "x11",
  "wayland",
  "pulseaudio",
  "session-bus",
  "system-bus",
  NULL
};

const char *dont_mount_in_root[] = {
  ".", "..", "lib", "lib32", "lib64", "bin", "sbin", "usr", "boot", "root",
  "tmp", "etc", "app", "run", "proc", "sys", "dev", "var", NULL
};

typedef enum {
  FLATPAK_CONTEXT_DEVICE_DRI         = 1 << 0,
  FLATPAK_CONTEXT_DEVICE_ALL         = 1 << 1,
} FlatpakContextDevices;

const char *flatpak_context_devices[] = {
  "dri",
  "all",
  NULL
};

typedef enum {
  FLATPAK_CONTEXT_FEATURE_DEVEL        = 1 << 0,
} FlatpakContextFeatures;

const char *flatpak_context_features[] = {
  "devel",
  NULL
};

struct FlatpakContext
{
  FlatpakContextShares  shares;
  FlatpakContextShares  shares_valid;
  FlatpakContextSockets sockets;
  FlatpakContextSockets sockets_valid;
  FlatpakContextDevices devices;
  FlatpakContextDevices devices_valid;
  FlatpakContextFeatures  features;
  FlatpakContextFeatures  features_valid;
  GHashTable           *env_vars;
  GHashTable           *persistent;
  GHashTable           *filesystems;
  GHashTable           *session_bus_policy;
  GHashTable           *system_bus_policy;
};

FlatpakContext *
flatpak_context_new (void)
{
  FlatpakContext *context;

  context = g_slice_new0 (FlatpakContext);
  context->env_vars = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  context->persistent = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->filesystems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->session_bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->system_bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  return context;
}

void
flatpak_context_free (FlatpakContext *context)
{
  g_hash_table_destroy (context->env_vars);
  g_hash_table_destroy (context->persistent);
  g_hash_table_destroy (context->filesystems);
  g_hash_table_destroy (context->session_bus_policy);
  g_hash_table_destroy (context->system_bus_policy);
  g_slice_free (FlatpakContext, context);
}

static guint32
flatpak_context_bitmask_from_string (const char *name, const char **names)
{
  guint32 i;

  for (i = 0; names[i] != NULL; i++)
    {
      if (strcmp (names[i], name) == 0)
        return 1 << i;
    }

  return 0;
}

static char **
flatpak_context_bitmask_to_string (guint32 enabled, guint32 valid, const char **names)
{
  guint32 i;
  GPtrArray *array;

  array = g_ptr_array_new ();

  for (i = 0; names[i] != NULL; i++)
    {
      guint32 bitmask = 1 << i;
      if (valid & bitmask)
        {
          if (enabled & bitmask)
            g_ptr_array_add (array, g_strdup (names[i]));
          else
            g_ptr_array_add (array, g_strdup_printf ("!%s", names[i]));
        }
    }

  g_ptr_array_add (array, NULL);
  return (char **) g_ptr_array_free (array, FALSE);
}

static void
flatpak_context_bitmask_to_args (guint32 enabled, guint32 valid, const char **names,
                                 const char *enable_arg, const char *disable_arg,
                                 GPtrArray *args)
{
  guint32 i;

  for (i = 0; names[i] != NULL; i++)
    {
      guint32 bitmask = 1 << i;
      if (valid & bitmask)
        {
          if (enabled & bitmask)
            g_ptr_array_add (args, g_strdup_printf ("%s=%s", enable_arg, names[i]));
          else
            g_ptr_array_add (args, g_strdup_printf ("%s=%s", disable_arg, names[i]));
        }
    }
}


static FlatpakContextShares
flatpak_context_share_from_string (const char *string, GError **error)
{
  FlatpakContextShares shares = flatpak_context_bitmask_from_string (string, flatpak_context_shares);

  if (shares == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_shares);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown share type %s, valid types are: %s"), string, values);
    }

  return shares;
}

static char **
flatpak_context_shared_to_string (FlatpakContextShares shares, FlatpakContextShares valid)
{
  return flatpak_context_bitmask_to_string (shares, valid, flatpak_context_shares);
}

static void
flatpak_context_shared_to_args (FlatpakContextShares shares,
                                FlatpakContextShares valid,
                                GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (shares, valid, flatpak_context_shares, "--share", "--unshare", args);
}

static FlatpakPolicy
flatpak_policy_from_string (const char *string, GError **error)
{
  const char *policies[] = { "none", "see", "talk", "own", NULL };
  int i;
  g_autofree char *values = NULL;

  for (i = 0; policies[i]; i++)
    {
      if (strcmp (string, policies[i]) == 0)
        return i;
    }

  values = g_strjoinv (", ", (char **)policies);
  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Unknown policy type %s, valid types are: %s"), string, values);

  return -1;
}

static const char *
flatpak_policy_to_string (FlatpakPolicy policy)
{
  if (policy == FLATPAK_POLICY_SEE)
    return "see";
  if (policy == FLATPAK_POLICY_TALK)
    return "talk";
  if (policy == FLATPAK_POLICY_OWN)
    return "own";

  return "none";
}

static gboolean
flatpak_verify_dbus_name (const char *name, GError **error)
{
  const char *name_part;
  g_autofree char *tmp = NULL;

  if (g_str_has_suffix (name, ".*"))
    {
      tmp = g_strndup (name, strlen (name) - 2);
      name_part = tmp;
    }
  else
    {
      name_part = name;
    }

  if (g_dbus_is_name (name_part) && !g_dbus_is_unique_name (name_part))
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Invalid dbus name %s\n"), name);
  return FALSE;
}

static FlatpakContextSockets
flatpak_context_socket_from_string (const char *string, GError **error)
{
  FlatpakContextSockets sockets = flatpak_context_bitmask_from_string (string, flatpak_context_sockets);

  if (sockets == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_sockets);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown socket type %s, valid types are: %s"), string, values);
    }

  return sockets;
}

static char **
flatpak_context_sockets_to_string (FlatpakContextSockets sockets, FlatpakContextSockets valid)
{
  return flatpak_context_bitmask_to_string (sockets, valid, flatpak_context_sockets);
}

static void
flatpak_context_sockets_to_args (FlatpakContextSockets sockets,
                                 FlatpakContextSockets valid,
                                 GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (sockets, valid, flatpak_context_sockets, "--socket", "--nosocket", args);
}

static FlatpakContextDevices
flatpak_context_device_from_string (const char *string, GError **error)
{
  FlatpakContextDevices devices = flatpak_context_bitmask_from_string (string, flatpak_context_devices);

  if (devices == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_devices);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown device type %s, valid types are: %s"), string, values);
    }
  return devices;
}

static char **
flatpak_context_devices_to_string (FlatpakContextDevices devices, FlatpakContextDevices valid)
{
  return flatpak_context_bitmask_to_string (devices, valid, flatpak_context_devices);
}

static void
flatpak_context_devices_to_args (FlatpakContextDevices devices,
                                 FlatpakContextDevices valid,
                                 GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (devices, valid, flatpak_context_devices, "--device", "--nodevice", args);
}

static FlatpakContextFeatures
flatpak_context_feature_from_string (const char *string, GError **error)
{
  FlatpakContextFeatures feature = flatpak_context_bitmask_from_string (string, flatpak_context_features);

  if (feature == 0)
    {
      g_autofree char *values = g_strjoinv (", ", (char **)flatpak_context_features);
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Unknown feature type %s, valid types are: %s"), string, values);
    }

  return feature;
}

static char **
flatpak_context_features_to_string (FlatpakContextFeatures features, FlatpakContextFeatures valid)
{
  return flatpak_context_bitmask_to_string (features, valid, flatpak_context_features);
}

static void
flatpak_context_features_to_args (FlatpakContextFeatures features,
                                FlatpakContextFeatures valid,
                                GPtrArray *args)
{
  return flatpak_context_bitmask_to_args (features, valid, flatpak_context_features, "--allow", "--disallow", args);
}

static void
flatpak_context_add_shares (FlatpakContext      *context,
                            FlatpakContextShares shares)
{
  context->shares_valid |= shares;
  context->shares |= shares;
}

static void
flatpak_context_remove_shares (FlatpakContext      *context,
                               FlatpakContextShares shares)
{
  context->shares_valid |= shares;
  context->shares &= ~shares;
}

static void
flatpak_context_add_sockets (FlatpakContext       *context,
                             FlatpakContextSockets sockets)
{
  context->sockets_valid |= sockets;
  context->sockets |= sockets;
}

static void
flatpak_context_remove_sockets (FlatpakContext       *context,
                                FlatpakContextSockets sockets)
{
  context->sockets_valid |= sockets;
  context->sockets &= ~sockets;
}

static void
flatpak_context_add_devices (FlatpakContext       *context,
                             FlatpakContextDevices devices)
{
  context->devices_valid |= devices;
  context->devices |= devices;
}

static void
flatpak_context_remove_devices (FlatpakContext       *context,
                                FlatpakContextDevices devices)
{
  context->devices_valid |= devices;
  context->devices &= ~devices;
}

static void
flatpak_context_add_features (FlatpakContext       *context,
                           FlatpakContextFeatures   features)
{
  context->features_valid |= features;
  context->features |= features;
}

static void
flatpak_context_remove_features (FlatpakContext       *context,
                               FlatpakContextFeatures  features)
{
  context->features_valid |= features;
  context->features &= ~features;
}

static void
flatpak_context_set_env_var (FlatpakContext *context,
                             const char     *name,
                             const char     *value)
{
  g_hash_table_insert (context->env_vars, g_strdup (name), g_strdup (value));
}

void
flatpak_context_set_session_bus_policy (FlatpakContext *context,
                                        const char     *name,
                                        FlatpakPolicy   policy)
{
  g_hash_table_insert (context->session_bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

void
flatpak_context_set_system_bus_policy (FlatpakContext *context,
                                       const char     *name,
                                       FlatpakPolicy   policy)
{
  g_hash_table_insert (context->system_bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

static void
flatpak_context_set_persistent (FlatpakContext *context,
                                const char     *path)
{
  g_hash_table_insert (context->persistent, g_strdup (path), GINT_TO_POINTER (1));
}

static gboolean
get_user_dir_from_string (const char  *filesystem,
                          const char **config_key,
                          const char **suffix,
                          const char **dir)
{
  char *slash;
  const char *rest;
  g_autofree char *prefix;
  gsize len;

  slash = strchr (filesystem, '/');

  if (slash)
    len = slash - filesystem;
  else
    len = strlen (filesystem);

  rest = filesystem + len;
  while (*rest == '/')
    rest++;

  if (suffix)
    *suffix = rest;

  prefix = g_strndup (filesystem, len);

  if (strcmp (prefix, "xdg-desktop") == 0)
    {
      if (config_key)
        *config_key = "XDG_DESKTOP_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-documents") == 0)
    {
      if (config_key)
        *config_key = "XDG_DOCUMENTS_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_DOCUMENTS);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-download") == 0)
    {
      if (config_key)
        *config_key = "XDG_DOWNLOAD_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-music") == 0)
    {
      if (config_key)
        *config_key = "XDG_MUSIC_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_MUSIC);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-pictures") == 0)
    {
      if (config_key)
        *config_key = "XDG_PICTURES_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_PICTURES);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-public-share") == 0)
    {
      if (config_key)
        *config_key = "XDG_PUBLICSHARE_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_PUBLIC_SHARE);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-templates") == 0)
    {
      if (config_key)
        *config_key = "XDG_TEMPLATES_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_TEMPLATES);
      return TRUE;
    }
  if (strcmp (prefix, "xdg-videos") == 0)
    {
      if (config_key)
        *config_key = "XDG_VIDEOS_DIR";
      if (dir)
        *dir = g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS);
      return TRUE;
    }
  /* Don't support xdg-run without suffix, because that doesn't work */
  if (strcmp (prefix, "xdg-run") == 0 &&
      *rest != 0)
    {
      if (config_key)
        *config_key = NULL;
      if (dir)
        *dir = g_get_user_runtime_dir ();
      return TRUE;
    }

  return FALSE;
}

static char *
parse_filesystem_flags (const char *filesystem, FlatpakFilesystemMode *mode)
{
  gsize len = strlen (filesystem);

  if (mode)
    *mode = FLATPAK_FILESYSTEM_MODE_READ_WRITE;

  if (g_str_has_suffix (filesystem, ":ro"))
    {
      len -= 3;
      if (mode)
        *mode = FLATPAK_FILESYSTEM_MODE_READ_ONLY;
    }
  else if (g_str_has_suffix (filesystem, ":rw"))
    {
      len -= 3;
      if (mode)
        *mode = FLATPAK_FILESYSTEM_MODE_READ_WRITE;
    }

  return g_strndup (filesystem, len);
}

static gboolean
flatpak_context_verify_filesystem (const char *filesystem_and_mode,
                                   GError    **error)
{
  g_autofree char *filesystem = parse_filesystem_flags (filesystem_and_mode, NULL);

  if (strcmp (filesystem, "host") == 0)
    return TRUE;
  if (strcmp (filesystem, "home") == 0)
    return TRUE;
  if (get_user_dir_from_string (filesystem, NULL, NULL, NULL))
    return TRUE;
  if (g_str_has_prefix (filesystem, "~/"))
    return TRUE;
  if (g_str_has_prefix (filesystem, "/"))
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               _("Unknown filesystem location %s, valid types are: host, home, xdg-*[/...], ~/dir, /dir"), filesystem);
  return FALSE;
}

static void
flatpak_context_add_filesystem (FlatpakContext *context,
                                const char     *what)
{
  FlatpakFilesystemMode mode;
  char *fs = parse_filesystem_flags (what, &mode);

  g_hash_table_insert (context->filesystems, fs, GINT_TO_POINTER (mode));
}

static void
flatpak_context_remove_filesystem (FlatpakContext *context,
                                   const char     *what)
{
  g_hash_table_insert (context->filesystems,
                       parse_filesystem_flags (what, NULL),
                       NULL);
}

void
flatpak_context_merge (FlatpakContext *context,
                       FlatpakContext *other)
{
  GHashTableIter iter;
  gpointer key, value;

  context->shares &= ~other->shares_valid;
  context->shares |= other->shares;
  context->shares_valid |= other->shares_valid;
  context->sockets &= ~other->sockets_valid;
  context->sockets |= other->sockets;
  context->sockets_valid |= other->sockets_valid;
  context->devices &= ~other->devices_valid;
  context->devices |= other->devices;
  context->devices_valid |= other->devices_valid;
  context->features &= ~other->features_valid;
  context->features |= other->features;
  context->features_valid |= other->features_valid;

  g_hash_table_iter_init (&iter, other->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->env_vars, g_strdup (key), g_strdup (value));

  g_hash_table_iter_init (&iter, other->persistent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->persistent, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->filesystems, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->session_bus_policy, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->system_bus_policy, g_strdup (key), value);
}

static gboolean
option_share_cb (const gchar *option_name,
                 const gchar *value,
                 gpointer     data,
                 GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextShares share;

  share = flatpak_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  flatpak_context_add_shares (context, share);

  return TRUE;
}

static gboolean
option_unshare_cb (const gchar *option_name,
                   const gchar *value,
                   gpointer     data,
                   GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextShares share;

  share = flatpak_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  flatpak_context_remove_shares (context, share);

  return TRUE;
}

static gboolean
option_socket_cb (const gchar *option_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextSockets socket;

  socket = flatpak_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  flatpak_context_add_sockets (context, socket);

  return TRUE;
}

static gboolean
option_nosocket_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextSockets socket;

  socket = flatpak_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  flatpak_context_remove_sockets (context, socket);

  return TRUE;
}

static gboolean
option_device_cb (const gchar *option_name,
                  const gchar *value,
                  gpointer     data,
                  GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextDevices device;

  device = flatpak_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  flatpak_context_add_devices (context, device);

  return TRUE;
}

static gboolean
option_nodevice_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextDevices device;

  device = flatpak_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  flatpak_context_remove_devices (context, device);

  return TRUE;
}

static gboolean
option_allow_cb (const gchar *option_name,
                 const gchar *value,
                 gpointer     data,
                 GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextFeatures feature;

  feature = flatpak_context_feature_from_string (value, error);
  if (feature == 0)
    return FALSE;

  flatpak_context_add_features (context, feature);

  return TRUE;
}

static gboolean
option_disallow_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;
  FlatpakContextFeatures feature;

  feature = flatpak_context_feature_from_string (value, error);
  if (feature == 0)
    return FALSE;

  flatpak_context_remove_features (context, feature);

  return TRUE;
}

static gboolean
option_filesystem_cb (const gchar *option_name,
                      const gchar *value,
                      gpointer     data,
                      GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_context_verify_filesystem (value, error))
    return FALSE;

  flatpak_context_add_filesystem (context, value);
  return TRUE;
}

static gboolean
option_nofilesystem_cb (const gchar *option_name,
                        const gchar *value,
                        gpointer     data,
                        GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_context_verify_filesystem (value, error))
    return FALSE;

  flatpak_context_remove_filesystem (context, value);
  return TRUE;
}

static gboolean
option_env_cb (const gchar *option_name,
               const gchar *value,
               gpointer     data,
               GError     **error)
{
  FlatpakContext *context = data;

  g_auto(GStrv) split = g_strsplit (value, "=", 2);

  if (split == NULL || split[0] == NULL || split[0][0] == 0 || split[1] == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   _("Invalid env format %s"), value);
      return FALSE;
    }

  flatpak_context_set_env_var (context, split[0], split[1]);
  return TRUE;
}

static gboolean
option_own_name_cb (const gchar *option_name,
                    const gchar *value,
                    gpointer     data,
                    GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_session_bus_policy (context, value, FLATPAK_POLICY_OWN);
  return TRUE;
}

static gboolean
option_talk_name_cb (const gchar *option_name,
                     const gchar *value,
                     gpointer     data,
                     GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_session_bus_policy (context, value, FLATPAK_POLICY_TALK);
  return TRUE;
}

static gboolean
option_system_own_name_cb (const gchar *option_name,
                           const gchar *value,
                           gpointer     data,
                           GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_system_bus_policy (context, value, FLATPAK_POLICY_OWN);
  return TRUE;
}

static gboolean
option_system_talk_name_cb (const gchar *option_name,
                            const gchar *value,
                            gpointer     data,
                            GError     **error)
{
  FlatpakContext *context = data;

  if (!flatpak_verify_dbus_name (value, error))
    return FALSE;

  flatpak_context_set_system_bus_policy (context, value, FLATPAK_POLICY_TALK);
  return TRUE;
}

static gboolean
option_persist_cb (const gchar *option_name,
                   const gchar *value,
                   gpointer     data,
                   GError     **error)
{
  FlatpakContext *context = data;

  flatpak_context_set_persistent (context, value);
  return TRUE;
}

static gboolean option_no_desktop_deprecated;

static GOptionEntry context_options[] = {
  { "share", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_share_cb, N_("Share with host"), N_("SHARE") },
  { "unshare", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_unshare_cb, N_("Unshare with host"), N_("SHARE") },
  { "socket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_socket_cb, N_("Expose socket to app"), N_("SOCKET") },
  { "nosocket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nosocket_cb, N_("Don't expose socket to app"), N_("SOCKET") },
  { "device", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_device_cb, N_("Expose device to app"), N_("DEVICE") },
  { "nodevice", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nodevice_cb, N_("Don't expose device to app"), N_("DEVICE") },
  { "allow", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_allow_cb, N_("Allow feature"), N_("FEATURE") },
  { "disallow", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_disallow_cb, N_("Don't allow feature"), N_("FEATURE") },
  { "filesystem", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_filesystem_cb, N_("Expose filesystem to app (:ro for read-only)"), N_("FILESYSTEM[:ro]") },
  { "nofilesystem", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nofilesystem_cb, N_("Don't expose filesystem to app"), N_("FILESYSTEM") },
  { "env", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_env_cb, N_("Set environment variable"), N_("VAR=VALUE") },
  { "own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_own_name_cb, N_("Allow app to own name on the session bus"), N_("DBUS_NAME") },
  { "talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_talk_name_cb, N_("Allow app to talk to name on the session bus"), N_("DBUS_NAME") },
  { "system-own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_system_own_name_cb, N_("Allow app to own name on the system bus"), N_("DBUS_NAME") },
  { "system-talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_system_talk_name_cb, N_("Allow app to talk to name on the system bus"), N_("DBUS_NAME") },
  { "persist", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_persist_cb, N_("Persist home directory"), N_("FILENAME") },
  /* This is not needed/used anymore, so hidden, but we accept it for backwards compat */
  { "no-desktop", 0, G_OPTION_FLAG_IN_MAIN |  G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &option_no_desktop_deprecated, N_("Don't require a running session (no cgroups creation)"), NULL },
  { NULL }
};

void
flatpak_context_complete (FlatpakContext *context, FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, context_options);
}

GOptionGroup  *
flatpak_context_get_options (FlatpakContext *context)
{
  GOptionGroup *group;

  group = g_option_group_new ("environment",
                              "Runtime Environment",
                              "Runtime Environment",
                              context,
                              NULL);
  g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);

  g_option_group_add_entries (group, context_options);

  return group;
}

static const char *
parse_negated (const char *option, gboolean *negated)
{
  if (option[0] == '!')
    {
      option++;
      *negated = TRUE;
    }
  else
    {
      *negated = FALSE;
    }
  return option;
}

/* This is a merge, not a replace */
gboolean
flatpak_context_load_metadata (FlatpakContext *context,
                               GKeyFile       *metakey,
                               GError        **error)
{
  gboolean remove;
  int i;

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_SHARED, NULL))
    {
      g_auto(GStrv) shares = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                         FLATPAK_METADATA_KEY_SHARED, NULL, error);
      if (shares == NULL)
        return FALSE;

      for (i = 0; shares[i] != NULL; i++)
        {
          FlatpakContextShares share;

          share = flatpak_context_share_from_string (parse_negated (shares[i], &remove), error);
          if (share == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_shares (context, share);
          else
            flatpak_context_add_shares (context, share);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_SOCKETS, NULL))
    {
      g_auto(GStrv) sockets = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                          FLATPAK_METADATA_KEY_SOCKETS, NULL, error);
      if (sockets == NULL)
        return FALSE;

      for (i = 0; sockets[i] != NULL; i++)
        {
          FlatpakContextSockets socket = flatpak_context_socket_from_string (parse_negated (sockets[i], &remove), error);
          if (socket == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_sockets (context, socket);
          else
            flatpak_context_add_sockets (context, socket);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_DEVICES, NULL))
    {
      g_auto(GStrv) devices = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                          FLATPAK_METADATA_KEY_DEVICES, NULL, error);
      if (devices == NULL)
        return FALSE;


      for (i = 0; devices[i] != NULL; i++)
        {
          FlatpakContextDevices device = flatpak_context_device_from_string (parse_negated (devices[i], &remove), error);
          if (device == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_devices (context, device);
          else
            flatpak_context_add_devices (context, device);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_FEATURES, NULL))
    {
      g_auto(GStrv) features = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                         FLATPAK_METADATA_KEY_FEATURES, NULL, error);
      if (features == NULL)
        return FALSE;


      for (i = 0; features[i] != NULL; i++)
        {
          FlatpakContextFeatures feature = flatpak_context_feature_from_string (parse_negated (features[i], &remove), error);
          if (feature == 0)
            return FALSE;
          if (remove)
            flatpak_context_remove_features (context, feature);
          else
            flatpak_context_add_features (context, feature);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_FILESYSTEMS, NULL))
    {
      g_auto(GStrv) filesystems = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                              FLATPAK_METADATA_KEY_FILESYSTEMS, NULL, error);
      if (filesystems == NULL)
        return FALSE;

      for (i = 0; filesystems[i] != NULL; i++)
        {
          const char *fs = parse_negated (filesystems[i], &remove);
          if (!flatpak_context_verify_filesystem (fs, error))
            return FALSE;
          if (remove)
            flatpak_context_remove_filesystem (context, fs);
          else
            flatpak_context_add_filesystem (context, fs);
        }
    }

  if (g_key_file_has_key (metakey, FLATPAK_METADATA_GROUP_CONTEXT, FLATPAK_METADATA_KEY_PERSISTENT, NULL))
    {
      g_auto(GStrv) persistent = g_key_file_get_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                                             FLATPAK_METADATA_KEY_PERSISTENT, NULL, error);
      if (persistent == NULL)
        return FALSE;

      for (i = 0; persistent[i] != NULL; i++)
        flatpak_context_set_persistent (context, persistent[i]);
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, key, NULL);
          FlatpakPolicy policy;

          if (!flatpak_verify_dbus_name (key, error))
            return FALSE;

          policy = flatpak_policy_from_string (value, error);
          if ((int) policy == -1)
            return FALSE;

          flatpak_context_set_session_bus_policy (context, key, policy);
        }
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, key, NULL);
          FlatpakPolicy policy;

          if (!flatpak_verify_dbus_name (key, error))
            return FALSE;

          policy = flatpak_policy_from_string (value, error);
          if ((int) policy == -1)
            return FALSE;

          flatpak_context_set_system_bus_policy (context, key, policy);
        }
    }

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, key, NULL);

          flatpak_context_set_env_var (context, key, value);
        }
    }

  return TRUE;
}

void
flatpak_context_save_metadata (FlatpakContext *context,
                               gboolean        flatten,
                               GKeyFile       *metakey)
{
  g_auto(GStrv) shared = NULL;
  g_auto(GStrv) sockets = NULL;
  g_auto(GStrv) devices = NULL;
  g_auto(GStrv) features = NULL;
  GHashTableIter iter;
  gpointer key, value;
  FlatpakContextShares shares_mask = context->shares;
  FlatpakContextShares shares_valid = context->shares_valid;
  FlatpakContextSockets sockets_mask = context->sockets;
  FlatpakContextSockets sockets_valid = context->sockets_valid;
  FlatpakContextDevices devices_mask = context->devices;
  FlatpakContextDevices devices_valid = context->devices_valid;
  FlatpakContextFeatures features_mask = context->features;
  FlatpakContextFeatures features_valid = context->features;

  if (flatten)
    {
      /* A flattened format means we don't expect this to be merged on top of
         another context. In that case we never need to negate any flags.
         We calculate this by removing the zero parts of the mask from the valid set.
      */
      /* First we make sure only the valid parts of the mask are set, in case we
         got some leftover */
      shares_mask &= shares_valid;
      sockets_mask &= sockets_valid;
      devices_mask &= devices_valid;
      features_mask &= devices_valid;

      /* Then just set the valid set to be the mask set */
      shares_valid = shares_mask;
      sockets_valid = sockets_mask;
      devices_valid = devices_mask;
      features_valid = devices_mask;
    }

  shared = flatpak_context_shared_to_string (shares_mask, shares_valid);
  sockets = flatpak_context_sockets_to_string (sockets_mask, sockets_valid);
  devices = flatpak_context_devices_to_string (devices_mask, devices_valid);
  features = flatpak_context_features_to_string (features_mask, features_valid);

  if (shared[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_SHARED,
                                  (const char * const *) shared, g_strv_length (shared));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_SHARED,
                             NULL);
    }

  if (sockets[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_SOCKETS,
                                  (const char * const *) sockets, g_strv_length (sockets));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_SOCKETS,
                             NULL);
    }

  if (devices[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_DEVICES,
                                  (const char * const *) devices, g_strv_length (devices));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_DEVICES,
                             NULL);
    }

  if (features[0] != NULL)
    {
      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_FEATURES,
                                  (const char * const *) features, g_strv_length (features));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_FEATURES,
                             NULL);
    }

  if (g_hash_table_size (context->filesystems) > 0)
    {
      g_autoptr(GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

      g_hash_table_iter_init (&iter, context->filesystems);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

          if (mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY)
            g_ptr_array_add (array, g_strconcat (key, ":ro", NULL));
          else if (value != NULL)
            g_ptr_array_add (array, g_strdup (key));
        }

      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_FILESYSTEMS,
                                  (const char * const *) array->pdata, array->len);
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_FILESYSTEMS,
                             NULL);
    }

  if (g_hash_table_size (context->persistent) > 0)
    {
      g_autofree char **keys = (char **) g_hash_table_get_keys_as_array (context->persistent, NULL);

      g_key_file_set_string_list (metakey,
                                  FLATPAK_METADATA_GROUP_CONTEXT,
                                  FLATPAK_METADATA_KEY_PERSISTENT,
                                  (const char * const *) keys, g_strv_length (keys));
    }
  else
    {
      g_key_file_remove_key (metakey,
                             FLATPAK_METADATA_GROUP_CONTEXT,
                             FLATPAK_METADATA_KEY_PERSISTENT,
                             NULL);
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);
      if (policy > 0)
        g_key_file_set_string (metakey,
                               FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
                               (char *) key, flatpak_policy_to_string (policy));
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);
      if (policy > 0)
        g_key_file_set_string (metakey,
                               FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY,
                               (char *) key, flatpak_policy_to_string (policy));
    }

  g_key_file_remove_group (metakey, FLATPAK_METADATA_GROUP_ENVIRONMENT, NULL);
  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_key_file_set_string (metakey,
                             FLATPAK_METADATA_GROUP_ENVIRONMENT,
                             (char *) key, (char *) value);
    }
}

void
flatpak_context_allow_host_fs (FlatpakContext *context)
{
  flatpak_context_add_filesystem (context, "host");
}

void
flatpak_context_to_args (FlatpakContext *context,
                         GPtrArray *args)
{
  GHashTableIter iter;
  gpointer key, value;

  flatpak_context_shared_to_args (context->shares, context->shares_valid, args);
  flatpak_context_sockets_to_args (context->sockets, context->sockets_valid, args);
  flatpak_context_devices_to_args (context->devices, context->devices_valid, args);
  flatpak_context_features_to_args (context->features, context->features_valid, args);

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_ptr_array_add (args, g_strdup_printf ("--env=%s=%s", (char *)key, (char *)value));

  g_hash_table_iter_init (&iter, context->persistent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_ptr_array_add (args, g_strdup_printf ("--persist=%s", (char *)key));

  g_hash_table_iter_init (&iter, context->session_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      g_ptr_array_add (args, g_strdup_printf ("--%s-name=%s", flatpak_policy_to_string (policy), name));
    }

  g_hash_table_iter_init (&iter, context->system_bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      g_ptr_array_add (args, g_strdup_printf ("--system-%s-name=%s", flatpak_policy_to_string (policy), name));
    }

  g_hash_table_iter_init (&iter, context->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

      if (mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY)
        g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s:ro", (char *)key));
      else if (mode == FLATPAK_FILESYSTEM_MODE_READ_WRITE)
        g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s", (char *)key));
      else
        g_ptr_array_add (args, g_strdup_printf ("--nofilesystem=%s", (char *)key));
    }
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

#ifdef ENABLE_XAUTH
static gboolean
auth_streq (char *str,
            char *au_str,
            int   au_len)
{
  return au_len == strlen (str) && memcmp (str, au_str, au_len) == 0;
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
      if (xa->family == FamilyLocal &&
          auth_streq (unames.nodename, xa->address, xa->address_length) &&
          (xa->number == NULL || auth_streq (number, xa->number, xa->number_length)))
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

static int
create_tmp_fd (const char *contents,
               gssize      length,
               GError    **error)
{
  char template[] = "/tmp/tmp_fd_XXXXXX";
  int fd;

  if (length < 0)
    length = strlen (contents);

  fd = g_mkstemp (template);
  if (fd < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to create temporary file"));
      return -1;
    }

  if (unlink (template) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to unlink temporary file"));
      close (fd);
      return -1;
    }

  while (length > 0)
    {
      gssize s;

      s = write (fd, contents, length);

      if (s < 0)
        {
          int saved_errno = errno;
          if (saved_errno == EINTR)
            continue;

          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                               _("Failed to write to temporary file"));
          close (fd);
          return -1;
        }

      g_assert (s <= length);

      contents += s;
      length -= s;
    }

  lseek (fd, 0, SEEK_SET);

  return fd;
}

static void
flatpak_run_add_x11_args (GPtrArray *argv_array,
                          GArray    *fd_array,
                          char    ***envp_p,
                          gboolean allowed)
{
  g_autofree char *x11_socket = NULL;
  const char *display;

  /* Always cover /tmp/.X11-unix, that way we never see the host one in case
   * we have access to the host /tmp. If you request X access we'll put the right
   * thing in this anyway.
   */
  add_args (argv_array,
            "--tmpfs", "/tmp/.X11-unix",
            NULL);

  if (!allowed)
    {
      *envp_p = g_environ_unsetenv (*envp_p, "DISPLAY");
      return;
    }

  g_debug ("Allowing x11 access");

  display = g_getenv ("DISPLAY");
  if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
    {
      const char *display_nr = &display[1];
      const char *display_nr_end = display_nr;
      g_autofree char *d = NULL;
      g_autofree char *tmp_path = NULL;

      while (g_ascii_isdigit (*display_nr_end))
        display_nr_end++;

      d = g_strndup (display_nr, display_nr_end - display_nr);
      x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

      add_args (argv_array,
                "--bind", x11_socket, "/tmp/.X11-unix/X99",
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DISPLAY", ":99.0", TRUE);

#ifdef ENABLE_XAUTH
      int fd;
      fd = g_file_open_tmp ("flatpak-xauth-XXXXXX", &tmp_path, NULL);
      if (fd >= 0)
        {
          FILE *output = fdopen (fd, "wb");
          if (output != NULL)
            {
              int tmp_fd = dup (fd);
              if (tmp_fd != -1)
                {
                  g_autofree char *tmp_fd_str = g_strdup_printf ("%d", tmp_fd);
                  g_autofree char *dest = g_strdup_printf ("/run/user/%d/Xauthority", getuid ());

                  write_xauth (d, output);
                  add_args (argv_array,
                            "--bind-data", tmp_fd_str, dest,
                            NULL);
                  if (fd_array)
                    g_array_append_val (fd_array, tmp_fd);
                  *envp_p = g_environ_setenv (*envp_p, "XAUTHORITY", dest, TRUE);
                }

              fclose (output);
              unlink (tmp_path);

              lseek (tmp_fd, 0, SEEK_SET);
            }
          else
            {
              close (fd);
            }
        }
#endif
    }
  else
    {
      *envp_p = g_environ_unsetenv (*envp_p, "DISPLAY");
    }

}

static void
flatpak_run_add_wayland_args (GPtrArray *argv_array,
                              char    ***envp_p)
{
  g_autofree char *wayland_socket = g_build_filename (g_get_user_runtime_dir (), "wayland-0", NULL);
  g_autofree char *sandbox_wayland_socket = g_strdup_printf ("/run/user/%d/wayland-0", getuid ());

  if (g_file_test (wayland_socket, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--bind", wayland_socket, sandbox_wayland_socket,
                NULL);
    }
}

static void
flatpak_run_add_pulseaudio_args (GPtrArray *argv_array,
                                 GArray    *fd_array,
                                 char    ***envp_p)
{
  g_autofree char *pulseaudio_socket = g_build_filename (g_get_user_runtime_dir (), "pulse/native", NULL);

  *envp_p = g_environ_unsetenv (*envp_p, "PULSE_SERVER");
  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      gboolean share_shm = FALSE; /* TODO: When do we add this? */
      g_autofree char *client_config = g_strdup_printf ("enable-shm=%s\n", share_shm ? "yes" : "no");
      g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/pulse/native", getuid ());
      g_autofree char *pulse_server = g_strdup_printf ("unix:/run/user/%d/pulse/native", getuid ());
      g_autofree char *config_path = g_strdup_printf ("/run/user/%d/pulse/config", getuid ());
      int fd;
      g_autofree char *fd_str = NULL;

      fd = create_tmp_fd (client_config, -1, NULL);
      if (fd == -1)
        return;

      fd_str = g_strdup_printf ("%d", fd);
      if (fd_array)
        g_array_append_val (fd_array, fd);

      add_args (argv_array,
                "--bind", pulseaudio_socket, sandbox_socket_path,
                "--bind-data", fd_str, config_path,
                NULL);

      *envp_p = g_environ_setenv (*envp_p, "PULSE_SERVER", pulse_server, TRUE);
      *envp_p = g_environ_setenv (*envp_p, "PULSE_CLIENTCONFIG", config_path, TRUE);
    }
}

static void
flatpak_run_add_journal_args (GPtrArray *argv_array)
{
  g_autofree char *journal_socket_socket = g_strdup ("/run/systemd/journal/socket");
  g_autofree char *journal_stdout_socket = g_strdup ("/run/systemd/journal/stdout");

  if (g_file_test (journal_socket_socket, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--bind", journal_socket_socket, journal_socket_socket,
                NULL);
    }
  if (g_file_test (journal_stdout_socket, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--bind", journal_stdout_socket, journal_stdout_socket,
                NULL);
    }
}

static char *
create_proxy_socket (char *template)
{
  g_autofree char *proxy_socket = g_build_filename (g_get_user_runtime_dir (), template, NULL);
  int fd;

  fd = g_mkstemp (proxy_socket);
  if (fd == -1)
    return NULL;

  close (fd);

  return g_steal_pointer (&proxy_socket);
}

gboolean
flatpak_run_add_system_dbus_args (FlatpakContext *context,
                                  char         ***envp_p,
                                  GPtrArray      *argv_array,
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
      add_args (argv_array,
                "--bind", dbus_system_socket, "/run/dbus/system_bus_socket",
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  else if (dbus_proxy_argv &&
           g_hash_table_size (context->system_bus_policy) > 0)
    {
      g_autofree char *proxy_socket = create_proxy_socket (".system-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      if (dbus_address)
        real_dbus_address = g_strdup (dbus_address);
      else
        real_dbus_address = g_strdup_printf ("unix:path=%s", dbus_system_socket);

      g_ptr_array_add (dbus_proxy_argv, g_strdup (real_dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));


      add_args (argv_array,
                "--bind", proxy_socket, "/run/dbus/system_bus_socket",
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  return FALSE;
}

gboolean
flatpak_run_add_session_dbus_args (GPtrArray *argv_array,
                                   char    ***envp_p,
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

      add_args (argv_array,
                "--bind", dbus_session_socket, sandbox_socket_path,
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }
  else if (dbus_proxy_argv && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket (".session-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      g_ptr_array_add (dbus_proxy_argv, g_strdup (dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));

      add_args (argv_array,
                "--bind", proxy_socket, sandbox_socket_path,
                NULL);
      *envp_p = g_environ_setenv (*envp_p, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }

  return FALSE;
}

static void
flatpak_add_bus_filters (GPtrArray      *dbus_proxy_argv,
                         GHashTable     *ht,
                         const char     *app_id,
                         FlatpakContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_ptr_array_add (dbus_proxy_argv, g_strdup ("--filter"));
  if (app_id)
    {
      g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s", app_id));
      g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s.*", app_id));
    }

  g_hash_table_iter_init (&iter, ht);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FlatpakPolicy policy = GPOINTER_TO_INT (value);

      if (policy > 0)
        g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--%s=%s", flatpak_policy_to_string (policy), (char *) key));
    }
}

gboolean
flatpak_run_add_extension_args (GPtrArray    *argv_array,
                                GKeyFile     *metakey,
                                const char   *full_ref,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_auto(GStrv) parts = NULL;
  gboolean is_app;
  GList *extensions, *l;

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    return flatpak_fail (error, "Failed to determine parts from ref: %s", full_ref);

  is_app = strcmp (parts[0], "app") == 0;

  extensions = flatpak_list_extensions (metakey,
                                        parts[2], parts[3]);

  for (l = extensions; l != NULL; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autofree char *full_directory = g_build_filename (is_app ? "/app" : "/usr", ext->directory, NULL);
      g_autofree char *ref = g_build_filename (full_directory, ".ref", NULL);
      g_autofree char *real_ref = g_build_filename (ext->files_path, ext->directory, ".ref", NULL);

      if (ext->needs_tmpfs)
        {
          g_autofree char *parent = g_path_get_dirname (full_directory);
          add_args (argv_array,
                    "--tmpfs", parent,
                    NULL);
        }

      add_args (argv_array,
                "--bind", ext->files_path, full_directory,
                NULL);

      if (g_file_test (real_ref, G_FILE_TEST_EXISTS))
        add_args (argv_array,
                  "--lock-file", ref,
                  NULL);
    }

  g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);

  return TRUE;
}

static void
add_file_arg (GPtrArray            *argv_array,
              FlatpakFilesystemMode mode,
              const char           *path)
{
  struct stat st;

  if (stat (path, &st) != 0)
    return;

  if (S_ISDIR (st.st_mode) ||
      S_ISREG (st.st_mode))
    {
      add_args (argv_array,
                (mode == FLATPAK_FILESYSTEM_MODE_READ_WRITE) ? "--bind" : "--ro-bind",
                path, path, NULL);
    }
}

void
flatpak_run_add_environment_args (GPtrArray      *argv_array,
                                  GArray         *fd_array,
                                  char         ***envp_p,
                                  GPtrArray      *session_bus_proxy_argv,
                                  GPtrArray      *system_bus_proxy_argv,
                                  const char     *app_id,
                                  FlatpakContext *context,
                                  GFile          *app_id_dir)
{
  GHashTableIter iter;
  gpointer key, value;
  gboolean unrestricted_session_bus;
  gboolean unrestricted_system_bus;
  gboolean home_access = FALSE;
  GString *xdg_dirs_conf = NULL;
  FlatpakFilesystemMode fs_mode, home_mode;

  if ((context->shares & FLATPAK_CONTEXT_SHARED_IPC) == 0)
    {
      g_debug ("Disallowing ipc access");
      add_args (argv_array, "--unshare-ipc", NULL);
    }

  if ((context->shares & FLATPAK_CONTEXT_SHARED_NETWORK) == 0)
    {
      g_debug ("Disallowing network access");
      add_args (argv_array, "--unshare-net", NULL);
    }

  if (context->devices & FLATPAK_CONTEXT_DEVICE_ALL)
    {
      add_args (argv_array,
                "--dev-bind", "/dev", "/dev",
                NULL);
    }
  else
    {
      add_args (argv_array,
                "--dev", "/dev",
                NULL);
      if (context->devices & FLATPAK_CONTEXT_DEVICE_DRI)
        {
          g_debug ("Allowing dri access");
          if (g_file_test ("/dev/dri", G_FILE_TEST_IS_DIR))
            add_args (argv_array, "--dev-bind", "/dev/dri", "/dev/dri", NULL);
          if (g_file_test ("/dev/nvidiactl", G_FILE_TEST_EXISTS))
            {
              add_args (argv_array,
                        "--dev-bind", "/dev/nvidiactl", "/dev/nvidiactl",
                        "--dev-bind", "/dev/nvidia0", "/dev/nvidia0",
                        NULL);
            }
        }
    }

  fs_mode = (FlatpakFilesystemMode) g_hash_table_lookup (context->filesystems, "host");
  if (fs_mode != 0)
    {
      DIR *dir;
      struct dirent *dirent;

      g_debug ("Allowing host-fs access");
      home_access = TRUE;

      /* Bind mount most dirs in / into the new root */
      dir = opendir ("/");
      if (dir != NULL)
        {
          while ((dirent = readdir (dir)))
            {
              g_autofree char *path = NULL;

              if (g_strv_contains (dont_mount_in_root, dirent->d_name))
                continue;

              path = g_build_filename ("/", dirent->d_name, NULL);
              add_file_arg (argv_array, fs_mode, path);
            }
          closedir (dir);
        }
      add_file_arg (argv_array, fs_mode, "/run/media");
    }

  home_mode = (FlatpakFilesystemMode) g_hash_table_lookup (context->filesystems, "home");
  if (home_mode != 0)
    {
      g_debug ("Allowing homedir access");
      home_access = TRUE;

      add_file_arg (argv_array, MAX (home_mode, fs_mode), g_get_home_dir ());
    }

  if (!home_access)
    {
      /* Enable persistent mapping only if no access to real home dir */

      g_hash_table_iter_init (&iter, context->persistent);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          const char *persist = key;
          g_autofree char *src = g_build_filename (g_get_home_dir (), ".var/app", app_id, persist, NULL);
          g_autofree char *dest = g_build_filename (g_get_home_dir (), persist, NULL);

          g_mkdir_with_parents (src, 0755);

          add_args (argv_array,
                    "--bind", src, dest,
                    NULL);
        }
    }

  {
    g_autofree char *run_user_app_dst = g_strdup_printf ("/run/user/%d/app/%s", getuid (), app_id);
    g_autofree char *run_user_app_src = g_build_filename (g_get_user_runtime_dir (), "app", app_id, NULL);

    if (glnx_shutil_mkdir_p_at (AT_FDCWD,
                                run_user_app_src,
                                0700,
                                NULL,
                                NULL))
        add_args (argv_array,
                  "--bind", run_user_app_src, run_user_app_dst,
                  NULL);
  }

  g_hash_table_iter_init (&iter, context->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *filesystem = key;
      FlatpakFilesystemMode mode = GPOINTER_TO_INT (value);

      if (value == NULL ||
          strcmp (filesystem, "host") == 0 ||
          strcmp (filesystem, "home") == 0)
        continue;

      if (g_str_has_prefix (filesystem, "xdg-"))
        {
          const char *path, *rest = NULL;
          const char *config_key = NULL;
          g_autofree char *subpath = NULL;

          if (!get_user_dir_from_string (filesystem, &config_key, &rest, &path))
            {
              g_warning ("Unsupported xdg dir %s\n", filesystem);
              continue;
            }

          if (path == NULL)
            continue; /* Unconfigured, ignore */

          if (strcmp (path, g_get_home_dir ()) == 0)
            {
              /* xdg-user-dirs sets disabled dirs to $HOME, and its in general not a good
                 idea to set full access to $HOME other than explicitly, so we ignore
                 these */
              g_debug ("Xdg dir %s is $HOME (i.e. disabled), ignoring\n", filesystem);
              continue;
            }

          subpath = g_build_filename (path, rest, NULL);
          if (g_file_test (subpath, G_FILE_TEST_EXISTS))
            {
              if (xdg_dirs_conf == NULL)
                xdg_dirs_conf = g_string_new ("");

              if (config_key)
                g_string_append_printf (xdg_dirs_conf, "%s=\"%s\"\n",
                                        config_key, path);

              add_file_arg (argv_array, mode, subpath);
            }
        }
      else if (g_str_has_prefix (filesystem, "~/"))
        {
          g_autofree char *path = NULL;

          path = g_build_filename (g_get_home_dir (), filesystem + 2, NULL);
          if (g_file_test (path, G_FILE_TEST_EXISTS))
            add_file_arg (argv_array, mode, path);
        }
      else if (g_str_has_prefix (filesystem, "/"))
        {
          if (g_file_test (filesystem, G_FILE_TEST_EXISTS))
            add_file_arg (argv_array, mode, filesystem);
        }
      else
        {
          g_warning ("Unexpected filesystem arg %s\n", filesystem);
        }
    }

  /* Do this after setting up everything in the home dir, so its not overwritten */
  if (app_id_dir)
    add_args (argv_array,
              "--bind", flatpak_file_get_path_cached (app_id_dir), flatpak_file_get_path_cached (app_id_dir),
              NULL);

  if (home_access  && app_id_dir != NULL)
    {
      g_autofree char *src_path = g_build_filename (g_get_user_config_dir (),
                                                    "user-dirs.dirs",
                                                    NULL);
      g_autofree char *path = g_build_filename (flatpak_file_get_path_cached (app_id_dir),
                                                "config/user-dirs.dirs", NULL);
      if (g_file_test (src_path, G_FILE_TEST_EXISTS))
        add_args (argv_array,
                  "--ro-bind", src_path, path,
                  NULL);
    }
  else if (xdg_dirs_conf != NULL && app_id_dir != NULL)
    {
      g_autofree char *tmp_path = NULL;
      g_autofree char *path = NULL;
      int fd;

      fd = g_file_open_tmp ("flatpak-user-dir-XXXXXX.dirs", &tmp_path, NULL);
      if (fd >= 0)
        {
          close (fd);
          if (g_file_set_contents (tmp_path, xdg_dirs_conf->str, xdg_dirs_conf->len, NULL))
            {
              int tmp_fd = open (tmp_path, O_RDONLY);
              unlink (tmp_path);
              if (tmp_fd != -1)
                {
                  g_autofree char *tmp_fd_str = g_strdup_printf ("%d", tmp_fd);
                  if (fd_array)
                    g_array_append_val (fd_array, tmp_fd);
                  path = g_build_filename (flatpak_file_get_path_cached (app_id_dir),
                                           "config/user-dirs.dirs", NULL);

                  add_args (argv_array, "--file", tmp_fd_str, path, NULL);
                }
            }
        }
      g_string_free (xdg_dirs_conf, TRUE);
    }

  flatpak_run_add_x11_args (argv_array, fd_array, envp_p,
                            (context->sockets & FLATPAK_CONTEXT_SOCKET_X11) != 0);

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_WAYLAND)
    {
      g_debug ("Allowing wayland access");
      flatpak_run_add_wayland_args (argv_array, envp_p);
    }

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_PULSEAUDIO)
    {
      g_debug ("Allowing pulseaudio access");
      flatpak_run_add_pulseaudio_args (argv_array, fd_array, envp_p);
    }

  unrestricted_session_bus = (context->sockets & FLATPAK_CONTEXT_SOCKET_SESSION_BUS) != 0;
  if (unrestricted_session_bus)
    g_debug ("Allowing session-dbus access");
  if (flatpak_run_add_session_dbus_args (argv_array, envp_p, session_bus_proxy_argv, unrestricted_session_bus) &&
      !unrestricted_session_bus && session_bus_proxy_argv)
    flatpak_add_bus_filters (session_bus_proxy_argv, context->session_bus_policy, app_id, context);

  unrestricted_system_bus = (context->sockets & FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS) != 0;
  if (unrestricted_system_bus)
    g_debug ("Allowing system-dbus access");
  if (flatpak_run_add_system_dbus_args (context, envp_p, argv_array, system_bus_proxy_argv,
                                        unrestricted_system_bus) &&
      !unrestricted_system_bus && system_bus_proxy_argv)
    flatpak_add_bus_filters (system_bus_proxy_argv, context->system_bus_policy, NULL, context);

  if (g_environ_getenv (*envp_p, "LD_LIBRARY_PATH") != NULL)
    {
      /* LD_LIBRARY_PATH is overridden for setuid helper, so pass it as cmdline arg */
      add_args (argv_array,
                "--setenv", "LD_LIBRARY_PATH", g_environ_getenv (*envp_p, "LD_LIBRARY_PATH"),
                NULL);
      *envp_p = g_environ_unsetenv (*envp_p, "LD_LIBRARY_PATH");
    }
}

static const struct {const char *env;
                     const char *val;
} default_exports[] = {
  {"PATH", "/app/bin:/usr/bin"},
  {"LD_LIBRARY_PATH", "/app/lib"},
  {"XDG_CONFIG_DIRS", "/app/etc/xdg:/etc/xdg"},
  {"XDG_DATA_DIRS", "/app/share:/usr/share"},
  {"SHELL", "/bin/sh"},
};

static const struct {const char *env;
                     const char *val;
} devel_exports[] = {
  {"ACLOCAL_PATH", "/app/share/aclocal"},
  {"C_INCLUDE_PATH", "/app/include"},
  {"CPLUS_INCLUDE_PATH", "/app/include"},
  {"LDFLAGS", "-L/app/lib "},
  {"PKG_CONFIG_PATH", "/app/lib/pkgconfig:/app/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"},
  {"LC_ALL", "en_US.utf8"},
};

char **
flatpak_run_get_minimal_env (gboolean devel)
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

  for (i = 0; i < G_N_ELEMENTS (default_exports); i++)
    g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", default_exports[i].env, default_exports[i].val));

  if (devel)
    {
      for (i = 0; i < G_N_ELEMENTS(devel_exports); i++)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", devel_exports[i].env, devel_exports[i].val));
    }

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

char **
flatpak_run_apply_env_default (char **envp)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (default_exports); i++)
    envp = g_environ_setenv (envp, default_exports[i].env, default_exports[i].val, TRUE);

  return envp;
}

char **
flatpak_run_apply_env_appid (char **envp,
                             GFile *app_dir)
{
  g_autoptr(GFile) app_dir_data = NULL;
  g_autoptr(GFile) app_dir_config = NULL;
  g_autoptr(GFile) app_dir_cache = NULL;

  app_dir_data = g_file_get_child (app_dir, "data");
  app_dir_config = g_file_get_child (app_dir, "config");
  app_dir_cache = g_file_get_child (app_dir, "cache");
  envp = g_environ_setenv (envp, "XDG_DATA_HOME", flatpak_file_get_path_cached (app_dir_data), TRUE);
  envp = g_environ_setenv (envp, "XDG_CONFIG_HOME", flatpak_file_get_path_cached (app_dir_config), TRUE);
  envp = g_environ_setenv (envp, "XDG_CACHE_HOME", flatpak_file_get_path_cached (app_dir_cache), TRUE);

  return envp;
}

char **
flatpak_run_apply_env_vars (char **envp, FlatpakContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *var = key;
      const char *val = value;

      if (val && val[0] != 0)
        envp = g_environ_setenv (envp, var, val, TRUE);
      else
        envp = g_environ_unsetenv (envp, var);
    }

  return envp;
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
  g_autoptr(GFile) config_dir = g_file_get_child (dir, "config");

  if (!flatpak_mkdir_p (data_dir, cancellable, error))
    return NULL;

  if (!flatpak_mkdir_p (cache_dir, cancellable, error))
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
                         "No systemd user session available, sandboxing not available");

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
add_font_path_args (GPtrArray *argv_array)
{
  g_autoptr(GFile) home = NULL;
  g_autoptr(GFile) user_font1 = NULL;
  g_autoptr(GFile) user_font2 = NULL;

  if (g_file_test (SYSTEM_FONTS_DIR, G_FILE_TEST_EXISTS))
    {
      add_args (argv_array,
                "--bind", SYSTEM_FONTS_DIR, "/run/host/fonts",
                NULL);
    }

  home = g_file_new_for_path (g_get_home_dir ());
  user_font1 = g_file_resolve_relative_path (home, ".local/share/fonts");
  user_font2 = g_file_resolve_relative_path (home, ".fonts");

  if (g_file_query_exists (user_font1, NULL))
    {
      add_args (argv_array,
                "--bind", flatpak_file_get_path_cached (user_font1), "/run/host/user-fonts",
                NULL);
    }
  else if (g_file_query_exists (user_font2, NULL))
    {
      add_args (argv_array,
                "--bind", flatpak_file_get_path_cached (user_font2), "/run/host/user-fonts",
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

static FlatpakContext *
compute_permissions (GKeyFile *app_metadata,
                     GKeyFile *runtime_metadata,
                     GError  **error)
{
  g_autoptr(FlatpakContext) app_context = NULL;

  app_context = flatpak_context_new ();

  add_default_permissions (app_context);

  if (!flatpak_context_load_metadata (app_context, runtime_metadata, error))
    return NULL;

  if (!flatpak_context_load_metadata (app_context, app_metadata, error))
    return NULL;

  return g_steal_pointer (&app_context);
}

gboolean
flatpak_run_add_app_info_args (GPtrArray      *argv_array,
                               GArray         *fd_array,
                               GFile          *app_files,
                               GFile          *runtime_files,
                               const char     *app_id,
                               const char     *app_branch,
                               const char     *runtime_ref,
                               FlatpakContext *final_app_context,
                               char          **app_info_path_out,
                               GError        **error)
{
  g_autofree char *tmp_path = NULL;
  int fd;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GFile) files = NULL;
  g_autofree char *app_path = NULL;
  g_autofree char *runtime_path = NULL;
  g_autofree char *fd_str = NULL;
  g_autofree char *old_dest = g_strdup_printf ("/run/user/%d/flatpak-info", getuid ());

  fd = g_file_open_tmp ("flatpak-context-XXXXXX", &tmp_path, NULL);
  if (fd < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to open flatpak-info temp file"));
      return FALSE;
    }

  close (fd);

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile, "Application", "name", app_id);
  g_key_file_set_string (keyfile, "Application", "runtime", runtime_ref);

  app_path = g_file_get_path (app_files);
  g_key_file_set_string (keyfile, "Instance", "app-path", app_path);
  runtime_path = g_file_get_path (runtime_files);
  g_key_file_set_string (keyfile, "Instance", "runtime-path", runtime_path);
  if (app_branch != NULL)
    g_key_file_set_string (keyfile, "Instance", "branch", app_branch);

  flatpak_context_save_metadata (final_app_context, TRUE, keyfile);

  if (!g_key_file_save_to_file (keyfile, tmp_path, error))
    return FALSE;

  fd = open (tmp_path, O_RDONLY);
  if (fd == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to open temp file"));
      return FALSE;
    }

  unlink (tmp_path);

  fd_str = g_strdup_printf ("%d", fd);
  if (fd_array)
    g_array_append_val (fd_array, fd);

  add_args (argv_array,
            "--ro-bind-data", fd_str, "/.flatpak-info",
            "--symlink", "../../../.flatpak-info", old_dest,
            NULL);

  if (app_info_path_out != NULL)
    *app_info_path_out = g_strdup_printf ("/proc/self/fd/%d", fd);

  return TRUE;
}

static void
add_monitor_path_args (gboolean use_session_helper,
                       GPtrArray *argv_array)
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
      add_args (argv_array,
                "--bind", monitor_path, "/run/host/monitor",
                NULL);
      add_args (argv_array,
                "--symlink", "/run/host/monitor/localtime", "/etc/localtime",
                NULL);
    }
  else
    {
      char localtime[PATH_MAX + 1];
      ssize_t symlink_size;

      add_args (argv_array,
                "--bind", "/etc/resolv.conf", "/run/host/monitor/resolv.conf",
                NULL);

      symlink_size = readlink ("/etc/localtime", localtime, sizeof (localtime) - 1);
      if (symlink_size > 0)
        {
          localtime[symlink_size] = 0;
          add_args (argv_array,
                    "--symlink", localtime, "/etc/localtime",
                    NULL);
        }
      else
        {
          add_args (argv_array,
                    "--bind", "/etc/localtime", "/etc/localtime",
                    NULL);
        }
    }
}

static void
add_document_portal_args (GPtrArray  *argv_array,
                          const char *app_id)
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
              g_message ("Can't get document portal: %s\n", local_error->message);
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
              add_args (argv_array, "--bind", src_path, dst_path, NULL);
            }
        }
    }
}

gchar *
join_args (GPtrArray *argv_array, gsize *len_out)
{
  gchar *string;
  gchar *ptr;
  gint i;
  gsize len = 0;

  for (i = 0; i < argv_array->len; i++)
    len +=  strlen (argv_array->pdata[i]) + 1;

  string = g_new (gchar, len);
  *string = 0;
  ptr = string;
  for (i = 0; i < argv_array->len; i++)
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
  glnx_fd_close int bwrap_args_fd = -1;
  g_autofree char *bwrap_args_data = NULL;

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

  g_ptr_array_add (bwrap_args, g_strdup ("--ro-bind-data"));
  g_ptr_array_add (bwrap_args, g_strdup_printf ("%d", app_info_fd));
  g_ptr_array_add (bwrap_args, g_strdup ("/.flatpak-info"));

  bwrap_args_data = join_args (bwrap_args, &bwrap_args_len);
  bwrap_args_fd = create_tmp_fd (bwrap_args_data, bwrap_args_len, error);
  if (bwrap_args_fd < 0)
    return FALSE;

  g_ptr_array_insert (argv, i++, g_strdup (flatpak_get_bwrap ()));
  g_ptr_array_insert (argv, i++, g_strdup ("--args"));
  g_ptr_array_insert (argv, i++, g_strdup_printf ("%d", bwrap_args_fd));

  *bwrap_fd_out = bwrap_args_fd;
  bwrap_args_fd = -1; /* Steal it */

  return TRUE;
}

static gboolean
add_dbus_proxy_args (GPtrArray *argv_array,
                     GPtrArray *dbus_proxy_argv,
                     gboolean   enable_logging,
                     int        sync_fds[2],
                     const char *app_info_path,
                     GError   **error)
{
  char x = 'x';
  const char *proxy;
  g_autofree char *commandline = NULL;
  DbusProxySpawnData spawn_data;
  glnx_fd_close int app_info_fd = -1;
  glnx_fd_close int bwrap_args_fd = -1;

  if (dbus_proxy_argv->len == 0)
    return TRUE;

  if (sync_fds[0] == -1)
    {
      g_autofree char *fd_str = NULL;

      if (pipe (sync_fds) < 0)
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               _("Unable to create sync pipe"));
          return FALSE;
        }

      fd_str = g_strdup_printf ("%d", sync_fds[0]);
      add_args (argv_array, "--sync-fd", fd_str, NULL);
    }

  proxy = g_getenv ("FLATPAK_DBUSPROXY");
  if (proxy == NULL)
    proxy = DBUSPROXY;

  g_ptr_array_insert (dbus_proxy_argv, 0, g_strdup (proxy));
  g_ptr_array_insert (dbus_proxy_argv, 1, g_strdup_printf ("--fd=%d", sync_fds[1]));

  if (enable_logging)
    g_ptr_array_add (dbus_proxy_argv, g_strdup ("--log"));

  g_ptr_array_add (dbus_proxy_argv, NULL); /* NULL terminate */

  app_info_fd = open (app_info_path, O_RDONLY);
  if (app_info_fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open app info file: %s"), strerror (errsv));
      return FALSE;
    }

  if (!prepend_bwrap_argv_wrapper (dbus_proxy_argv, app_info_fd, &bwrap_args_fd, error))
    return FALSE;

  commandline = g_strjoinv (" ", (char **) dbus_proxy_argv->pdata);
  g_debug ("Running '%s'", commandline);

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
static inline void
cleanup_seccomp (void *p)
{
  scmp_filter_ctx *pp = (scmp_filter_ctx *) p;

  if (*pp)
    seccomp_release (*pp);
}

static gboolean
setup_seccomp (GPtrArray  *argv_array,
               GArray     *fd_array,
               const char *arch,
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
    {SCMP_SYS (personality)},
    /* Don't allow disabling accounting */
    {SCMP_SYS (acct)},
    /* 16-bit code is unnecessary in the sandbox, and modify_ldt is a
       historic source of interesting information leaks. */
    {SCMP_SYS (modify_ldt)},
    /* Don't allow reading current quota use */
    {SCMP_SYS (quotactl)},

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
  glnx_fd_close int fd = -1;
  g_autofree char *fd_str = NULL;
  g_autofree char *path = NULL;

  seccomp = seccomp_init (SCMP_ACT_ALLOW);
  if (!seccomp)
    return flatpak_fail (error, "Initialize seccomp failed");

  if (arch != NULL)
    {
      uint32_t arch_id = 0;

      if (strcmp (arch, "i386") == 0)
        arch_id = SCMP_ARCH_X86;
      else if (strcmp (arch, "x86_64") == 0)
        arch_id = SCMP_ARCH_X86_64;
      else if (strcmp (arch, "arm") == 0)
        arch_id = SCMP_ARCH_ARM;
#ifdef SCMP_ARCH_AARCH64
      else if (strcmp (arch, "aarch64") == 0)
        arch_id = SCMP_ARCH_AARCH64;
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
        r = seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_GE, family));
      else
        r = seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_EQ, family));
    }

  fd = g_file_open_tmp ("flatpak-seccomp-XXXXXX", &path, error);
  if (fd == -1)
    return FALSE;

  unlink (path);

  if (seccomp_export_bpf (seccomp, fd) != 0)
    return flatpak_fail (error, "Failed to export bpf");

  lseek (fd, 0, SEEK_SET);

  fd_str = g_strdup_printf ("%d", fd);
  if (fd_array)
    g_array_append_val (fd_array, fd);

  add_args (argv_array,
            "--seccomp", fd_str,
            NULL);

  fd = -1; /* Don't close on success */

  return TRUE;
}
#endif

gboolean
flatpak_run_setup_base_argv (GPtrArray      *argv_array,
                             GArray         *fd_array,
                             GFile          *runtime_files,
                             GFile          *app_id_dir,
                             const char     *arch,
                             FlatpakRunFlags flags,
                             GError        **error)
{
  const char *usr_links[] = {"lib", "lib32", "lib64", "bin", "sbin"};
  g_autofree char *run_dir = g_strdup_printf ("/run/user/%d", getuid ());
  int i;
  int passwd_fd = -1;
  g_autofree char *passwd_fd_str = NULL;
  g_autofree char *passwd_contents = NULL;
  int group_fd = -1;
  g_autofree char *group_fd_str = NULL;
  g_autofree char *group_contents = NULL;
  struct group *g = getgrgid (getgid ());

  g_autoptr(GFile) etc = NULL;

  passwd_contents = g_strdup_printf ("%s:x:%d:%d:%s:%s:%s\n"
                                     "nfsnobody:x:65534:65534:Unmapped user:/:/sbin/nologin\n",
                                     g_get_user_name (),
                                     getuid (), getgid (),
                                     g_get_real_name (),
                                     g_get_home_dir (),
                                     DEFAULT_SHELL);

  if ((passwd_fd = create_tmp_fd (passwd_contents, -1, error)) < 0)
    return FALSE;
  passwd_fd_str = g_strdup_printf ("%d", passwd_fd);
  if (fd_array)
    g_array_append_val (fd_array, passwd_fd);

  group_contents = g_strdup_printf ("%s:x:%d:%s\n"
                                    "nfsnobody:x:65534:\n",
                                    g->gr_name,
                                    getgid (), g_get_user_name ());
  if ((group_fd = create_tmp_fd (group_contents, -1, error)) < 0)
    return FALSE;
  group_fd_str = g_strdup_printf ("%d", group_fd);
  if (fd_array)
    g_array_append_val (fd_array, group_fd);

  add_args (argv_array,
            "--unshare-pid",
            "--unshare-user-try",
            "--proc", "/proc",
            "--dir", "/tmp",
            "--dir", "/var/tmp",
            "--dir", "/run/host",
            "--dir", run_dir,
            "--setenv", "XDG_RUNTIME_DIR", run_dir,
            "--symlink", "/run", "/var/run",
            "--ro-bind", "/sys/block", "/sys/block",
            "--ro-bind", "/sys/bus", "/sys/bus",
            "--ro-bind", "/sys/class", "/sys/class",
            "--ro-bind", "/sys/dev", "/sys/dev",
            "--ro-bind", "/sys/devices", "/sys/devices",
            "--bind-data", passwd_fd_str, "/etc/passwd",
            "--bind-data", group_fd_str, "/etc/group",
            "--symlink", "/run/host/monitor/resolv.conf", "/etc/resolv.conf",
            /* Always create a homedir to start from, although it may be covered later */
            "--dir", g_get_home_dir (),
            NULL);

  if (g_file_test ("/etc/machine-id", G_FILE_TEST_EXISTS))
    add_args (argv_array, "--bind", "/etc/machine-id", "/etc/machine-id", NULL);
  else if (g_file_test ("/var/lib/dbus/machine-id", G_FILE_TEST_EXISTS))
    add_args (argv_array, "--bind", "/var/lib/dbus/machine-id", "/etc/machine-id", NULL);

  etc = g_file_get_child (runtime_files, "etc");
  if (g_file_query_exists (etc, NULL))
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
      struct dirent *dent;
      char path_buffer[PATH_MAX + 1];
      ssize_t symlink_size;

      glnx_dirfd_iterator_init_at (AT_FDCWD, flatpak_file_get_path_cached (etc), FALSE, &dfd_iter, NULL);

      while (TRUE)
        {
          g_autofree char *src = NULL;
          g_autofree char *dest = NULL;

          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, NULL) || dent == NULL)
            break;

          if (strcmp (dent->d_name, "passwd") == 0 ||
              strcmp (dent->d_name, "group") == 0 ||
              strcmp (dent->d_name, "machine-id") == 0 ||
              strcmp (dent->d_name, "resolv.conf") == 0 ||
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
              add_args (argv_array, "--symlink", path_buffer, dest, NULL);
            }
          else
            {
              add_args (argv_array, "--bind", src, dest, NULL);
            }
        }
    }

  if (app_id_dir != NULL)
    {
      g_autoptr(GFile) app_cache_dir = g_file_get_child (app_id_dir, "cache");
      g_autoptr(GFile) app_data_dir = g_file_get_child (app_id_dir, "data");
      g_autoptr(GFile) app_config_dir = g_file_get_child (app_id_dir, "config");

      add_args (argv_array,
                /* These are nice to have as a fixed path */
                "--bind", flatpak_file_get_path_cached (app_cache_dir), "/var/cache",
                "--bind", flatpak_file_get_path_cached (app_data_dir), "/var/data",
                "--bind", flatpak_file_get_path_cached (app_config_dir), "/var/config",
                NULL);
    }

  for (i = 0; i < G_N_ELEMENTS (usr_links); i++)
    {
      const char *subdir = usr_links[i];
      g_autoptr(GFile) runtime_subdir = g_file_get_child (runtime_files, subdir);
      if (g_file_query_exists (runtime_subdir, NULL))
        {
          g_autofree char *link = g_strconcat ("usr/", subdir, NULL);
          g_autofree char *dest = g_strconcat ("/", subdir, NULL);
          add_args (argv_array,
                    "--symlink", link, dest,
                    NULL);
        }
    }


#ifdef ENABLE_SECCOMP
  if (!setup_seccomp (argv_array,
                      fd_array,
                      arch,
                      (flags & FLATPAK_RUN_FLAG_DEVEL) != 0,
                      error))
    return FALSE;
#endif

  add_monitor_path_args ((flags & FLATPAK_RUN_FLAG_NO_SESSION_HELPER) == 0, argv_array);

  return TRUE;
}

static void
clear_fd (gpointer data)
{
  int *fd_p = data;
  if (fd_p != NULL && *fd_p != -1)
    close (*fd_p);
}

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
    fcntl (g_array_index (fd_array, int, i), F_SETFD, 0);
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
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) app_id_dir = NULL;
  g_autofree char *default_runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  int sync_fds[2] = {-1, -1};
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_autoptr(GArray) fd_array = NULL;
  g_autoptr(GPtrArray) real_argv_array = NULL;
  g_auto(GStrv) envp = NULL;
  g_autoptr(GPtrArray) session_bus_proxy_argv = NULL;
  g_autoptr(GPtrArray) system_bus_proxy_argv = NULL;
  const char *command = "/bin/sh";
  g_autoptr(GError) my_error = NULL;
  g_auto(GStrv) runtime_parts = NULL;
  int i;
  g_autofree char *app_info_path = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_auto(GStrv) app_ref_parts = NULL;

  app_ref_parts = flatpak_decompose_ref (app_ref, error);
  if (app_ref_parts == NULL)
    return FALSE;

  metakey = flatpak_deploy_get_metadata (app_deploy);

  argv_array = g_ptr_array_new_with_free_func (g_free);
  fd_array = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_clear_func (fd_array, clear_fd);

  session_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
  system_bus_proxy_argv = g_ptr_array_new_with_free_func (g_free);

  default_runtime = g_key_file_get_string (metakey, "Application",
                                           (flags & FLATPAK_RUN_FLAG_DEVEL) != 0 ? "sdk" : "runtime",
                                           &my_error);
  if (my_error)
    {
      g_propagate_error (error, g_steal_pointer (&my_error));
      return FALSE;
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

  runtime_metakey = flatpak_deploy_get_metadata (runtime_deploy);

  app_context = compute_permissions (metakey, runtime_metakey, error);
  if (app_context == NULL)
    return FALSE;

  overrides = flatpak_deploy_get_overrides (app_deploy);
  flatpak_context_merge (app_context, overrides);

  if (extra_context)
    flatpak_context_merge (app_context, extra_context);

  runtime_files = flatpak_deploy_get_files (runtime_deploy);
  app_files = flatpak_deploy_get_files (app_deploy);

  if ((app_id_dir = flatpak_ensure_data_dir (app_ref_parts[1], cancellable, error)) == NULL)
    return FALSE;

  envp = g_get_environ ();
  envp = flatpak_run_apply_env_default (envp);
  envp = flatpak_run_apply_env_vars (envp, app_context);
  envp = flatpak_run_apply_env_appid (envp, app_id_dir);

  add_args (argv_array,
            "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
            "--lock-file", "/usr/.ref",
            "--ro-bind", flatpak_file_get_path_cached (app_files), "/app",
            "--lock-file", "/app/.ref",
            NULL);

  if (app_context->features & FLATPAK_CONTEXT_FEATURE_DEVEL)
    flags |= FLATPAK_RUN_FLAG_DEVEL;

  if (!flatpak_run_setup_base_argv (argv_array, fd_array, runtime_files, app_id_dir, app_ref_parts[2], flags, error))
    return FALSE;

  if (!flatpak_run_add_app_info_args (argv_array, fd_array, app_files, runtime_files, app_ref_parts[1], app_ref_parts[3],
                                      runtime_ref, app_context, &app_info_path, error))
    return FALSE;

  if (!flatpak_run_add_extension_args (argv_array, metakey, app_ref, cancellable, error))
    return FALSE;

  if (!flatpak_run_add_extension_args (argv_array, runtime_metakey, runtime_ref, cancellable, error))
    return FALSE;

  add_document_portal_args (argv_array, app_ref_parts[1]);

  flatpak_run_add_environment_args (argv_array, fd_array, &envp,
                                    session_bus_proxy_argv,
                                    system_bus_proxy_argv,
                                    app_ref_parts[1], app_context, app_id_dir);
  flatpak_run_add_journal_args (argv_array);
  add_font_path_args (argv_array);

  /* Must run this before spawning the dbus proxy, to ensure it
     ends up in the app cgroup */
  if (!flatpak_run_in_transient_unit (app_ref_parts[1], &my_error))
    {
      /* We still run along even if we don't get a cgroup, as nothing
         really depends on it. Its just nice to have */
      g_debug ("Failed to run in transient scope: %s\n", my_error->message);
      g_clear_error (&my_error);
    }

  if (!add_dbus_proxy_args (argv_array, session_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_SESSION_BUS) != 0,
                            sync_fds, app_info_path, error))
    return FALSE;

  if (!add_dbus_proxy_args (argv_array, system_bus_proxy_argv, (flags & FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS) != 0,
                            sync_fds, app_info_path, error))
    return FALSE;

  if (sync_fds[1] != -1)
    close (sync_fds[1]);

  add_args (argv_array,
            /* Not in base, because we don't want this for flatpak build */
            "--symlink", "/app/lib/debug/source", "/run/build",
            "--symlink", "/usr/lib/debug/source", "/run/build-runtime",
            NULL);

  if (custom_command)
    {
      command = custom_command;
    }
  else
    {
      default_command = g_key_file_get_string (metakey, "Application", "command", &my_error);
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
    int arg_fd;
    g_autofree char *arg_fd_str = NULL;
    g_autofree char *args = join_args (argv_array, &len);

    arg_fd = create_tmp_fd (args, len, error);
    if (arg_fd < 0)
      return FALSE;

    arg_fd_str = g_strdup_printf ("%d", arg_fd);
    g_array_append_val (fd_array, arg_fd);

    add_args (real_argv_array,
              "--args", arg_fd_str,
              NULL);
  }

  g_ptr_array_add (real_argv_array, g_strdup (command));
  for (i = 0; i < n_args; i++)
    g_ptr_array_add (real_argv_array, g_strdup (args[i]));

  g_ptr_array_add (real_argv_array, NULL);

  if ((flags & FLATPAK_RUN_FLAG_BACKGROUND) != 0)
    {
      if (!g_spawn_async (NULL,
                          (char **) real_argv_array->pdata,
                          envp,
                          G_SPAWN_SEARCH_PATH,
                          child_setup, fd_array,
                          NULL,
                          error))
        return FALSE;
    }
  else
    {
      if (execvpe (flatpak_get_bwrap (), (char **) real_argv_array->pdata, envp) == -1)
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               _("Unable to start app"));
          return FALSE;
        }
      /* Not actually reached... */
    }

  return TRUE;
}
