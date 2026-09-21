#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_USER_DATA_UTILS_H__
#define __FLATPAK_USER_DATA_UTILS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

FLATPAK_EXTERN gboolean flatpak_user_data_delete (const char   *app_id,
                                                  GCancellable *cancellable,
                                                  GError      **error);

G_END_DECLS

#endif /* __FLATPAK_USER_DATA_UTILS_H__ */
