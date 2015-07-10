#ifndef XDP_FUSE_H
#define XDP_FUSE_H

#include <glib.h>
#include "xdp-doc-db.h"

G_BEGIN_DECLS

gboolean xdp_fuse_init (XdpDocDb *db,
			GError **error);
void xdp_fuse_exit (void);
const char * xdp_fuse_get_mountpoint (void);

G_END_DECLS

#endif /* XDP_FUSE_H */
