#ifndef XDP_UTIL_H
#define XDP_UTIL_H

#include <gio/gio.h>

G_BEGIN_DECLS

void  xdp_invocation_lookup_app_id        (GDBusMethodInvocation  *invocation,
                                           GCancellable           *cancellable,
                                           GAsyncReadyCallback     callback,
                                           gpointer                user_data);

char *xdp_invocation_lookup_app_id_finish (GDBusMethodInvocation  *invocation,
                                           GAsyncResult           *result,
                                           GError                **error);

void  xdp_connection_track_name_owners    (GDBusConnection        *connection);

G_END_DECLS

#endif /* XDP_UTIL_H */
