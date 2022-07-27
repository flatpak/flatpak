/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "portal-impl.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

static void
portal_implementation_free (PortalImplementation *impl)
{
  g_free (impl->source);
  g_free (impl->dbus_name);
  g_strfreev (impl->interfaces);
  g_strfreev (impl->use_in);
  g_free (impl);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortalImplementation, portal_implementation_free)

static GList *implementations = NULL;

static gboolean
register_portal (const char *path, gboolean opt_verbose, GError **error)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(PortalImplementation) impl = g_new0 (PortalImplementation, 1);
  int i;

  g_debug ("loading %s", path);

  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    return FALSE;

  impl->source = g_path_get_basename (path);
  impl->dbus_name = g_key_file_get_string (keyfile, "portal", "DBusName", error);
  if (impl->dbus_name == NULL)
    return FALSE;
  if (!g_dbus_is_name (impl->dbus_name))
    {
      g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                   "Not a valid bus name: %s", impl->dbus_name);
      return FALSE;
    }

  impl->interfaces = g_key_file_get_string_list (keyfile, "portal", "Interfaces", NULL, error);
  if (impl->interfaces == NULL)
    return FALSE;
  for (i = 0; impl->interfaces[i]; i++)
    {
      if (!g_dbus_is_interface_name (impl->interfaces[i]))
        {
          g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                       "Not a valid interface name: %s", impl->interfaces[i]);
          return FALSE;
        }
      if (!g_str_has_prefix (impl->interfaces[i], "org.freedesktop.impl.portal."))
        {
          g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                       "Not a portal backend interface: %s", impl->interfaces[i]);
          return FALSE;
        }
    }

  impl->use_in = g_key_file_get_string_list (keyfile, "portal", "UseIn", NULL, error);
  if (impl->use_in == NULL)
    return FALSE;

  if (opt_verbose)
    {
      g_autofree char *uses = g_strjoinv (", ", impl->use_in);
      g_debug ("portal implementation for %s", uses);
      for (i = 0; impl->interfaces[i]; i++)
        g_debug ("portal implementation supports %s", impl->interfaces[i]);
    }

  implementations = g_list_prepend (implementations, impl);
  impl = NULL;

  return TRUE;
}

static gint
sort_impl_by_name (gconstpointer a,
                   gconstpointer b)
{
  const PortalImplementation *pa = a;
  const PortalImplementation *pb = b;

  return strcmp (pa->source, pb->source);
}

void
load_installed_portals (gboolean opt_verbose)
{
  const char *portal_dir;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) enumerator = NULL;

  /* We need to override this in the tests */
  portal_dir = g_getenv ("XDG_DESKTOP_PORTAL_DIR");
  if (portal_dir == NULL)
    portal_dir = DATADIR "/xdg-desktop-portal/portals";

  g_debug ("load portals from %s", portal_dir);

  dir = g_file_new_for_path (portal_dir);
  enumerator = g_file_enumerate_children (dir, "*", G_FILE_QUERY_INFO_NONE, NULL, NULL);

  if (enumerator == NULL)
    return;

  while (TRUE)
    {
      g_autoptr(GFileInfo) info = g_file_enumerator_next_file (enumerator, NULL, NULL);
      g_autoptr(GFile) child = NULL;
      g_autofree char *path = NULL;
      const char *name;
      g_autoptr(GError) error = NULL;

      if (info == NULL)
        break;

      name = g_file_info_get_name (info);

      if (!g_str_has_suffix (name, ".portal"))
        continue;

      child = g_file_enumerator_get_child (enumerator, info);
      path = g_file_get_path (child);

      if (!register_portal (path, opt_verbose, &error))
        {
          g_warning ("Error loading %s: %s", path, error->message);
          continue;
        }
    }

  implementations = g_list_sort (implementations, sort_impl_by_name);
}

static gboolean
g_strv_case_contains (const gchar * const *strv,
                      const gchar         *str)
{
  for (; *strv != NULL; strv++)
    {
      if (g_ascii_strcasecmp (str, *strv) == 0)
        return TRUE;
    }

  return FALSE;
}

PortalImplementation *
find_portal_implementation (const char *interface)
{
  const char *desktops_str = g_getenv ("XDG_CURRENT_DESKTOP");
  g_auto(GStrv) desktops = NULL;
  int i;
  GList *l;

  if (desktops_str == NULL)
    desktops_str = "";

  desktops = g_strsplit (desktops_str, ":", -1);

  for (i = 0; desktops[i] != NULL; i++)
    {
     for (l = implementations; l != NULL; l = l->next)
        {
          PortalImplementation *impl = l->data;

          if (!g_strv_contains ((const char **)impl->interfaces, interface))
            continue;

          if (g_strv_case_contains ((const char **)impl->use_in, desktops[i]))
            {
              g_debug ("Using %s for %s in %s", impl->source, interface, desktops[i]);
              return impl;
            }
        }
    }

  /* Fall back to *any* installed implementation */
  for (l = implementations; l != NULL; l = l->next)
    {
      PortalImplementation *impl = l->data;

      if (!g_strv_contains ((const char **)impl->interfaces, interface))
        continue;

      g_debug ("Falling back to %s for %s", impl->source, interface);
      return impl;
    }

  return NULL;
}
