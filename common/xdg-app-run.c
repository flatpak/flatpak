/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include <X11/Xauth.h>

#include <gio/gio.h>
#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-run.h"
#include "xdg-app-proxy.h"
#include "xdg-app-utils.h"
#include "xdg-app-systemd-dbus.h"


typedef enum {
  XDG_APP_CONTEXT_SHARED_NETWORK   = 1 << 0,
  XDG_APP_CONTEXT_SHARED_IPC       = 1 << 1,
} XdgAppContextShares;

typedef enum {
  XDG_APP_FILESYSTEM_MODE_READ_WRITE   = 1,
  XDG_APP_FILESYSTEM_MODE_READ_ONLY    = 2,
} XdgAppFilesystemMode;


/* Same order as enum */
static const char *xdg_app_context_shares[] = {
  "network",
  "ipc",
  NULL
};

typedef enum {
  XDG_APP_CONTEXT_SOCKET_X11         = 1 << 0,
  XDG_APP_CONTEXT_SOCKET_WAYLAND     = 1 << 1,
  XDG_APP_CONTEXT_SOCKET_PULSEAUDIO  = 1 << 2,
  XDG_APP_CONTEXT_SOCKET_SESSION_BUS = 1 << 3,
  XDG_APP_CONTEXT_SOCKET_SYSTEM_BUS  = 1 << 4,
} XdgAppContextSockets;

/* Same order as enum */
static const char *xdg_app_context_sockets[] = {
  "x11",
  "wayland",
  "pulseaudio",
  "session-bus",
  "system-bus",
  NULL
};

typedef enum {
  XDG_APP_CONTEXT_DEVICE_DRI         = 1 << 0,
} XdgAppContextDevices;

static const char *xdg_app_context_devices[] = {
  "dri",
  NULL
};

struct XdgAppContext {
  XdgAppContextShares shares;
  XdgAppContextShares shares_valid;
  XdgAppContextSockets sockets;
  XdgAppContextSockets sockets_valid;
  XdgAppContextDevices devices;
  XdgAppContextDevices devices_valid;
  GHashTable *env_vars;
  GHashTable *persistent;
  GHashTable *filesystems;
  GHashTable *bus_policy;
};

XdgAppContext *
xdg_app_context_new (void)
{
  XdgAppContext *context;

  context = g_slice_new0 (XdgAppContext);
  context->env_vars = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  context->persistent = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->filesystems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  context->bus_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  return context;
}

void
xdg_app_context_free (XdgAppContext *context)
{
  g_hash_table_destroy (context->env_vars);
  g_hash_table_destroy (context->persistent);
  g_hash_table_destroy (context->filesystems);
  g_hash_table_destroy (context->bus_policy);
  g_slice_free (XdgAppContext, context);
}

static guint32
xdg_app_context_bitmask_from_string (const char *name, const char **names)
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
xdg_app_context_bitmask_to_string (guint32 enabled, guint32 valid, const char **names)
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
  return (char **)g_ptr_array_free (array, FALSE);
}

static XdgAppContextShares
xdg_app_context_share_from_string (const char *string, GError **error)
{
  XdgAppContextShares shares = xdg_app_context_bitmask_from_string (string, xdg_app_context_shares);

  if (shares == 0)
    g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                 "Unknown share type %s, valid types are: network, ipc\n", string);
  return shares;
}

static char **
xdg_app_context_shared_to_string (XdgAppContextShares shares, XdgAppContextShares valid)
{
  return xdg_app_context_bitmask_to_string (shares, valid, xdg_app_context_shares);
}

static XdgAppPolicy
xdg_app_policy_from_string (const char *string, GError **error)
{
  if (strcmp (string, "none") == 0)
    return XDG_APP_POLICY_NONE;
  if (strcmp (string, "see") == 0)
    return XDG_APP_POLICY_SEE;
  if (strcmp (string, "talk") == 0)
    return XDG_APP_POLICY_TALK;
  if (strcmp (string, "own") == 0)
    return XDG_APP_POLICY_OWN;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               "Unknown socket type %s, valid types are: x11,wayland,pulseaudio,session-bus,system-bus\n", string);
  return -1;
}

static const char *
xdg_app_policy_to_string (XdgAppPolicy policy)
{
  if (policy == XDG_APP_POLICY_SEE)
    return "see";
  if (policy == XDG_APP_POLICY_TALK)
    return "talk";
  if (policy == XDG_APP_POLICY_OWN)
    return "own";

  return "none";
}


static gboolean
xdg_app_verify_dbus_name (const char *name, GError **error)
{
  const char *name_part;
  g_autofree char *tmp = NULL;

  if (g_str_has_suffix (name, ".*"))
    {
      tmp = g_strndup (name, strlen (name) - 2);
      name_part = tmp;
    }
  else
    name_part = name;

  if (g_dbus_is_name (name_part) && !g_dbus_is_unique_name (name_part))
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "Invalid dbus name %s\n", name);
  return FALSE;
}

static XdgAppContextSockets
xdg_app_context_socket_from_string (const char *string, GError **error)
{
  XdgAppContextSockets sockets = xdg_app_context_bitmask_from_string (string, xdg_app_context_sockets);

  if (sockets == 0)
    g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                 "Unknown socket type %s, valid types are: x11,wayland,pulseaudio,session-bus,system-bus\n", string);
  return sockets;
}

static char **
xdg_app_context_sockets_to_string (XdgAppContextSockets sockets, XdgAppContextSockets valid)
{
  return xdg_app_context_bitmask_to_string (sockets, valid, xdg_app_context_sockets);
}

static XdgAppContextDevices
xdg_app_context_device_from_string (const char *string, GError **error)
{
  XdgAppContextDevices devices = xdg_app_context_bitmask_from_string (string, xdg_app_context_devices);

  if (devices == 0)
    g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                 "Unknown device type %s, valid types are: dri\n", string);
  return devices;
}

static char **
xdg_app_context_devices_to_string (XdgAppContextDevices devices, XdgAppContextDevices valid)
{
  return xdg_app_context_bitmask_to_string (devices, valid, xdg_app_context_devices);
}

static void
xdg_app_context_add_shares (XdgAppContext            *context,
                            XdgAppContextShares       shares)
{
  context->shares_valid |= shares;
  context->shares |= shares;
}

static void
xdg_app_context_remove_shares (XdgAppContext            *context,
                               XdgAppContextShares       shares)
{
  context->shares_valid |= shares;
  context->shares &= ~shares;
}

static void
xdg_app_context_add_sockets (XdgAppContext            *context,
                             XdgAppContextSockets      sockets)
{
  context->sockets_valid |= sockets;
  context->sockets |= sockets;
}

static void
xdg_app_context_remove_sockets (XdgAppContext            *context,
                                XdgAppContextSockets      sockets)
{
  context->sockets_valid |= sockets;
  context->sockets &= ~sockets;
}

static void
xdg_app_context_add_devices (XdgAppContext            *context,
                             XdgAppContextDevices      devices)
{
  context->devices_valid |= devices;
  context->devices |= devices;
}

static void
xdg_app_context_remove_devices (XdgAppContext            *context,
                                XdgAppContextDevices      devices)
{
  context->devices_valid |= devices;
  context->devices &= ~devices;
}

static void
xdg_app_context_set_env_var (XdgAppContext            *context,
                             const char               *name,
                             const char               *value)
{
  g_hash_table_insert (context->env_vars, g_strdup (name), g_strdup (value));
}

void
xdg_app_context_set_session_bus_policy (XdgAppContext            *context,
                                        const char               *name,
                                        XdgAppPolicy              policy)
{
  g_hash_table_insert (context->bus_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

static void
xdg_app_context_set_persistent (XdgAppContext            *context,
                                const char               *path)
{
  g_hash_table_insert (context->persistent, g_strdup (path), GINT_TO_POINTER (1));
}

static const char *
get_user_dir_config_key (GUserDirectory dir)
{
  switch (dir)
    {
    case G_USER_DIRECTORY_DESKTOP:
      return "XDG_DESKTOP_DIR";
    case G_USER_DIRECTORY_DOCUMENTS:
      return "XDG_DOCUMENTS_DIR";
    case G_USER_DIRECTORY_DOWNLOAD:
      return "XDG_DOWNLOAD_DIR";
    case G_USER_DIRECTORY_MUSIC:
      return "XDG_MUSIC_DIR";
    case G_USER_DIRECTORY_PICTURES:
      return "XDG_PICTURES_DIR";
    case G_USER_DIRECTORY_PUBLIC_SHARE:
      return "XDG_PUBLICSHARE_DIR";
    case G_USER_DIRECTORY_TEMPLATES:
      return "XDG_TEMPLATES_DIR";
    case G_USER_DIRECTORY_VIDEOS:
      return "XDG_VIDEOS_DIR";
    default:
      return NULL;
    }
}

static int
get_user_dir_from_string (const char *filesystem)
{
  if (strcmp (filesystem, "xdg-desktop") == 0)
    return G_USER_DIRECTORY_DESKTOP;
  if (strcmp (filesystem, "xdg-documents") == 0)
    return G_USER_DIRECTORY_DOCUMENTS;
  if (strcmp (filesystem, "xdg-download") == 0)
    return G_USER_DIRECTORY_DOWNLOAD;
  if (strcmp (filesystem, "xdg-music") == 0)
    return G_USER_DIRECTORY_MUSIC;
  if (strcmp (filesystem, "xdg-pictures") == 0)
    return G_USER_DIRECTORY_PICTURES;
  if (strcmp (filesystem, "xdg-public-share") == 0)
    return G_USER_DIRECTORY_PUBLIC_SHARE;
  if (strcmp (filesystem, "xdg-templates") == 0)
    return G_USER_DIRECTORY_TEMPLATES;
  if (strcmp (filesystem, "xdg-videos") == 0)
    return G_USER_DIRECTORY_VIDEOS;

  return -1;
}

static char *
parse_filesystem_flags (const char *filesystem, XdgAppFilesystemMode *mode)
{
  gsize len = strlen (filesystem);

  if (mode)
    *mode = XDG_APP_FILESYSTEM_MODE_READ_WRITE;

  if (g_str_has_suffix (filesystem, ":ro"))
    {
      len -= 3;
      if (mode)
        *mode = XDG_APP_FILESYSTEM_MODE_READ_ONLY;
    }
  else if (g_str_has_suffix (filesystem, ":rw"))
    {
      len -= 3;
      if (mode)
        *mode = XDG_APP_FILESYSTEM_MODE_READ_WRITE;
    }

  return g_strndup (filesystem, len);
}

static gboolean
xdg_app_context_verify_filesystem (const char *filesystem_and_mode,
                                   GError **error)
{
  char *filesystem = parse_filesystem_flags (filesystem_and_mode, NULL);

  if (strcmp (filesystem, "host") == 0)
    return TRUE;
  if (strcmp (filesystem, "home") == 0)
    return TRUE;
  if (get_user_dir_from_string (filesystem) >= 0)
    return TRUE;
  if (g_str_has_prefix (filesystem, "~/"))
    return TRUE;
  if (g_str_has_prefix (filesystem, "/"))
    return TRUE;

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               "Unknown filesystem location %s, valid types are: host,home,xdg-*,~/dir,/dir,\n", filesystem);
  return FALSE;
}

static void
xdg_app_context_add_filesystem (XdgAppContext            *context,
                                const char               *what)
{
  XdgAppFilesystemMode mode;
  char *fs = parse_filesystem_flags (what, &mode);

  g_hash_table_insert (context->filesystems, fs, GINT_TO_POINTER (mode));
}

static void
xdg_app_context_remove_filesystem (XdgAppContext            *context,
                                   const char               *what)
{
  g_hash_table_insert (context->filesystems,
                       parse_filesystem_flags (what, NULL),
                       NULL);
}

void
xdg_app_context_merge (XdgAppContext            *context,
                       XdgAppContext            *other)
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

  g_hash_table_iter_init (&iter, other->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->env_vars, g_strdup (key), g_strdup (value));

  g_hash_table_iter_init (&iter, other->persistent);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->persistent, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->filesystems, g_strdup (key), value);

  g_hash_table_iter_init (&iter, other->bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (context->bus_policy, g_strdup (key), value);
}

static gboolean
option_share_cb (const gchar    *option_name,
                 const gchar    *value,
                 gpointer        data,
                 GError        **error)
{
  XdgAppContext *context = data;
  XdgAppContextShares share;

  share = xdg_app_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  xdg_app_context_add_shares (context, share);

  return TRUE;
}

static gboolean
option_unshare_cb (const gchar    *option_name,
                   const gchar    *value,
                   gpointer        data,
                   GError        **error)
{
  XdgAppContext *context = data;
  XdgAppContextShares share;

  share = xdg_app_context_share_from_string (value, error);
  if (share == 0)
    return FALSE;

  xdg_app_context_remove_shares (context, share);

  return TRUE;
}

static gboolean
option_socket_cb (const gchar    *option_name,
                  const gchar    *value,
                  gpointer        data,
                  GError        **error)
{
  XdgAppContext *context = data;
  XdgAppContextSockets socket;

  socket = xdg_app_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  xdg_app_context_add_sockets (context, socket);

  return TRUE;
}

static gboolean
option_nosocket_cb (const gchar    *option_name,
                    const gchar    *value,
                    gpointer        data,
                    GError        **error)
{
  XdgAppContext *context = data;
  XdgAppContextSockets socket;

  socket = xdg_app_context_socket_from_string (value, error);
  if (socket == 0)
    return FALSE;

  xdg_app_context_remove_sockets (context, socket);

  return TRUE;
}

static gboolean
option_device_cb (const gchar    *option_name,
                  const gchar    *value,
                  gpointer        data,
                  GError        **error)
{
  XdgAppContext *context = data;
  XdgAppContextDevices device;

  device = xdg_app_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  xdg_app_context_add_devices (context, device);

  return TRUE;
}

static gboolean
option_nodevice_cb (const gchar    *option_name,
                    const gchar    *value,
                    gpointer        data,
                    GError        **error)
{
  XdgAppContext *context = data;
  XdgAppContextDevices device;

  device = xdg_app_context_device_from_string (value, error);
  if (device == 0)
    return FALSE;

  xdg_app_context_remove_devices (context, device);

  return TRUE;
}

static gboolean
option_filesystem_cb (const gchar    *option_name,
                      const gchar    *value,
                      gpointer        data,
                      GError        **error)
{
  XdgAppContext *context = data;

  if (!xdg_app_context_verify_filesystem (value, error))
    return FALSE;

  xdg_app_context_add_filesystem (context, value);
  return TRUE;
}

static gboolean
option_nofilesystem_cb (const gchar    *option_name,
                        const gchar    *value,
                        gpointer        data,
                        GError        **error)
{
  XdgAppContext *context = data;

  if (!xdg_app_context_verify_filesystem (value, error))
    return FALSE;

  xdg_app_context_remove_filesystem (context, value);
  return TRUE;
}

static gboolean
option_env_cb (const gchar    *option_name,
               const gchar    *value,
               gpointer        data,
               GError        **error)
{
  XdgAppContext *context = data;
  g_auto(GStrv) split = g_strsplit (value, "=", 2);

  if (split == NULL || split[0] == NULL || split[0][0] == 0 || split[1] == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "Invalid env format %s", value);
      return FALSE;
    }

  xdg_app_context_set_env_var (context, split[0], split[1]);
  return TRUE;
}

static gboolean
option_own_name_cb (const gchar    *option_name,
                    const gchar    *value,
                    gpointer        data,
                    GError        **error)
{
  XdgAppContext *context = data;

  if (!xdg_app_verify_dbus_name (value, error))
    return FALSE;

  xdg_app_context_set_session_bus_policy (context, value, XDG_APP_POLICY_OWN);
  return TRUE;
}

static gboolean
option_talk_name_cb (const gchar    *option_name,
                     const gchar    *value,
                     gpointer        data,
                     GError        **error)
{
  XdgAppContext *context = data;

  if (!xdg_app_verify_dbus_name (value, error))
    return FALSE;

  xdg_app_context_set_session_bus_policy (context, value, XDG_APP_POLICY_TALK);
  return TRUE;
}

static gboolean
option_persist_cb (const gchar    *option_name,
                   const gchar    *value,
                   gpointer        data,
                   GError        **error)
{
  XdgAppContext *context = data;

  xdg_app_context_set_persistent (context, value);
  return TRUE;
}

static GOptionEntry context_options[] = {
  { "share", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_share_cb, "Share with host", "SHARE" },
  { "unshare", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_unshare_cb, "Unshare with host", "SHARE" },
  { "socket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_socket_cb, "Expose socket to app", "SOCKET" },
  { "nosocket", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nosocket_cb, "Don't expose socket to app", "SOCKET" },
  { "device", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_device_cb, "Expose device to app", "DEVICE" },
  { "nodevice", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nodevice_cb, "Don't expose device to app", "DEVICE" },
  { "filesystem", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_filesystem_cb, "Expose filesystem to app (:ro for read-only)", "FILESYSTEM[:ro]" },
  { "nofilesystem", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_nofilesystem_cb, "Don't expose filesystem to app", "FILESYSTEM" },
  { "env", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_env_cb, "Set environment variable", "VAR=VALUE" },
  { "own-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_own_name_cb, "Allow app to own name on the session bus", "DBUS_NAME" },
  { "talk-name", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_talk_name_cb, "Allow app to talk to name on the session bus", "DBUS_NAME" },
  { "persist", 0, G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_CALLBACK, &option_persist_cb, "Persist home directory directory", "FILENAME" },
  { NULL }
};

GOptionGroup  *
xdg_app_context_get_options (XdgAppContext            *context)
{
  GOptionGroup  *group;

  group = g_option_group_new ("environment",
                              "Runtime Environment",
                              "Runtime Environment",
                              context,
                              NULL);

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
    *negated = FALSE;
  return option;
}

/* This is a merge, not a replace */
gboolean
xdg_app_context_load_metadata (XdgAppContext            *context,
                               GKeyFile                 *metakey,
                               GError                  **error)
{
  gboolean remove;
  int i;

  if (g_key_file_has_key (metakey, XDG_APP_METADATA_GROUP_CONTEXT, XDG_APP_METADATA_KEY_SHARED, NULL))
    {
      g_auto(GStrv) shares = g_key_file_get_string_list (metakey, XDG_APP_METADATA_GROUP_CONTEXT,
                                                         XDG_APP_METADATA_KEY_SHARED, NULL, error);
      if (shares == NULL)
        return FALSE;

      for (i = 0; shares[i] != NULL; i++)
        {
          XdgAppContextShares share;

          share = xdg_app_context_share_from_string (parse_negated (shares[i], &remove), error);
          if (share == 0)
            return FALSE;
          if (remove)
            xdg_app_context_remove_shares (context, share);
          else
            xdg_app_context_add_shares (context, share);
        }
    }

  if (g_key_file_has_key (metakey, XDG_APP_METADATA_GROUP_CONTEXT, XDG_APP_METADATA_KEY_SOCKETS, NULL))
    {
      g_auto(GStrv) sockets = g_key_file_get_string_list (metakey, XDG_APP_METADATA_GROUP_CONTEXT,
                                                          XDG_APP_METADATA_KEY_SOCKETS, NULL, error);
      if (sockets == NULL)
        return FALSE;

      for (i = 0; sockets[i] != NULL; i++)
        {
          XdgAppContextSockets socket = xdg_app_context_socket_from_string (parse_negated (sockets[i], &remove), error);
          if (socket == 0)
            return FALSE;
          if (remove)
            xdg_app_context_remove_sockets (context, socket);
          else
            xdg_app_context_add_sockets (context, socket);
        }
    }

  if (g_key_file_has_key (metakey, XDG_APP_METADATA_GROUP_CONTEXT, XDG_APP_METADATA_KEY_DEVICES, NULL))
    {
      g_auto(GStrv) devices = g_key_file_get_string_list (metakey, XDG_APP_METADATA_GROUP_CONTEXT,
                                                          XDG_APP_METADATA_KEY_DEVICES, NULL, error);
      if (devices == NULL)
        return FALSE;


      for (i = 0; devices[i] != NULL; i++)
        {
          XdgAppContextDevices device = xdg_app_context_device_from_string (parse_negated (devices[i], &remove), error);
          if (device == 0)
            return FALSE;
          if (remove)
            xdg_app_context_remove_devices (context, device);
          else
            xdg_app_context_add_devices (context, device);
        }
    }

  if (g_key_file_has_key (metakey, XDG_APP_METADATA_GROUP_CONTEXT, XDG_APP_METADATA_KEY_FILESYSTEMS, NULL))
    {
      g_auto(GStrv) filesystems = g_key_file_get_string_list (metakey, XDG_APP_METADATA_GROUP_CONTEXT,
                                                              XDG_APP_METADATA_KEY_FILESYSTEMS, NULL, error);
      if (filesystems == NULL)
        return FALSE;

      for (i = 0; filesystems[i] != NULL; i++)
        {
          const char *fs = parse_negated (filesystems[i], &remove);
          if (!xdg_app_context_verify_filesystem (fs, error))
            return FALSE;
          if (remove)
            xdg_app_context_remove_filesystem (context, fs);
          else
            xdg_app_context_add_filesystem (context, fs);
        }
    }

  if (g_key_file_has_key (metakey, XDG_APP_METADATA_GROUP_CONTEXT, XDG_APP_METADATA_KEY_PERSISTENT, NULL))
    {
      g_auto(GStrv) persistent = g_key_file_get_string_list (metakey, XDG_APP_METADATA_GROUP_CONTEXT,
                                                                    XDG_APP_METADATA_KEY_PERSISTENT, NULL, error);
      if (persistent == NULL)
        return FALSE;

      for (i = 0; persistent[i] != NULL; i++)
        xdg_app_context_set_persistent (context, persistent[i]);
    }

  if (g_key_file_has_group (metakey, XDG_APP_METADATA_GROUP_SESSION_BUS_POLICY))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, XDG_APP_METADATA_GROUP_SESSION_BUS_POLICY, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, XDG_APP_METADATA_GROUP_SESSION_BUS_POLICY, key, NULL);
          XdgAppPolicy policy;

          if (!xdg_app_verify_dbus_name (key, error))
            return FALSE;

          policy = xdg_app_policy_from_string (value, error);
          if ((int)policy == -1)
            return FALSE;

          xdg_app_context_set_session_bus_policy (context, key, policy);
        }
    }

  if (g_key_file_has_group (metakey, XDG_APP_METADATA_GROUP_ENVIRONMENT))
    {
      g_auto(GStrv) keys = NULL;
      gsize i, keys_count;

      keys = g_key_file_get_keys (metakey, XDG_APP_METADATA_GROUP_ENVIRONMENT, &keys_count, NULL);
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, XDG_APP_METADATA_GROUP_ENVIRONMENT, key, NULL);

          xdg_app_context_set_env_var (context, key, value);
        }
    }

  return TRUE;
}

void
xdg_app_context_save_metadata (XdgAppContext            *context,
                               GKeyFile                 *metakey)
{
  g_auto(GStrv) shared = xdg_app_context_shared_to_string (context->shares, context->shares_valid);
  g_auto(GStrv) sockets = xdg_app_context_sockets_to_string (context->sockets, context->sockets_valid);
  g_auto(GStrv) devices = xdg_app_context_devices_to_string (context->devices, context->devices_valid);
  GHashTableIter iter;
  gpointer key, value;

  if (shared[0] != NULL)
    g_key_file_set_string_list (metakey,
                                XDG_APP_METADATA_GROUP_CONTEXT,
                                XDG_APP_METADATA_KEY_SHARED,
                                (const char * const*)shared, g_strv_length (shared));
  else
    g_key_file_remove_key (metakey,
                           XDG_APP_METADATA_GROUP_CONTEXT,
                           XDG_APP_METADATA_KEY_SHARED,
                           NULL);

  if (sockets[0] != NULL)
    g_key_file_set_string_list (metakey,
                                XDG_APP_METADATA_GROUP_CONTEXT,
                                XDG_APP_METADATA_KEY_SOCKETS,
                                (const char * const*)sockets, g_strv_length (sockets));
  else
    g_key_file_remove_key (metakey,
                           XDG_APP_METADATA_GROUP_CONTEXT,
                           XDG_APP_METADATA_KEY_SOCKETS,
                           NULL);

  if (devices[0] != NULL)
    g_key_file_set_string_list (metakey,
                                XDG_APP_METADATA_GROUP_CONTEXT,
                                XDG_APP_METADATA_KEY_DEVICES,
                                (const char * const*)devices, g_strv_length (devices));
  else
    g_key_file_remove_key (metakey,
                           XDG_APP_METADATA_GROUP_CONTEXT,
                           XDG_APP_METADATA_KEY_DEVICES,
                           NULL);

  if (g_hash_table_size (context->filesystems) > 0)
    {
      g_autoptr(GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

      g_hash_table_iter_init (&iter, context->filesystems);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          XdgAppFilesystemMode mode = GPOINTER_TO_INT (value);

          if (mode == XDG_APP_FILESYSTEM_MODE_READ_ONLY)
            g_ptr_array_add (array, g_strconcat (key, ":ro", NULL));
          else if (value != NULL)
            g_ptr_array_add (array, g_strdup (key));
        }

      g_key_file_set_string_list (metakey,
                                  XDG_APP_METADATA_GROUP_CONTEXT,
                                  XDG_APP_METADATA_KEY_FILESYSTEMS,
                                  (const char * const*)array->pdata, array->len);
    }
  else
    g_key_file_remove_key (metakey,
                           XDG_APP_METADATA_GROUP_CONTEXT,
                           XDG_APP_METADATA_KEY_FILESYSTEMS,
                           NULL);

  if (g_hash_table_size (context->persistent) > 0)
    {
      g_autofree char **keys = (char **)g_hash_table_get_keys_as_array (context->persistent, NULL);

      g_key_file_set_string_list (metakey,
                                  XDG_APP_METADATA_GROUP_CONTEXT,
                                  XDG_APP_METADATA_KEY_PERSISTENT,
                                  (const char * const*)keys, g_strv_length (keys));
    }
  else
    g_key_file_remove_key (metakey,
                           XDG_APP_METADATA_GROUP_CONTEXT,
                           XDG_APP_METADATA_KEY_PERSISTENT,
                           NULL);

  g_key_file_remove_group (metakey, XDG_APP_METADATA_GROUP_SESSION_BUS_POLICY, NULL);
  g_hash_table_iter_init (&iter, context->bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      XdgAppPolicy policy = GPOINTER_TO_INT (value);
      if (policy > 0)
        g_key_file_set_string (metakey,
                               XDG_APP_METADATA_GROUP_SESSION_BUS_POLICY,
                               (char *)key, xdg_app_policy_to_string (policy));
    }

  g_key_file_remove_group (metakey, XDG_APP_METADATA_GROUP_ENVIRONMENT, NULL);
  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_key_file_set_string (metakey,
                             XDG_APP_METADATA_GROUP_ENVIRONMENT,
                             (char *)key, (char *)value);
    }
}

void
xdg_app_context_allow_host_fs (XdgAppContext            *context)
{
  xdg_app_context_add_filesystem (context, "host");
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

static gboolean auth_streq (char *str,
                            char *au_str,
                            int au_len)
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

          if (!XauWriteAuth(output, &local_xa))
            g_warning ("xauth write error");
        }

      XauDisposeAuth(xa);
    }

  fclose (f);
}

static void
xdg_app_run_add_x11_args (GPtrArray *argv_array)
{
  char *x11_socket = NULL;
  const char *display = g_getenv ("DISPLAY");

  if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
    {
      const char *display_nr = &display[1];
      const char *display_nr_end = display_nr;
      g_autofree char *d = NULL;
      g_autofree char *tmp_path = NULL;
      int fd;
      FILE *output;

      while (g_ascii_isdigit (*display_nr_end))
        display_nr_end++;

      d = g_strndup (display_nr, display_nr_end - display_nr);
      x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

      g_ptr_array_add (argv_array, g_strdup ("-x"));
      g_ptr_array_add (argv_array, x11_socket);

      fd = g_file_open_tmp ("xdg-app-xauth-XXXXXX", &tmp_path, NULL);
      if (fd >= 0)
        {
          output = fdopen (fd, "wb");
          if (output != NULL)
            {
              write_xauth (d, output);
              fclose (output);

              g_ptr_array_add (argv_array, g_strdup ("-M"));
              g_ptr_array_add (argv_array, g_strdup_printf ("/run/user/%d/Xauthority=%s", getuid(), tmp_path));
            }
          else
            close (fd);
        }
    }
}

static void
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

static void
xdg_app_run_add_pulseaudio_args (GPtrArray *argv_array)
{
  char *pulseaudio_socket = g_build_filename (g_get_user_runtime_dir (), "pulse/native", NULL);
  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (argv_array, g_strdup ("-p"));
      g_ptr_array_add (argv_array, pulseaudio_socket);
    }
}

static char *
create_proxy_socket (char *template)
{
  g_autofree char *dir = g_build_filename (g_get_user_runtime_dir (), "bus-proxy", NULL);
  g_autofree char *proxy_socket = g_build_filename (dir, template, NULL);
  int fd;

  if (mkdir (dir, 0700) == -1 && errno != EEXIST)
    return NULL;

  fd = g_mkstemp (proxy_socket);
  if (fd == -1)
    return NULL;

  close (fd);

  return g_steal_pointer (&proxy_socket);
}

void
xdg_app_run_add_system_dbus_args (GPtrArray *argv_array,
				  GPtrArray *dbus_proxy_argv)
{
  const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");
  char *dbus_system_socket = NULL;

  if (dbus_address != NULL)
    dbus_system_socket = extract_unix_path_from_dbus_address (dbus_address);
  else if (g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");

  if (dbus_system_socket != NULL)
    {
      g_ptr_array_add (argv_array, g_strdup ("-D"));
      g_ptr_array_add (argv_array, dbus_system_socket);
    }
  else if (dbus_proxy_argv && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("system-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
	return;

      g_ptr_array_add (dbus_proxy_argv, g_strdup (dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));

      g_ptr_array_add (argv_array, g_strdup ("-D"));
      g_ptr_array_add (argv_array, g_strdup (proxy_socket));
    }
}

gboolean
xdg_app_run_add_session_dbus_args (GPtrArray *argv_array,
                                   GPtrArray *dbus_proxy_argv,
                                   gboolean unrestricted)
{
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  char *dbus_session_socket = NULL;

  if (dbus_address == NULL)
    return FALSE;

  dbus_session_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_session_socket != NULL && unrestricted)
    {
      g_ptr_array_add (argv_array, g_strdup ("-d"));
      g_ptr_array_add (argv_array, dbus_session_socket);

      return TRUE;
    }
  else if (dbus_proxy_argv && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("session-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
	return FALSE;

      g_ptr_array_add (dbus_proxy_argv, g_strdup (dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));

      g_ptr_array_add (argv_array, g_strdup ("-d"));
      g_ptr_array_add (argv_array, g_strdup (proxy_socket));

      return TRUE;
    }

  return FALSE;
}

static void
xdg_app_add_bus_filters (GPtrArray *dbus_proxy_argv,
                         const char *app_id,
                         XdgAppContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_ptr_array_add (dbus_proxy_argv, g_strdup ("--filter"));
  g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s", app_id));
  g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s.*", app_id));

  g_hash_table_iter_init (&iter, context->bus_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      XdgAppPolicy policy = GPOINTER_TO_INT (value);

      if (policy > 0)
        g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--%s=%s", xdg_app_policy_to_string (policy), (char *)key));
    }
}

static void
add_extension_arg (const char *directory,
                   const char *type,
                   const char *extension,
                   const char *arch,
                   const char *branch,
                   GPtrArray *argv_array,
                   GCancellable *cancellable)
{
  g_autofree char *extension_ref;
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *full_directory = NULL;
  gboolean is_app;

  is_app = strcmp (type, "app") == 0;

  full_directory = g_build_filename (is_app ? "/app" : "/usr", directory, NULL);

  extension_ref = g_build_filename (type, extension, arch, branch, NULL);
  deploy = xdg_app_find_deploy_dir_for_ref (extension_ref, cancellable, NULL);
  if (deploy != NULL)
    {
      g_autoptr(GFile) files = g_file_get_child (deploy, "files");
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("%s=%s", full_directory, gs_file_get_path_cached (files)));
    }
}


gboolean
xdg_app_run_add_extension_args (GPtrArray   *argv_array,
                                GKeyFile    *metakey,
                                const char  *full_ref,
                                GCancellable *cancellable,
                                GError     **error)
{
  g_auto(GStrv) groups = NULL;
  g_auto(GStrv) parts = NULL;
  int i;

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    return xdg_app_fail (error, "Failed to determine parts from ref: %s", full_ref);

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      char *extension;

      if (g_str_has_prefix (groups[i], "Extension ") &&
          *(extension = (groups[i] + strlen ("Extension "))) != 0)
        {
          g_autofree char *directory = g_key_file_get_string (metakey, groups[i], "directory", NULL);
          g_autofree char *version = g_key_file_get_string (metakey, groups[i], "version", NULL);

          if (directory == NULL)
            continue;

          if (g_key_file_get_boolean (metakey, groups[i],
                                      "subdirectories", NULL))
            {
              g_autofree char *prefix = g_strconcat (extension, ".", NULL);
              g_auto(GStrv) refs = NULL;
              int i;

              refs = xdg_app_list_deployed_refs (parts[0], prefix, parts[2], parts[3],
                                                 cancellable, error);
              if (refs == NULL)
                return FALSE;

              for (i = 0; refs[i] != NULL; i++)
                {
                  g_autofree char *extended_dir = g_build_filename (directory, refs[i] + strlen (prefix), NULL);
                  add_extension_arg (extended_dir, parts[0], refs[i], parts[2], parts[3],
                                     argv_array, cancellable);
                }
            }
          else
            add_extension_arg (directory, parts[0], extension, parts[2], version ? version : parts[3],
                               argv_array, cancellable);
        }
    }

  return TRUE;
}

void
xdg_app_run_add_environment_args (GPtrArray *argv_array,
				  GPtrArray *dbus_proxy_argv,
                                  const char  *doc_mount_path,
                                  const char *app_id,
                                  XdgAppContext *context,
                                  GFile *app_id_dir)
{
  GHashTableIter iter;
  gpointer key, value;
  gboolean unrestricted_session_bus;
  gboolean home_access = FALSE;
  GString *xdg_dirs_conf = NULL;
  char opts[16];
  XdgAppFilesystemMode fs_mode, home_mode;
  int i;

  i = 0;
  opts[i++] = '-';

  if (context->shares & XDG_APP_CONTEXT_SHARED_IPC)
    {
      g_debug ("Allowing ipc access");
      opts[i++] = 'i';
    }

  if (context->shares & XDG_APP_CONTEXT_SHARED_NETWORK)
    {
      g_debug ("Allowing network access");
      opts[i++] = 'n';
    }

  if (context->devices & XDG_APP_CONTEXT_DEVICE_DRI)
    {
      g_debug ("Allowing dri access");
      opts[i++] = 'g';
    }

  fs_mode = (XdgAppFilesystemMode)g_hash_table_lookup (context->filesystems, "host");
  if (fs_mode != 0)
    {
      g_debug ("Allowing host-fs access");
      if (fs_mode == XDG_APP_FILESYSTEM_MODE_READ_WRITE)
        opts[i++] = 'F';
      else
        opts[i++] = 'f';
      home_access = TRUE;
    }

  home_mode = (XdgAppFilesystemMode)g_hash_table_lookup (context->filesystems, "home");
  if (home_mode != 0)
    {
      g_debug ("Allowing homedir access");
      if (home_mode == XDG_APP_FILESYSTEM_MODE_READ_WRITE)
        opts[i++] = 'H';
      else
        opts[i++] = 'h';
      home_access = TRUE;
    }

  if (!home_access)
    {
      /* Enable persistant mapping only if no access to real home dir */

      g_hash_table_iter_init (&iter, context->persistent);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          const char *persist = key;
          g_autofree char *src = g_build_filename (g_get_home_dir (), ".var/app", app_id, persist, NULL);
          g_autofree char *dest = g_build_filename (g_get_home_dir (), persist, NULL);

          g_mkdir_with_parents (src, 0755);

          g_ptr_array_add (argv_array, g_strdup ("-B"));
          g_ptr_array_add (argv_array, g_strdup_printf ("%s=%s", dest, src));
        }
    }

  if (doc_mount_path && app_id)
    {
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("/run/user/%d/doc=%s/by-app/%s",
                                                    getuid(), doc_mount_path, app_id));
    }

  g_hash_table_iter_init (&iter, context->filesystems);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *filesystem = key;
      XdgAppFilesystemMode mode = GPOINTER_TO_INT(value);
      const char *mode_arg;

      if (value == NULL ||
          strcmp (filesystem, "host") == 0 ||
          strcmp (filesystem, "home") == 0)
        continue;

      if (mode == XDG_APP_FILESYSTEM_MODE_READ_WRITE)
        mode_arg = "-B";
      else
        mode_arg = "-b";

      if (g_str_has_prefix (filesystem, "xdg-"))
        {
          const char *path;
          int dir = get_user_dir_from_string (filesystem);

          if (dir < 0)
            {
              g_warning ("Unsupported xdg dir %s\n", filesystem);
              continue;
            }

         path = g_get_user_special_dir (dir);
          if (strcmp (path, g_get_home_dir ()) == 0)
            {
              /* xdg-user-dirs sets disabled dirs to $HOME, and its in general not a good
                 idea to set full access to $HOME other than explicitly, so we ignore
                 these */
              g_debug ("Xdg dir %s is $HOME (i.e. disabled), ignoring\n", filesystem);
              continue;
            }

          if (g_file_test (path, G_FILE_TEST_EXISTS))
            {
              if (xdg_dirs_conf == NULL)
                xdg_dirs_conf = g_string_new ("");

              g_string_append_printf (xdg_dirs_conf, "%s=\"%s\"\n", get_user_dir_config_key (dir), path);

              g_ptr_array_add (argv_array, g_strdup (mode_arg));
              g_ptr_array_add (argv_array, g_strdup_printf ("%s", path));
            }
        }
      else if (g_str_has_prefix (filesystem, "~/"))
        {
          g_autofree char *path = NULL;

          path = g_build_filename (g_get_home_dir(), filesystem+2, NULL);
          if (g_file_test (path, G_FILE_TEST_EXISTS))
            {
              g_ptr_array_add (argv_array, g_strdup (mode_arg));
              g_ptr_array_add (argv_array, g_strdup (path));
            }
        }
      else if (g_str_has_prefix (filesystem, "/"))
        {
          if (g_file_test (filesystem, G_FILE_TEST_EXISTS))
            {
              g_ptr_array_add (argv_array, g_strdup (mode_arg));
              g_ptr_array_add (argv_array, g_strdup_printf ("%s", filesystem));
            }
        }
      else
        g_warning ("Unexpected filesystem arg %s\n", filesystem);
    }

  if (home_access  && app_id_dir != NULL)
    {
      g_autofree char *src_path = g_build_filename (g_get_user_config_dir (),
                                                    "user-dirs.dirs",
                                                    NULL);
      g_autofree char *path = g_build_filename (gs_file_get_path_cached (app_id_dir),
                                                "config/user-dirs.dirs", NULL);
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("%s=%s", path, src_path));
    }
  else if (xdg_dirs_conf != NULL && app_id_dir != NULL)
    {
      g_autofree char *tmp_path = NULL;
      g_autofree char *path = NULL;
      int fd;

      fd = g_file_open_tmp ("xdg-app-user-dir-XXXXXX.dirs", &tmp_path, NULL);
      if (fd >= 0)
        {
          close (fd);
          if (g_file_set_contents (tmp_path, xdg_dirs_conf->str, xdg_dirs_conf->len, NULL))
            {
              path = g_build_filename (gs_file_get_path_cached (app_id_dir),
                                       "config/user-dirs.dirs", NULL);
              g_ptr_array_add (argv_array, g_strdup ("-M"));
              g_ptr_array_add (argv_array, g_strdup_printf ("%s=%s", path, tmp_path));
            }
        }
      g_string_free (xdg_dirs_conf, TRUE);
    }

  if (context->sockets & XDG_APP_CONTEXT_SOCKET_X11)
    {
      g_debug ("Allowing x11 access");
      xdg_app_run_add_x11_args (argv_array);
    }

  if (context->sockets & XDG_APP_CONTEXT_SOCKET_WAYLAND)
    {
      g_debug ("Allowing wayland access");
      xdg_app_run_add_wayland_args (argv_array);
    }

  if (context->sockets & XDG_APP_CONTEXT_SOCKET_PULSEAUDIO)
    {
      g_debug ("Allowing pulseaudio access");
      xdg_app_run_add_pulseaudio_args (argv_array);
    }

  unrestricted_session_bus = (context->sockets & XDG_APP_CONTEXT_SOCKET_SESSION_BUS) != 0;
  if (unrestricted_session_bus)
    g_debug ("Allowing session-dbus access");
  if (xdg_app_run_add_session_dbus_args (argv_array, dbus_proxy_argv, unrestricted_session_bus) &&
      !unrestricted_session_bus && dbus_proxy_argv)
    {
      xdg_app_add_bus_filters (dbus_proxy_argv, app_id, context);
    }

  if (context->sockets & XDG_APP_CONTEXT_SOCKET_SYSTEM_BUS)
    {
      g_debug ("Allowing system-dbus access");
      xdg_app_run_add_system_dbus_args (argv_array, dbus_proxy_argv);
    }

  g_assert (sizeof(opts) > i);
  if (i > 1)
    {
      opts[i++] = 0;
      g_ptr_array_add (argv_array, g_strdup (opts));
    }
}

static const struct {const char *env; const char *val;} default_exports[] = {
  {"PATH","/app/bin:/usr/bin"},
  {"LD_LIBRARY_PATH", ""},
  {"_LD_LIBRARY_PATH", "/app/lib"},
  {"XDG_CONFIG_DIRS","/app/etc/xdg:/etc/xdg"},
  {"XDG_DATA_DIRS","/app/share:/usr/share"},
  {"SHELL","/bin/sh"},
};

static const struct {const char *env; const char *val;} devel_exports[] = {
  {"ACLOCAL_PATH","/app/share/aclocal"},
  {"C_INCLUDE_PATH","/app/include"},
  {"CPLUS_INCLUDE_PATH","/app/include"},
  {"LDFLAGS","-L/app/lib "},
  {"PKG_CONFIG_PATH","/app/lib/pkgconfig:/app/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"},
  {"LC_ALL","en_US.utf8"},
};

char **
xdg_app_run_get_minimal_env (gboolean devel)
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

  for (i = 0; i < G_N_ELEMENTS(default_exports); i++)
    g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", default_exports[i].env, default_exports[i].val));

  if (devel)
    {
      for (i = 0; i < G_N_ELEMENTS(devel_exports); i++)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", devel_exports[i].env, devel_exports[i].val));
    }

  for (i = 0; i < G_N_ELEMENTS(copy); i++)
    {
      const char *current = g_getenv(copy[i]);
      if (current)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy[i], current));
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS(copy_nodevel); i++)
        {
          const char *current = g_getenv(copy_nodevel[i]);
          if (current)
            g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy_nodevel[i], current));
        }
    }

  g_ptr_array_add (env_array, NULL);
  return (char **)g_ptr_array_free (env_array, FALSE);
}

char **
xdg_app_run_apply_env_default (char **envp)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS(default_exports); i++)
    envp = g_environ_setenv (envp, default_exports[i].env, default_exports[i].val, TRUE);

  return envp;
}

char **
xdg_app_run_apply_env_appid (char       **envp,
                             GFile       *app_dir)
{
  g_autoptr(GFile) app_dir_data = NULL;
  g_autoptr(GFile) app_dir_config = NULL;
  g_autoptr(GFile) app_dir_cache = NULL;

  app_dir_data = g_file_get_child (app_dir, "data");
  app_dir_config = g_file_get_child (app_dir, "config");
  app_dir_cache = g_file_get_child (app_dir, "cache");
  envp = g_environ_setenv (envp, "XDG_DATA_HOME", gs_file_get_path_cached (app_dir_data), TRUE);
  envp = g_environ_setenv (envp, "XDG_CONFIG_HOME", gs_file_get_path_cached (app_dir_config), TRUE);
  envp = g_environ_setenv (envp, "XDG_CACHE_HOME", gs_file_get_path_cached (app_dir_cache), TRUE);

  return envp;
}

char **
xdg_app_run_apply_env_vars (char **envp, XdgAppContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *var = key;
      const char *val = value;

      /* We special case LD_LIBRARY_PATH to avoid passing it top
         the helper */
      if (strcmp (var, "LD_LIBRARY_PATH") == 0)
        var = "_LD_LIBRARY_PATH";

      if (val && val[0] != 0)
        envp = g_environ_setenv (envp, var, val, TRUE);
      else
        envp = g_environ_unsetenv (envp, var);
    }

  return envp;
}

GFile *
xdg_app_get_data_dir (const char *app_id)
{
  g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) var_app = g_file_resolve_relative_path (home, ".var/app");

  return g_file_get_child (var_app, app_id);
}

GFile *
xdg_app_ensure_data_dir (const char *app_id,
			 GCancellable  *cancellable,
			 GError **error)
{
  g_autoptr(GFile) dir = xdg_app_get_data_dir (app_id);
  g_autoptr(GFile) data_dir = g_file_get_child (dir, "data");
  g_autoptr(GFile) cache_dir = g_file_get_child (dir, "cache");
  g_autoptr(GFile) config_dir = g_file_get_child (dir, "config");

  if (!gs_file_ensure_directory (data_dir, TRUE, cancellable, error))
    return NULL;

  if (!gs_file_ensure_directory (cache_dir, TRUE, cancellable, error))
    return NULL;

  if (!gs_file_ensure_directory (config_dir, TRUE, cancellable, error))
    return NULL;

  return g_object_ref (dir);
}

struct JobData {
  char *job;
  GMainLoop *main_loop;
};

static void
job_removed_cb (SystemdManager *manager,
                guint32 id,
                char *job,
                char *unit,
                char *result,
                struct JobData *data)
{
  if (strcmp (job, data->job) == 0)
    g_main_loop_quit (data->main_loop);
}

void
xdg_app_run_in_transient_unit (const char *appid)
{
  GDBusConnection *conn = NULL;
  GError *error = NULL;
  char *path = NULL;
  char *address = NULL;
  char *name = NULL;
  char *job = NULL;
  SystemdManager *manager = NULL;
  GVariantBuilder builder;
  GVariant *properties = NULL;
  GVariant *aux = NULL;
  guint32 pid;
  GMainContext *main_context = NULL;
  GMainLoop *main_loop = NULL;
  struct JobData data;

  path = g_strdup_printf ("/run/user/%d/systemd/private", getuid());

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    goto out;

  main_context = g_main_context_new ();
  main_loop = g_main_loop_new (main_context, FALSE);

  g_main_context_push_thread_default (main_context);


  address = g_strconcat ("unix:path=", path, NULL);

  conn = g_dbus_connection_new_for_address_sync (address,
                                                 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                 NULL,
                                                 NULL, &error);
  if (!conn)
    {
      g_warning ("Can't connect to systemd: %s\n", error->message);
      goto out;
    }

  manager = systemd_manager_proxy_new_sync (conn,
                                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                            NULL,
                                            "/org/freedesktop/systemd1",
                                            NULL, &error);
  if (!manager)
    {
      g_warning ("Can't create manager proxy: %s\n", error->message);
      goto out;
    }

  name = g_strdup_printf ("xdg-app-%s-%d.scope", appid, getpid());

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
                                                       &error))
    {
      g_warning ("Can't start transient unit: %s\n", error->message);
      goto out;
    }

  data.job = job;
  data.main_loop = main_loop;
  g_signal_connect (manager,"job-removed", G_CALLBACK (job_removed_cb), &data);

  g_main_loop_run (main_loop);

 out:
  if (main_context)
    {
      g_main_context_pop_thread_default (main_context);
      g_main_context_unref (main_context);
    }
  if (main_loop)
    g_main_loop_unref (main_loop);
  if (error)
    g_error_free (error);
  if (manager)
    g_object_unref (manager);
  if (conn)
    g_object_unref (conn);
  g_free (path);
  g_free (address);
  g_free (job);
  g_free (name);
}

static void
dbus_spawn_child_setup (gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);
  fcntl (fd, F_SETFD, 0);
}

gboolean
xdg_app_run_app (const char *app_ref,
                 XdgAppDeploy *app_deploy,
                 XdgAppContext *extra_context,
                 const char *custom_runtime,
                 XdgAppRunFlags flags,
                 const char *custom_command,
                 char *args[],
                 int n_args,
                 GCancellable *cancellable,
                 GError **error)
{
  g_autoptr(XdgAppDeploy) runtime_deploy = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) app_id_dir = NULL;
  g_autoptr(GFile) app_cache_dir = NULL;
  g_autoptr(GFile) app_data_dir = NULL;
  g_autoptr(GFile) app_config_dir = NULL;
  g_autoptr(GFile) home = NULL;
  g_autoptr(GFile) user_font1 = NULL;
  g_autoptr(GFile) user_font2 = NULL;
  g_autoptr(XdgAppSessionHelper) session_helper = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autofree char *doc_mount_path = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_auto(GStrv) envp = NULL;
  g_autoptr(GPtrArray) dbus_proxy_argv = NULL;
  g_autofree char *monitor_path = NULL;
  const char *command = "/bin/sh";
  int i;
  int sync_proxy_pipes[2];
  g_autoptr(XdgAppContext) app_context = NULL;
  g_autoptr(XdgAppContext) overrides = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_auto(GStrv) app_ref_parts = NULL;

  app_ref_parts = xdg_app_decompose_ref (app_ref, error);
  if (app_ref_parts == NULL)
    return FALSE;

  metakey = xdg_app_deploy_get_metadata (app_deploy);

  argv_array = g_ptr_array_new_with_free_func (g_free);
  dbus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (HELPER));
  g_ptr_array_add (argv_array, g_strdup ("-l"));

  if (!xdg_app_run_add_extension_args (argv_array, metakey, app_ref, cancellable, error))
    return FALSE;

  if (custom_runtime)
    runtime = g_strdup (custom_runtime);
  else
    {
      runtime = g_key_file_get_string (metakey, "Application",
                                       (flags & XDG_APP_RUN_FLAG_DEVEL) != 0 ? "sdk" : "runtime",
                                       error);
      if (*error)
        return FALSE;
    }

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_deploy = xdg_app_find_deploy_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    return FALSE;

  runtime_metakey = xdg_app_deploy_get_metadata (runtime_deploy);

  app_context = xdg_app_context_new ();
  xdg_app_context_set_session_bus_policy (app_context, "org.freedesktop.portal.Documents", XDG_APP_POLICY_TALK);

  if (!xdg_app_context_load_metadata (app_context, runtime_metakey, error))
    return FALSE;
  if (!xdg_app_context_load_metadata (app_context, metakey, error))
    return FALSE;

  overrides = xdg_app_deploy_get_overrides (app_deploy);
  xdg_app_context_merge (app_context, overrides);

  xdg_app_context_merge (app_context, extra_context);

    {
      g_autofree char *tmp_path = NULL;
      int fd;

      fd = g_file_open_tmp ("xdg-app-context-XXXXXX", &tmp_path, NULL);
      if (fd >= 0)
        {
          g_autoptr(GKeyFile) keyfile = NULL;

          close (fd);

          keyfile = g_key_file_new ();

          g_key_file_set_string (keyfile, "Application", "name", app_ref_parts[1]);
          g_key_file_set_string (keyfile, "Application", "runtime", runtime_ref);

          xdg_app_context_save_metadata (app_context, keyfile);

          if (!g_key_file_save_to_file (keyfile, tmp_path, error))
            return FALSE;

          g_ptr_array_add (argv_array, g_strdup ("-M"));
          g_ptr_array_add (argv_array, g_strdup_printf ("/run/user/%d/xdg-app-info=%s", getuid(), tmp_path));
        }
    }

  if (!xdg_app_run_add_extension_args (argv_array, runtime_metakey, runtime_ref, cancellable, error))
    return FALSE;

  if ((app_id_dir = xdg_app_ensure_data_dir (app_ref_parts[1], cancellable, error)) == NULL)
      return FALSE;

  app_cache_dir = g_file_get_child (app_id_dir, "cache");
  g_ptr_array_add (argv_array, g_strdup ("-B"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/var/cache=%s", gs_file_get_path_cached (app_cache_dir)));

  app_data_dir = g_file_get_child (app_id_dir, "data");
  g_ptr_array_add (argv_array, g_strdup ("-B"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/var/data=%s", gs_file_get_path_cached (app_data_dir)));

  app_config_dir = g_file_get_child (app_id_dir, "config");
  g_ptr_array_add (argv_array, g_strdup ("-B"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/var/config=%s", gs_file_get_path_cached (app_config_dir)));

  app_files = xdg_app_deploy_get_files (app_deploy);
  runtime_files = xdg_app_deploy_get_files (runtime_deploy);

  default_command = g_key_file_get_string (metakey, "Application", "command", error);
  if (*error)
    return FALSE;
  if (custom_command)
    command = custom_command;
  else
    command = default_command;

  session_helper =
    xdg_app_session_helper_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                   G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                   "org.freedesktop.XdgApp",
                                                   "/org/freedesktop/XdgApp/SessionHelper",
                                                   NULL, NULL);
  if (session_helper &&
      xdg_app_session_helper_call_request_monitor_sync (session_helper,
                                                        &monitor_path,
                                                        NULL, NULL))
    {
      g_ptr_array_add (argv_array, g_strdup ("-m"));
      g_ptr_array_add (argv_array, monitor_path);
    }
  else
    g_ptr_array_add (argv_array, g_strdup ("-r"));

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session_bus)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GDBusMessage) reply = NULL;
      g_autoptr (GDBusMessage) msg =
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
            g_warning ("Can't get document portal: %s\n", local_error->message);
          else
            g_variant_get (g_dbus_message_get_body (reply),
                           "(^ay)", &doc_mount_path);
        }
    }

  xdg_app_run_add_environment_args (argv_array, dbus_proxy_argv, doc_mount_path,
                                    app_ref_parts[1], app_context, app_id_dir);

  g_ptr_array_add (argv_array, g_strdup ("-b"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/run/host/fonts=%s", SYSTEM_FONTS_DIR));

  if ((flags & XDG_APP_RUN_FLAG_DEVEL) != 0)
    g_ptr_array_add (argv_array, g_strdup ("-c"));

  home = g_file_new_for_path (g_get_home_dir ());
  user_font1 = g_file_resolve_relative_path (home, ".local/share/fonts");
  user_font2 = g_file_resolve_relative_path (home, ".fonts");

  if (g_file_query_exists (user_font1, NULL))
    {
      g_autofree char *path = g_file_get_path (user_font1);
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("/run/host/user-fonts=%s", path));
    }
  else if (g_file_query_exists (user_font2, NULL))
    {
      g_autofree char *path = g_file_get_path (user_font2);
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("/run/host/user-fonts=%s", path));
    }

  /* Must run this before spawning the dbus proxy, to ensure it
     ends up in the app cgroup */
  xdg_app_run_in_transient_unit (app_ref_parts[1]);

  if (dbus_proxy_argv->len > 0)
    {
      char x;

      if (pipe (sync_proxy_pipes) < 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to create sync pipe");
          return FALSE;
        }

      g_ptr_array_insert (dbus_proxy_argv, 0, g_strdup (DBUSPROXY));
      g_ptr_array_insert (dbus_proxy_argv, 1, g_strdup_printf ("--fd=%d", sync_proxy_pipes[1]));

      g_ptr_array_add (dbus_proxy_argv, NULL); /* NULL terminate */

      if (!g_spawn_async (NULL,
                          (char **)dbus_proxy_argv->pdata,
                          NULL,
                          G_SPAWN_SEARCH_PATH,
                          dbus_spawn_child_setup,
                          GINT_TO_POINTER (sync_proxy_pipes[1]),
                          NULL, error))
        return FALSE;

      close (sync_proxy_pipes[1]);

      /* Sync with proxy, i.e. wait until its listening on the sockets */
      if (read (sync_proxy_pipes[0], &x, 1) != 1)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Failed to sync with dbus proxy");
          return FALSE;
        }

      g_ptr_array_add (argv_array, g_strdup ("-S"));
      g_ptr_array_add (argv_array, g_strdup_printf ("%d", sync_proxy_pipes[0]));
    }

  g_ptr_array_add (argv_array, g_strdup ("-a"));
  g_ptr_array_add (argv_array, g_file_get_path (app_files));
  g_ptr_array_add (argv_array, g_strdup ("-I"));
  g_ptr_array_add (argv_array, g_strdup (app_ref_parts[1]));
  g_ptr_array_add (argv_array, g_file_get_path (runtime_files));

  g_ptr_array_add (argv_array, g_strdup (command));
  for (i = 0; i < n_args; i++)
    g_ptr_array_add (argv_array, g_strdup (args[i]));

  g_ptr_array_add (argv_array, NULL);

  envp = g_get_environ ();
  envp = xdg_app_run_apply_env_default (envp);

  envp = xdg_app_run_apply_env_vars (envp, app_context);

  envp = xdg_app_run_apply_env_appid (envp, app_id_dir);

  if ((flags & XDG_APP_RUN_FLAG_BACKGROUND) != 0)
    {
      if (!g_spawn_async (NULL,
                          (char **)argv_array->pdata,
                          envp,
                          G_SPAWN_DEFAULT,
                          NULL, NULL,
                          NULL,
                          error))
        return FALSE;
    }
  else
    {
      if (execvpe (HELPER, (char **)argv_array->pdata, envp) == -1)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start app");
          return FALSE;
        }
      /* Not actually reached... */
    }

  return TRUE;
}
