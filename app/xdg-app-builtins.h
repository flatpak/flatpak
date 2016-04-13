/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

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

gboolean usage_error (GOptionContext *context,
                      const char *message,
                      GError **error);

#define BUILTINPROTO(name) gboolean xdg_app_builtin_ ## name (int argc, char **argv, GCancellable *cancellable, GError **error)

BUILTINPROTO(add_remote);
BUILTINPROTO(modify_remote);
BUILTINPROTO(delete_remote);
BUILTINPROTO(ls_remote);
BUILTINPROTO(list_remotes);
BUILTINPROTO(install);
BUILTINPROTO(update);
BUILTINPROTO(make_current_app);
BUILTINPROTO(uninstall);
BUILTINPROTO(install_bundle);
BUILTINPROTO(list);
BUILTINPROTO(info);
BUILTINPROTO(run);
BUILTINPROTO(enter);
BUILTINPROTO(build_init);
BUILTINPROTO(build);
BUILTINPROTO(build_finish);
BUILTINPROTO(build_sign);
BUILTINPROTO(build_export);
BUILTINPROTO(build_bundle);
BUILTINPROTO(build_update_repo);
BUILTINPROTO(export_file);
BUILTINPROTO(override);

/* Deprecated */
BUILTINPROTO(install_runtime);
BUILTINPROTO(install_app);
BUILTINPROTO(update_runtime);
BUILTINPROTO(update_app);
BUILTINPROTO(uninstall_runtime);
BUILTINPROTO(uninstall_app);
BUILTINPROTO(list_apps);
BUILTINPROTO(list_runtimes);

#undef BUILTINPROTO

G_END_DECLS

#endif /* __XDG_APP_BUILTINS_H__ */
