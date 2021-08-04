/*
 * Copyright © 2019 Red Hat, Inc
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

#include <glib/gi18n-lib.h>

#include "flatpak-dir-private.h"
#include "flatpak-auth-private.h"
#include "flatpak-utils-private.h"

FlatpakAuthenticator *
flatpak_auth_new_for_remote (FlatpakDir *dir,
                             const char *remote,
                             GCancellable *cancellable,
                             GError **error)
{
  g_autofree char *name = NULL;
  g_autoptr(AutoFlatpakAuthenticator) authenticator = NULL;
  g_autoptr(GVariant) auth_options = NULL;
  g_auto(GStrv) keys = NULL;
  g_autoptr(GVariantBuilder) auth_options_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  OstreeRepo *repo;
  int i;

  if (!flatpak_dir_ensure_repo (dir, cancellable, error))
    return FALSE;

  repo = flatpak_dir_get_repo (dir);
  if (repo != NULL)
    {
      if (!ostree_repo_get_remote_option (repo, remote, FLATPAK_REMOTE_CONFIG_AUTHENTICATOR_NAME, NULL, &name, error))
        return NULL;
    }

  if (name == NULL && flatpak_dir_get_remote_oci (dir, remote))
    name = g_strdup ("org.flatpak.Authenticator.Oci");

  if (name == NULL || *name == 0 /* or if no repo */)
    {
      flatpak_fail (error, _("No authenticator configured for remote `%s`"), remote);
      return NULL;
    }

  keys = flatpak_dir_list_remote_config_keys (dir, remote);

  for (i = 0; keys != NULL && keys[i] != NULL; i++)
    {
      const char *key = keys[i];
      const char *key_suffix;
      g_autofree char *value = NULL;

      if (!g_str_has_prefix (key, FLATPAK_REMOTE_CONFIG_AUTHENTICATOR_OPTIONS_PREFIX))
        continue;

      key_suffix = key + strlen(FLATPAK_REMOTE_CONFIG_AUTHENTICATOR_OPTIONS_PREFIX);
      if (key_suffix[0] == 0)
        continue;

      if (!ostree_repo_get_remote_option (repo, remote, key, NULL, &value, error))
        return NULL;

      g_variant_builder_add (auth_options_builder, "{sv}", key_suffix, g_variant_new_string(value));
    }

  auth_options = g_variant_ref_sink (g_variant_builder_end (auth_options_builder));

  authenticator = flatpak_authenticator_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                                name,
                                                                FLATPAK_AUTHENTICATOR_OBJECT_PATH,
                                                                cancellable, error);
  if (authenticator == NULL)
    return NULL;

  g_object_set_data_full (G_OBJECT (authenticator), "authenticator-options", g_steal_pointer (&auth_options), (GDestroyNotify)g_variant_unref);
  return g_steal_pointer (&authenticator);
}

char *
flatpak_auth_create_request_path (const char *peer,
                                  const char *token,
                                  GError **error)
{
  g_autofree gchar *escaped_peer = NULL;
  int i;

  for (i = 0; token[i]; i++)
    {
      if (!g_ascii_isalnum (token[i]) && token[i] != '_')
        {
          flatpak_fail (error, _("Invalid token %s"), token);
          return NULL;
        }
    }

  escaped_peer = g_strdup (peer + 1);
  for (i = 0; escaped_peer[i]; i++)
    if (escaped_peer[i] == '.')
      escaped_peer[i] = '_';

  return g_strconcat (FLATPAK_AUTHENTICATOR_REQUEST_OBJECT_PATH_PREFIX, escaped_peer, "/", token, NULL);
}

FlatpakAuthenticatorRequest *
flatpak_auth_create_request (FlatpakAuthenticator *authenticator,
                             GCancellable *cancellable,
                             GError **error)
{
  static int next_token = 0;
  g_autofree char *request_path = NULL;
  GDBusConnection *bus;
  FlatpakAuthenticatorRequest *request;
  g_autofree char *token = NULL;

  token = g_strdup_printf ("%d", ++next_token);
  bus = g_dbus_proxy_get_connection (G_DBUS_PROXY (authenticator));

  request_path = flatpak_auth_create_request_path (g_dbus_connection_get_unique_name (bus), token, error);
  if (request_path == NULL)
    return NULL;

  request = flatpak_authenticator_request_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (authenticator)),
                                                                  request_path,
                                                                  cancellable, error);
  if (request == NULL)
    return NULL;

  return request;
}

gboolean
flatpak_auth_request_ref_tokens (FlatpakAuthenticator *authenticator,
                                 FlatpakAuthenticatorRequest *request,
                                 const char *remote,
                                 const char *remote_uri,
                                 GVariant *refs,
                                 GVariant *options,
                                 const char *parent_window,
                                 GCancellable *cancellable,
                                 GError **error)
{
  const char *token;
  GVariant *auth_options;
  g_autofree char *handle = NULL;

  token = strrchr (g_dbus_proxy_get_object_path (G_DBUS_PROXY (request)), '/') + 1;

  auth_options = g_object_get_data (G_OBJECT (authenticator), "authenticator-options");

  if (!flatpak_authenticator_call_request_ref_tokens_sync (authenticator, token, auth_options, remote, remote_uri, refs, options,
                                                           parent_window ? parent_window : "",
                                                           &handle, cancellable, error))
    return FALSE;

  if (strcmp (g_dbus_proxy_get_object_path (G_DBUS_PROXY (request)), handle) !=0)
    {
      /* This shouldn't happen, as it would be a broken authenticator, but lets validate it */
      flatpak_fail (error, _("Authenticator returned wrong handle"));
      return FALSE;
    }

  return TRUE;
}
