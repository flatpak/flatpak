/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2026 Red Hat, Inc.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

typedef enum _GlnxChaseFlags {
  /* Default */
  GLNX_CHASE_DEFAULT = 0,
  /* Disable triggering of automounts */
  GLNX_CHASE_NO_AUTOMOUNT = 1 << 1,
  /* Do not follow the path's right-most component. When the path's right-most
   * component refers to symlink, return O_PATH fd of the symlink. */
  GLNX_CHASE_NOFOLLOW = 1 << 2,
  /* Do not permit the path resolution to succeed if any component of the
   * resolution is not a descendant of the directory indicated by dirfd. */
  GLNX_CHASE_RESOLVE_BENEATH = 1 << 3,
  /* Symlinks are resolved relative to the given dirfd instead of root. */
  GLNX_CHASE_RESOLVE_IN_ROOT = 1 << 4,
  /* Fail if any symlink is encountered. */
  GLNX_CHASE_RESOLVE_NO_SYMLINKS = 1 << 5,
  /* Fail if the path's right-most component is not a regular file */
  GLNX_CHASE_MUST_BE_REGULAR = 1 << 6,
  /* Fail if the path's right-most component is not a directory */
  GLNX_CHASE_MUST_BE_DIRECTORY = 1 << 7,
  /* Fail if the path's right-most component is not a socket */
  GLNX_CHASE_MUST_BE_SOCKET = 1 << 8,
} GlnxChaseFlags;

/* How many iterations to execute before returning ELOOP */
#define GLNX_CHASE_MAX 32

G_BEGIN_DECLS

int glnx_chaseat (int              dirfd,
                  const char      *path,
                  GlnxChaseFlags   flags,
                  GError         **error);

int glnx_chase_and_statxat (int                 dirfd,
                            const char         *path,
                            GlnxChaseFlags      flags,
                            unsigned int        mask,
                            struct glnx_statx  *statbuf,
                            GError            **error);

G_END_DECLS
