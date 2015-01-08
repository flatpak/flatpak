#ifndef __XDG_APP_BUILTINS_H__
#define __XDG_APP_BUILTINS_H__

#include <ostree.h>
#include <gio/gio.h>

#include "xdg-app-dir.h"

G_BEGIN_DECLS

typedef enum {
  XDG_APP_BUILTIN_FLAG_NO_DIR = 1 << 0,
  XDG_APP_BUILTIN_FLAG_NO_REPO = 1 << 1,
} XdgAppBuiltinFlags;

gboolean xdg_app_option_context_parse (GOptionContext *context,
                                       const GOptionEntry *main_entries,
                                       int *argc,
                                       char ***argv,
                                       XdgAppBuiltinFlags flags,
                                       XdgAppDir **out_dir,
                                       GCancellable *cancellable,
                                       GError **error);

#define BUILTINPROTO(name) gboolean xdg_app_builtin_ ## name (int argc, char **argv, GCancellable *cancellable, GError **error)

BUILTINPROTO(add_repo);
BUILTINPROTO(install_runtime);
BUILTINPROTO(update_runtime);
BUILTINPROTO(install_app);
BUILTINPROTO(update_app);
BUILTINPROTO(run);
BUILTINPROTO(build_init);

#undef BUILTINPROTO

G_END_DECLS

#endif /* __XDG_APP_BUILTINS_H__ */
