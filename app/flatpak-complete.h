/*
 * Copyright © 2018 Red Hat, Inc
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

#ifndef __FLATPAK_COMPLETE_H__
#define __FLATPAK_COMPLETE_H__

#include <ostree.h>
#include "flatpak-dir-private.h"
#include "flatpak-builtins-utils.h"

typedef struct FlatpakCompletion FlatpakCompletion;

struct FlatpakCompletion
{
  char  *shell_cur;
  char  *cur;
  char  *prev;
  char  *line;
  int    point;
  char **argv;
  char **original_argv;
  int    argc;
  int    original_argc;
};

void flatpak_completion_debug (const gchar *format,
                               ...) G_GNUC_PRINTF (1, 2);

FlatpakCompletion *flatpak_completion_new (const char *arg_line,
                                           const char *arg_point,
                                           const char *arg_cur);
void               flatpak_complete_word (FlatpakCompletion *completion,
                                          const char        *format,
                                          ...) G_GNUC_PRINTF (2, 3);
void               flatpak_complete_ref (FlatpakCompletion *completion,
                                         OstreeRepo        *repo);
void               flatpak_complete_ref_id (FlatpakCompletion *completion,
                                            GPtrArray         *refs);
void               flatpak_complete_ref_branch (FlatpakCompletion *completion,
                                                GPtrArray         *refs);
void               flatpak_complete_partial_ref (FlatpakCompletion *completion,
                                                 FlatpakKinds       kinds,
                                                 const char        *only_arch,
                                                 FlatpakDir        *dir,
                                                 const char        *remote);
void               flatpak_complete_file (FlatpakCompletion *completion,
                                          const char        *file_type);
void               flatpak_complete_dir (FlatpakCompletion *completion);
void               flatpak_complete_options (FlatpakCompletion *completion,
                                             GOptionEntry      *entries);
void               flatpak_complete_columns (FlatpakCompletion *completion,
                                             Column            *columns);
void               flatpak_completion_free (FlatpakCompletion *completion);
void               flatpak_complete_context (FlatpakCompletion *completion);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakCompletion, flatpak_completion_free)

#endif /* __FLATPAK_COMPLETE_H__ */
