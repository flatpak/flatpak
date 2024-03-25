/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2019 Red Hat, Inc
 * Copyright © 2017 Endless Mobile, Inc.
 * Copyright © 2023 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Philip Withnall <withnall@endlessm.com>
 *       Matthew Leeds <matthew.leeds@endlessm.com>
 */

#include "config.h"
#include "flatpak-locale-utils-private.h"

#include <glib.h>
#include "libglnx.h"

#include "flatpak-utils-private.h"

const char * const *
flatpak_get_locale_categories (void)
{
  /* See locale(7) for these categories */
  static const char * const categories[] = {
    "LANG", "LC_ALL", "LC_MESSAGES", "LC_ADDRESS", "LC_COLLATE", "LC_CTYPE",
    "LC_IDENTIFICATION", "LC_MONETARY", "LC_MEASUREMENT", "LC_NAME", "LC_NUMERIC",
    "LC_PAPER", "LC_TELEPHONE", "LC_TIME",
    NULL
  };

  return categories;
}

char *
flatpak_get_lang_from_locale (const char *locale)
{
  g_autofree char *lang = g_strdup (locale);
  char *c;

  c = strchr (lang, '@');
  if (c != NULL)
    *c = 0;
  c = strchr (lang, '_');
  if (c != NULL)
    *c = 0;
  c = strchr (lang, '.');
  if (c != NULL)
    *c = 0;

  if (strcmp (lang, "C") == 0)
    return NULL;

  return g_steal_pointer (&lang);
}

char **
flatpak_get_current_locale_langs (void)
{
  const char * const *categories = flatpak_get_locale_categories ();
  GPtrArray *langs = g_ptr_array_new ();
  int i;

  for (; categories != NULL && *categories != NULL; categories++)
    {
      const gchar * const *locales = g_get_language_names_with_category (*categories);

      for (i = 0; locales[i] != NULL; i++)
        {
          g_autofree char *lang = flatpak_get_lang_from_locale (locales[i]);
          if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
            g_ptr_array_add (langs, g_steal_pointer (&lang));
        }
    }

  g_ptr_array_sort (langs, flatpak_strcmp0_ptr);
  g_ptr_array_add (langs, NULL);

  return (char **) g_ptr_array_free (langs, FALSE);
}

GDBusProxy *
flatpak_locale_get_localed_dbus_proxy (void)
{
  const char *localed_bus_name = "org.freedesktop.locale1";
  const char *localed_object_path = "/org/freedesktop/locale1";
  const char *localed_interface_name = localed_bus_name;

  return g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                        G_DBUS_PROXY_FLAGS_NONE,
                                        NULL,
                                        localed_bus_name,
                                        localed_object_path,
                                        localed_interface_name,
                                        NULL,
                                        NULL);
}

void
flatpak_get_locale_langs_from_localed_dbus (GDBusProxy *proxy, GPtrArray *langs)
{
  g_autoptr(GVariant) locale_variant = NULL;
  g_autofree const gchar **strv = NULL;
  gsize i, j;

  locale_variant = g_dbus_proxy_get_cached_property (proxy, "Locale");
  if (locale_variant == NULL)
    return;

  strv = g_variant_get_strv (locale_variant, NULL);

  for (i = 0; strv[i]; i++)
    {
      const gchar *locale = NULL;
      g_autofree char *lang = NULL;

      const char * const *categories = flatpak_get_locale_categories ();

      for (j = 0; categories[j]; j++)
        {
          g_autofree char *prefix = g_strdup_printf ("%s=", categories[j]);
          if (g_str_has_prefix (strv[i], prefix))
            {
              locale = strv[i] + strlen (prefix);
              break;
            }
        }

      if (locale == NULL || strcmp (locale, "") == 0)
        continue;

      lang = flatpak_get_lang_from_locale (locale);
      if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
        g_ptr_array_add (langs, g_steal_pointer (&lang));
    }
}

GDBusProxy *
flatpak_locale_get_accounts_dbus_proxy (void)
{
  const char *accounts_bus_name = "org.freedesktop.Accounts";
  const char *accounts_object_path = "/org/freedesktop/Accounts";
  const char *accounts_interface_name = accounts_bus_name;

  return g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                        G_DBUS_PROXY_FLAGS_NONE,
                                        NULL,
                                        accounts_bus_name,
                                        accounts_object_path,
                                        accounts_interface_name,
                                        NULL,
                                        NULL);
}

gboolean
flatpak_get_all_langs_from_accounts_dbus (GDBusProxy *proxy, GPtrArray *langs)
{
  g_auto(GStrv) all_langs = NULL;
  int i;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) error = NULL;

  ret = g_dbus_proxy_call_sync (G_DBUS_PROXY (proxy),
                                "GetUsersLanguages",
                                g_variant_new ("()"),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                &error);
  if (!ret)
    {
      g_debug ("Failed to get languages for all users: %s", error->message);
      return FALSE;
    }

  g_variant_get (ret,
                 "(^as)",
                 &all_langs);

  if (all_langs != NULL)
    {
      for (i = 0; all_langs[i] != NULL; i++)
        {
          g_autofree char *lang = NULL;
            lang = flatpak_get_lang_from_locale (all_langs[i]);
            if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
              g_ptr_array_add (langs, g_steal_pointer (&lang));
        }
    }

  return TRUE;
}

void
flatpak_get_locale_langs_from_accounts_dbus (GDBusProxy *proxy, GPtrArray *langs)
{
  const char *accounts_bus_name = "org.freedesktop.Accounts";
  const char *accounts_interface_name = "org.freedesktop.Accounts.User";
  g_auto(GStrv) object_paths = NULL;
  int i;
  g_autoptr(GVariant) ret = NULL;

  ret = g_dbus_proxy_call_sync (G_DBUS_PROXY (proxy),
                                "ListCachedUsers",
                                g_variant_new ("()"),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                NULL);
  if (ret != NULL)
    g_variant_get (ret,
                   "(^ao)",
                   &object_paths);

  if (object_paths != NULL)
    {
      for (i = 0; object_paths[i] != NULL; i++)
        {
          g_autoptr(GDBusProxy) accounts_proxy = NULL;
          g_autoptr(GVariant) value = NULL;

          accounts_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                          NULL,
                                                          accounts_bus_name,
                                                          object_paths[i],
                                                          accounts_interface_name,
                                                          NULL,
                                                          NULL);

          if (accounts_proxy)
            {
              value = g_dbus_proxy_get_cached_property (accounts_proxy, "Language");
              if (value != NULL)
                {
                  const char *locale = g_variant_get_string (value, NULL);
                  g_autofree char *lang = NULL;

                  if (strcmp (locale, "") == 0)
                    continue; /* This user wants the system default locale */

                  lang = flatpak_get_lang_from_locale (locale);
                  if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
                    g_ptr_array_add (langs, g_steal_pointer (&lang));
                }
            }
        }
    }
}

void
flatpak_get_locale_langs_from_accounts_dbus_for_user (GDBusProxy *proxy, GPtrArray *langs, guint uid)
{
  const char *accounts_bus_name = "org.freedesktop.Accounts";
  const char *accounts_interface_name = "org.freedesktop.Accounts.User";
  g_autofree char *object_path = NULL;
  g_autoptr(GVariant) ret = NULL;

  ret = g_dbus_proxy_call_sync (G_DBUS_PROXY (proxy),
                                "FindUserById",
                                g_variant_new ("(x)", uid),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                NULL);
  if (ret != NULL)
    g_variant_get (ret, "(o)", &object_path);

  if (object_path != NULL)
    {
      g_autoptr(GDBusProxy) accounts_proxy = NULL;
      g_autoptr(GVariant) value = NULL;

      accounts_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      NULL,
                                                      accounts_bus_name,
                                                      object_path,
                                                      accounts_interface_name,
                                                      NULL,
                                                      NULL);
      if (!accounts_proxy)
        return;

      value = g_dbus_proxy_get_cached_property (accounts_proxy, "Languages");
      if (value != NULL)
        {
          g_autofree const char **locales = g_variant_get_strv (value, NULL);
          guint i;

          for (i = 0; locales != NULL && locales[i] != NULL; i++)
            {
              g_autofree char *lang = NULL;
              lang = flatpak_get_lang_from_locale (locales[i]);
              if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
                g_ptr_array_add (langs, g_steal_pointer (&lang));
            }
        }
      else
        {
          value = g_dbus_proxy_get_cached_property (accounts_proxy, "Language");
          if (value != NULL)
            {
              const char *locale = g_variant_get_string (value, NULL);
              g_autofree char *lang = NULL;

              if (strcmp (locale, "") != 0)
                {
                  lang = flatpak_get_lang_from_locale (locale);
                  if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
                    g_ptr_array_add (langs, g_steal_pointer (&lang));
                }
            }
        }
    }
}

const GPtrArray *
flatpak_get_system_locales (void)
{
  static GPtrArray *cached = NULL;

  if (g_once_init_enter (&cached))
    {
      GPtrArray *langs = g_ptr_array_new_with_free_func (g_free);
      g_autoptr(GDBusProxy) accounts_proxy = NULL;
      g_autoptr(GDBusProxy) localed_proxy = NULL;

      /* Get the system default locales */
      localed_proxy = flatpak_locale_get_localed_dbus_proxy ();
      if (localed_proxy != NULL)
        flatpak_get_locale_langs_from_localed_dbus (localed_proxy, langs);

      /* Add user account languages from AccountsService */
      accounts_proxy = flatpak_locale_get_accounts_dbus_proxy ();
      if (accounts_proxy != NULL)
        if (!flatpak_get_all_langs_from_accounts_dbus (accounts_proxy, langs))
          /* If AccountsService is too old for GetUsersLanguages, fall back
           * to retrieving languages for each user account */
          flatpak_get_locale_langs_from_accounts_dbus (accounts_proxy, langs);

      g_ptr_array_add (langs, NULL);

      g_once_init_leave (&cached, langs);
    }

  return (const GPtrArray *)cached;
}

const GPtrArray *
flatpak_get_user_locales (void)
{
  static GPtrArray *cached = NULL;

  if (g_once_init_enter (&cached))
    {
      GPtrArray *langs = g_ptr_array_new_with_free_func (g_free);
      g_autoptr(GDBusProxy) accounts_proxy = NULL;

      accounts_proxy = flatpak_locale_get_accounts_dbus_proxy ();

      if (accounts_proxy != NULL)
        flatpak_get_locale_langs_from_accounts_dbus_for_user (accounts_proxy, langs, getuid ());

      g_ptr_array_add (langs, NULL);

      g_once_init_leave (&cached, langs);
    }

  return (const GPtrArray *)cached;
}
