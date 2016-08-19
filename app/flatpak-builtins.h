/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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

#ifndef __FLATPAK_BUILTINS_H__
#define __FLATPAK_BUILTINS_H__

#include <ostree.h>
#include <gio/gio.h>

#include "flatpak-utils.h"
#include "flatpak-dir.h"

G_BEGIN_DECLS

typedef enum {
  FLATPAK_BUILTIN_FLAG_NO_DIR = 1 << 0,
  FLATPAK_BUILTIN_FLAG_NO_REPO = 1 << 1,
} FlatpakBuiltinFlags;

gboolean flatpak_option_context_parse (GOptionContext     *context,
                                       const GOptionEntry *main_entries,
                                       int                *argc,
                                       char             ***argv,
                                       FlatpakBuiltinFlags flags,
                                       FlatpakDir        **out_dir,
                                       GCancellable       *cancellable,
                                       GError            **error);

extern GOptionEntry user_entries[];
extern GOptionEntry global_entries[];

gboolean usage_error (GOptionContext *context,
                      const char     *message,
                      GError        **error);

#define BUILTINPROTO(name) \
  gboolean flatpak_builtin_ ## name (int argc, char **argv, GCancellable * cancellable, GError * *error); \
  gboolean flatpak_complete_ ## name (FlatpakCompletion *completion);


BUILTINPROTO (add_remote)
BUILTINPROTO (modify_remote)
BUILTINPROTO (delete_remote)
BUILTINPROTO (ls_remote)
BUILTINPROTO (list_remotes)
BUILTINPROTO (install)
BUILTINPROTO (update)
BUILTINPROTO (make_current_app)
BUILTINPROTO (uninstall)
BUILTINPROTO (install_bundle)
BUILTINPROTO (list)
BUILTINPROTO (info)
BUILTINPROTO (run)
BUILTINPROTO (enter)
BUILTINPROTO (build_init)
BUILTINPROTO (build)
BUILTINPROTO (build_finish)
BUILTINPROTO (build_sign)
BUILTINPROTO (build_export)
BUILTINPROTO (build_bundle)
BUILTINPROTO (build_import)
BUILTINPROTO (build_commit_from)
BUILTINPROTO (build_update_repo)
BUILTINPROTO (document_export)
BUILTINPROTO (document_unexport)
BUILTINPROTO (document_info)
BUILTINPROTO (document_list)
BUILTINPROTO (override)

#undef BUILTINPROTO

G_END_DECLS

#endif /* __FLATPAK_BUILTINS_H__ */
