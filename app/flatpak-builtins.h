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

#include "flatpak-complete.h"
#include "flatpak-utils-private.h"
#include "flatpak-dir-private.h"

G_BEGIN_DECLS

/**
 * FlatpakBuiltinFlags:
 * @FLATPAK_BUILTIN_FLAG_NO_DIR: Don't allow --user/--system/--installation and
 *    don't return any dir
 * @FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO: Don't fail if we can't create an entire installation
 *    directory structure
 * @FLATPAK_BUILTIN_FLAG_ONE_DIR: Allow a single --user/--system/--installation option
 *    and return a single dir. If no option is specified, default to --system
 * @FLATPAK_BUILTIN_FLAG_STANDARD_DIRS: Allow repeated use of --user/--system/--installation
 *    and return multiple dirs. If no option is specified return system(default)+user
 * @FLATPAK_BUILTIN_FLAG_ALL_DIRS: Allow repeated use of --user/--system/--installation
 *    and return multiple dirs. If no option is specified, return all installations,
 *    starting with system(default)+user
 *
 * Flags affecting the behavior of flatpak_option_context_parse().
 *
 * If the default system installation is among the returned directories,
 * it will be returned first.
 */
typedef enum {
  FLATPAK_BUILTIN_FLAG_NO_DIR = 1 << 0,
  FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO = 1 << 1,
  FLATPAK_BUILTIN_FLAG_ONE_DIR = 1 << 2,
  FLATPAK_BUILTIN_FLAG_STANDARD_DIRS = 1 << 3,
  FLATPAK_BUILTIN_FLAG_ALL_DIRS = 1 << 4,
} FlatpakBuiltinFlags;

gboolean flatpak_option_context_parse (GOptionContext     *context,
                                       const GOptionEntry *main_entries,
                                       int                *argc,
                                       char             ***argv,
                                       FlatpakBuiltinFlags flags,
                                       GPtrArray         **out_dirs,
                                       GCancellable       *cancellable,
                                       GError            **error);

extern GOptionEntry user_entries[];
extern GOptionEntry global_entries[];

gboolean usage_error (GOptionContext *context,
                      const char     *message,
                      GError        **error);

#define BUILTINPROTO(name) \
  gboolean flatpak_builtin_ ## name (int argc, char **argv, GCancellable * cancellable, GError **error); \
  gboolean flatpak_complete_ ## name (FlatpakCompletion * completion);


BUILTINPROTO (remote_add)
BUILTINPROTO (remote_modify)
BUILTINPROTO (remote_delete)
BUILTINPROTO (remote_ls)
BUILTINPROTO (remote_info)
BUILTINPROTO (remote_list)
BUILTINPROTO (install)
BUILTINPROTO (mask)
BUILTINPROTO (pin)
BUILTINPROTO (update)
BUILTINPROTO (make_current_app)
BUILTINPROTO (uninstall)
BUILTINPROTO (install_bundle)
BUILTINPROTO (list)
BUILTINPROTO (info)
BUILTINPROTO (run)
BUILTINPROTO (enter)
BUILTINPROTO (ps)
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
BUILTINPROTO (permission_remove)
BUILTINPROTO (permission_set)
BUILTINPROTO (permission_list)
BUILTINPROTO (permission_show)
BUILTINPROTO (permission_reset)
BUILTINPROTO (override)
BUILTINPROTO (repo)
BUILTINPROTO (config)
BUILTINPROTO (search)
BUILTINPROTO (repair)
BUILTINPROTO (create_usb)
BUILTINPROTO (kill)
BUILTINPROTO (history)

#undef BUILTINPROTO

G_END_DECLS

#endif /* __FLATPAK_BUILTINS_H__ */
