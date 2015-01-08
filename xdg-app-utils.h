#ifndef __XDG_APP_UTILS_H__
#define __XDG_APP_UTILS_H__

#include <gio/gio.h>

const char * xdg_app_get_arch (void);

char * xdg_app_build_untyped_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * xdg_app_build_runtime_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * xdg_app_build_app_ref (const char *app,
                              const char *branch,
                              const char *arch);
GFile * xdg_app_find_deploy_dir_for_ref (const char *ref,
                                         GCancellable *cancellable,
                                         GError **error);

#endif /* __XDG_APP_UTILS_H__ */
