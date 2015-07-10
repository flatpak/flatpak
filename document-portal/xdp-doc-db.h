#ifndef XDP_DB
#define XDP_DB

#include <glib-object.h>

#include "xdp-enums.h"

G_BEGIN_DECLS

#define XDP_TYPE_DOC_DB (xdp_doc_db_get_type())

G_DECLARE_FINAL_TYPE(XdpDocDb, xdp_doc_db, XDP, DOC_DB, GObject);

XdpDocDb *         xdp_doc_db_new             (const char          *filename,
                                               GError             **error);
gboolean           xdp_doc_db_save            (XdpDocDb            *db,
                                               GError             **error);
gboolean           xdp_doc_db_is_dirty        (XdpDocDb            *db);
void               xdp_doc_db_dump            (XdpDocDb            *db);
GVariant *         xdp_doc_db_lookup_doc_name (XdpDocDb            *db,
                                               const char          *doc_name);
GVariant *         xdp_doc_db_lookup_doc      (XdpDocDb            *db,
                                               guint32              doc_id);
GVariant *         xdp_doc_db_lookup_app      (XdpDocDb            *db,
                                               const char          *app_id);
GVariant *         xdp_doc_db_lookup_uri      (XdpDocDb            *db,
                                               const char          *uri);
guint32*           xdp_doc_db_list_docs       (XdpDocDb            *db);
char **            xdp_doc_db_list_apps       (XdpDocDb            *db);
char **            xdp_doc_db_list_uris       (XdpDocDb            *db);
guint32            xdp_doc_db_create_doc      (XdpDocDb            *db,
                                               const char          *uri);
gboolean           xdp_doc_db_delete_doc      (XdpDocDb            *db,
                                               guint32              doc_id);
gboolean           xdp_doc_db_set_permissions (XdpDocDb            *db,
                                               guint32              doc_id,
                                               const char          *app_id,
                                               XdpPermissionFlags   permissions,
                                               gboolean             add);

XdpPermissionFlags xdp_doc_get_permissions    (GVariant            *doc,
                                               const char          *app_id);
gboolean           xdp_doc_has_permissions    (GVariant            *doc,
                                               const char          *app_id,
                                               XdpPermissionFlags   permissions);
guint32            xdb_doc_id_from_name       (const char          *name);
char *             xdb_doc_name_from_id       (guint32              doc_id);
const char *       xdp_doc_get_uri            (GVariant            *doc);
char *             xdp_doc_dup_path           (GVariant            *doc);
char *             xdp_doc_dup_basename       (GVariant            *doc);
char *             xdp_doc_dup_dirname        (GVariant            *doc);

G_END_DECLS

#endif /* XDP_DB */
