#ifndef XDP_FUSE_H
#define XDP_FUSE_H

#include <glib.h>
#include "flatpak-db.h"

G_BEGIN_DECLS

char **        xdp_list_apps (void);
char **        xdp_list_docs (void);
FlatpakDbEntry *xdp_lookup_doc (const char *doc_id);

gboolean    xdp_fuse_init (GError **error);
void        xdp_fuse_exit (void);
const char *xdp_fuse_get_mountpoint (void);
void        xdp_fuse_invalidate_doc_app (const char *doc_id,
                                         const char *opt_app_id);
char      *xdp_fuse_lookup_id_for_inode (ino_t inode);


G_END_DECLS

#endif /* XDP_FUSE_H */
