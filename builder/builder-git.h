/*
 * Copyright © 2015 Red Hat, Inc
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

#ifndef __BUILDER_GIT_H__
#define __BUILDER_GIT_H__

#include "builder-context.h"

G_BEGIN_DECLS

gboolean builder_git_mirror_repo        (const char      *repo_location,
                                         gboolean         update,
                                         gboolean         mirror_submodules,
                                         gboolean         disable_fsck,
                                         const char      *ref,
                                         BuilderContext  *context,
                                         GError         **error);
char *   builder_git_get_current_commit (const char      *repo_location,
                                         const char      *branch,
                                         BuilderContext  *context,
                                         GError         **error);
gboolean builder_git_checkout           (const char      *repo_location,
                                         const char      *branch,
                                         GFile           *dest,
                                         BuilderContext  *context,
                                         GError         **error);
gboolean builder_git_checkout_dir       (const char      *repo_location,
                                         const char      *branch,
                                         const char      *dir,
                                         GFile           *dest,
                                         BuilderContext  *context,
                                         GError         **error);

G_END_DECLS

#endif /* __BUILDER_GIT_H__ */
