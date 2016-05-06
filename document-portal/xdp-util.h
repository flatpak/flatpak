#ifndef XDP_UTIL_H
#define XDP_UTIL_H

#include <gio/gio.h>
#include "flatpak-db.h"
#include "xdp-enums.h"

G_BEGIN_DECLS

#define XDP_ENTRY_FLAG_UNIQUE (1 << 0)
#define XDP_ENTRY_FLAG_TRANSIENT (1 << 1)

const char **      xdg_unparse_permissions (XdpPermissionFlags permissions);
XdpPermissionFlags xdp_parse_permissions (const char **permissions);

XdpPermissionFlags xdp_entry_get_permissions (FlatpakDbEntry *entry,
                                              const char     *app_id);
gboolean           xdp_entry_has_permissions (FlatpakDbEntry    *entry,
                                              const char        *app_id,
                                              XdpPermissionFlags perms);
const char *       xdp_entry_get_path (FlatpakDbEntry *entry);
char *             xdp_entry_dup_basename (FlatpakDbEntry *entry);
char *             xdp_entry_dup_dirname (FlatpakDbEntry *entry);
guint64            xdp_entry_get_device (FlatpakDbEntry *entry);
guint64            xdp_entry_get_inode (FlatpakDbEntry *entry);
guint32            xdp_entry_get_flags (FlatpakDbEntry *entry);

char *  xdp_name_from_id (guint32 doc_id);


G_END_DECLS

#endif /* XDP_UTIL_H */
