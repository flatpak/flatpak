#ifndef XDP_FUSE_H
#define XDP_FUSE_H

#include <glib.h>
#include "xdg-app-db.h"

G_BEGIN_DECLS

char **        xdp_list_apps  (void);
guint32 *      xdp_list_docs  (void);
XdgAppDbEntry *xdp_lookup_doc (guint32 id);

gboolean xdp_fuse_init (GError **error);
void xdp_fuse_exit (void);
const char * xdp_fuse_get_mountpoint (void);

G_END_DECLS

#endif /* XDP_FUSE_H */
