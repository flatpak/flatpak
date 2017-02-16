/*
 * Copyright © 2017 Red Hat, Inc
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

#ifndef __BUILDER_POST_PROCESS_H__
#define __BUILDER_POST_PROCESS_H__

#include "builder-cache.h"
#include "builder-context.h"

G_BEGIN_DECLS

typedef enum {
  BUILDER_POST_PROCESS_FLAGS_NONE = 0,
  BUILDER_POST_PROCESS_FLAGS_PYTHON_TIMESTAMPS  = 1<<0,
  BUILDER_POST_PROCESS_FLAGS_STRIP              = 1<<1,
  BUILDER_POST_PROCESS_FLAGS_DEBUGINFO          = 1<<2,
} BuilderPostProcessFlags;

gboolean builder_post_process (BuilderPostProcessFlags   flags,
                               GFile                    *app_dir,
                               BuilderCache             *cache,
                               BuilderContext           *context,
                               GError                  **error);

G_END_DECLS

#endif /* __BUILDER_POST_PROCESS_H__ */
