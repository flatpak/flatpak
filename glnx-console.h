/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014,2015 Colin Walters <walters@verbum.org>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <glnx-backport-autocleanups.h>

G_BEGIN_DECLS

struct GLnxConsoleRef {
  gboolean locked;
  gboolean is_tty;
};

typedef struct GLnxConsoleRef GLnxConsoleRef;

gboolean glnx_stdout_is_tty (void);

void	 glnx_console_lock (GLnxConsoleRef *ref);

void	 glnx_console_text (const char     *text);

void	 glnx_console_progress_text_percent (const char     *text,
                                           guint           percentage);

void	 glnx_console_progress_n_items (const char     *text,
                                      guint           current,
                                      guint           total);

void	 glnx_console_unlock (GLnxConsoleRef *ref);

guint    glnx_console_lines (void);

guint    glnx_console_columns (void);

static inline void
glnx_console_ref_cleanup (GLnxConsoleRef *p)
{
  if (p->locked)
    glnx_console_unlock (p);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GLnxConsoleRef, glnx_console_ref_cleanup)

G_END_DECLS
