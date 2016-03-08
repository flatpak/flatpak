#ifndef XDP_UTIL_H
#define XDP_UTIL_H

#include <gio/gio.h>
#include "xdg-app-db.h"
#include "xdp-enums.h"

G_BEGIN_DECLS

#define XDP_ENTRY_FLAG_UNIQUE (1<<0)
#define XDP_ENTRY_FLAG_TRANSIENT (1<<1)

const char **      xdg_unparse_permissions (XdpPermissionFlags   permissions);
XdpPermissionFlags xdp_parse_permissions   (const char         **permissions);

XdpPermissionFlags xdp_entry_get_permissions (XdgAppDbEntry      *entry,
                                              const char         *app_id);
gboolean           xdp_entry_has_permissions (XdgAppDbEntry      *entry,
                                              const char         *app_id,
                                              XdpPermissionFlags  perms);
const char *       xdp_entry_get_path        (XdgAppDbEntry      *entry);
char *             xdp_entry_dup_basename    (XdgAppDbEntry      *entry);
char *             xdp_entry_dup_dirname     (XdgAppDbEntry      *entry);
guint64            xdp_entry_get_device      (XdgAppDbEntry      *entry);
guint64            xdp_entry_get_inode       (XdgAppDbEntry      *entry);
guint32            xdp_entry_get_flags       (XdgAppDbEntry      *entry);

char *  xdp_name_from_id (guint32     doc_id);


G_END_DECLS

#endif /* XDP_UTIL_H */
