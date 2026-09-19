#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_APP_DATA_H__
#define __FLATPAK_APP_DATA_H__

#include <gio/gio.h>

G_BEGIN_DECLS

FLATPAK_EXTERN gboolean flatpak_app_data_delete (const char   *app_id,
                                                 GCancellable *cancellable,
                                                 GError      **error);

G_END_DECLS

#endif /* __FLATPAK_APP_DATA_H__ */
