#ifndef __XDG_APP_BUILTINS_H__
#define __XDG_APP_BUILTINS_H__

#include <ostree.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  XDG_APP_BUILTIN_FLAG_NO_USER = 1 << 0,
  XDG_APP_BUILTIN_FLAG_NO_REPO = 1 << 1,
} XdgAppBuiltinFlags;

gboolean xdg_app_option_context_parse (GOptionContext *context,
                                       const GOptionEntry *main_entries,
                                       int *argc,
                                       char ***argv,
                                       XdgAppBuiltinFlags flags,
                                       OstreeRepo **repo,
                                       GFile **basedir,
                                       GCancellable *cancellable,
                                       GError **error);

#define BUILTINPROTO(name) gboolean xdg_app_builtin_ ## name (int argc, char **argv, GCancellable *cancellable, GError **error)

BUILTINPROTO(add_repo);

#undef BUILTINPROTO

G_END_DECLS

#endif /* __XDG_APP_BUILTINS_H__ */
