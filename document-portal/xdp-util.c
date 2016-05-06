#include "config.h"
#include <string.h>
#include <errno.h>
#include <gio/gio.h>
#include "flatpak-portal-error.h"
#include "xdp-util.h"

const char **
xdg_unparse_permissions (XdpPermissionFlags permissions)
{
  GPtrArray *array;

  array = g_ptr_array_new ();

  if (permissions & XDP_PERMISSION_FLAGS_READ)
    g_ptr_array_add (array, "read");
  if (permissions & XDP_PERMISSION_FLAGS_WRITE)
    g_ptr_array_add (array, "write");
  if (permissions & XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS)
    g_ptr_array_add (array, "grant-permissions");
  if (permissions & XDP_PERMISSION_FLAGS_DELETE)
    g_ptr_array_add (array, "delete");

  g_ptr_array_add (array, NULL);
  return (const char **) g_ptr_array_free (array, FALSE);
}

XdpPermissionFlags
xdp_parse_permissions (const char **permissions)
{
  XdpPermissionFlags perms;
  int i;

  perms = 0;
  for (i = 0; permissions[i]; i++)
    {
      if (strcmp (permissions[i], "read") == 0)
        perms |= XDP_PERMISSION_FLAGS_READ;
      else if (strcmp (permissions[i], "write") == 0)
        perms |= XDP_PERMISSION_FLAGS_WRITE;
      else if (strcmp (permissions[i], "grant-permissions") == 0)
        perms |= XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS;
      else if (strcmp (permissions[i], "delete") == 0)
        perms |= XDP_PERMISSION_FLAGS_DELETE;
      else
        g_warning ("No such permission: %s", permissions[i]);
    }

  return perms;
}

XdpPermissionFlags
xdp_entry_get_permissions (FlatpakDbEntry *entry,
                           const char     *app_id)
{
  g_autofree const char **permissions = NULL;

  if (strcmp (app_id, "") == 0)
    return XDP_PERMISSION_FLAGS_ALL;

  permissions = flatpak_db_entry_list_permissions (entry, app_id);
  return xdp_parse_permissions (permissions);
}

gboolean
xdp_entry_has_permissions (FlatpakDbEntry    *entry,
                           const char        *app_id,
                           XdpPermissionFlags perms)
{
  XdpPermissionFlags current_perms;

  current_perms = xdp_entry_get_permissions (entry, app_id);

  return (current_perms & perms) == perms;
}

char *
xdp_name_from_id (guint32 doc_id)
{
  return g_strdup_printf ("%x", doc_id);
}

const char *
xdp_entry_get_path (FlatpakDbEntry *entry)
{
  g_autoptr(GVariant) v = flatpak_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 0);
  return g_variant_get_bytestring (c);
}

char *
xdp_entry_dup_basename (FlatpakDbEntry *entry)
{
  const char *path = xdp_entry_get_path (entry);

  return g_path_get_basename (path);
}

char *
xdp_entry_dup_dirname (FlatpakDbEntry *entry)
{
  const char *path = xdp_entry_get_path (entry);

  return g_path_get_dirname (path);
}

guint64
xdp_entry_get_device (FlatpakDbEntry *entry)
{
  g_autoptr(GVariant) v = flatpak_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 1);
  return g_variant_get_uint64 (c);
}

guint64
xdp_entry_get_inode (FlatpakDbEntry *entry)
{
  g_autoptr(GVariant) v = flatpak_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 2);
  return g_variant_get_uint64 (c);
}

guint32
xdp_entry_get_flags (FlatpakDbEntry *entry)
{
  g_autoptr(GVariant) v = flatpak_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 3);
  return g_variant_get_uint32 (c);
}
