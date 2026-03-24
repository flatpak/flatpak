/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2026 Red Hat, Inc.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * glnx_chaseat was inspired by systemd's chase
 */

#include "libglnx-config.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <glnx-backports.h>
#include <glnx-errors.h>
#include <glnx-fdio.h>
#include <glnx-local-alloc.h>
#include <glnx-missing.h>

#include <glnx-chase.h>

#define AUTOFS_SUPER_MAGIC 0x0187 /* man fstatfs */

#define GLNX_CHASE_DEBUG_NO_OPENAT2 (1U << 31)
#define GLNX_CHASE_DEBUG_NO_OPEN_TREE (1U << 30)

#define GLNX_CHASE_ALL_DEBUG_FLAGS \
  (GLNX_CHASE_DEBUG_NO_OPENAT2 | \
   GLNX_CHASE_DEBUG_NO_OPEN_TREE)

#define GLNX_CHASE_ALL_REGULAR_FLAGS \
  (GLNX_CHASE_NO_AUTOMOUNT | \
   GLNX_CHASE_NOFOLLOW | \
   GLNX_CHASE_RESOLVE_BENEATH | \
   GLNX_CHASE_RESOLVE_IN_ROOT | \
   GLNX_CHASE_RESOLVE_NO_SYMLINKS | \
   GLNX_CHASE_MUST_BE_REGULAR | \
   GLNX_CHASE_MUST_BE_DIRECTORY | \
   GLNX_CHASE_MUST_BE_SOCKET)

#define GLNX_CHASE_ALL_FLAGS \
  (GLNX_CHASE_ALL_DEBUG_FLAGS | GLNX_CHASE_ALL_REGULAR_FLAGS)

typedef GQueue GlnxStatxQueue;

static void
glnx_statx_queue_push (GlnxStatxQueue          *queue,
                       const struct glnx_statx *st)
{
  struct glnx_statx *copy;

  copy = g_memdup2 (st, sizeof (*st));
  g_queue_push_tail (queue, copy);
}

static void
glnx_statx_queue_free_element (gpointer element,
                               G_GNUC_UNUSED gpointer userdata)
{
  g_free (element);
}

static void
glnx_statx_queue_free (GlnxStatxQueue *squeue)
{
  GQueue *queue = (GQueue *) squeue;

  /* Same as g_queue_clear_full (queue, g_free), but works for <2.60 */
  g_queue_foreach (queue, glnx_statx_queue_free_element, NULL);
  g_queue_clear (queue);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GlnxStatxQueue, glnx_statx_queue_free)

static gboolean
glnx_statx_inode_same (const struct glnx_statx *a,
                       const struct glnx_statx *b)
{
  g_assert ((a->stx_mask & (GLNX_STATX_TYPE | GLNX_STATX_INO)) ==
            (GLNX_STATX_TYPE | GLNX_STATX_INO));
  g_assert ((b->stx_mask & (GLNX_STATX_TYPE | GLNX_STATX_INO)) ==
            (GLNX_STATX_TYPE | GLNX_STATX_INO));

  return ((a->stx_mode ^ b->stx_mode) & S_IFMT) == 0 &&
         a->stx_dev_major == b->stx_dev_major &&
         a->stx_dev_minor == b->stx_dev_minor &&
         a->stx_ino == b->stx_ino;
}

static gboolean
glnx_statx_mount_same (const struct glnx_statx *a,
                       const struct glnx_statx *b)
{
  g_assert ((a->stx_mask & (GLNX_STATX_MNT_ID | GLNX_STATX_MNT_ID_UNIQUE)) != 0);
  g_assert ((b->stx_mask & (GLNX_STATX_MNT_ID | GLNX_STATX_MNT_ID_UNIQUE)) != 0);

  return a->stx_mnt_id == b->stx_mnt_id;
}

static gboolean
glnx_chase_statx (int                 dfd,
                  int                 additional_flags,
                  struct glnx_statx  *buf,
                  GError            **error)
{
  if (!glnx_statx (dfd, "",
                   AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | additional_flags,
                   GLNX_STATX_TYPE | GLNX_STATX_INO |
                   GLNX_STATX_MNT_ID | GLNX_STATX_MNT_ID_UNIQUE,
                   buf,
                   error))
    return FALSE;

  if ((buf->stx_mask & (GLNX_STATX_TYPE | GLNX_STATX_INO)) !=
        (GLNX_STATX_TYPE | GLNX_STATX_INO) ||
      (buf->stx_mask & (GLNX_STATX_MNT_ID | GLNX_STATX_MNT_ID_UNIQUE)) == 0)
    {
      errno = ENODATA;
      return glnx_throw_errno_prefix (error,
                                      "statx didn't return all required fields");
    }

  return TRUE;
}

/* TODO: procfs magiclinks handling */

/* open_tree subset which transparently falls back to openat.
 *
 * Returned fd is always OPATH and CLOEXEC.
 *
 * With NO_AUTOMOUNT this function never triggers automounts. Otherwise, it only
 * guarantees to trigger an automount which is on last segment of the path!
 *
 * flags can be a combinations of:
 *  - GLNX_CHASE_NO_AUTOMOUNT
 *  - GLNX_CHASE_NOFOLLOW
 */
static int
chase_open_tree (int              dirfd,
                 const char      *path,
                 GlnxChaseFlags   flags,
                 GError         **error)
{
  glnx_autofd int fd = -1;
  static gboolean can_open_tree = TRUE;
  unsigned int openat_flags = 0;

  g_assert ((flags & ~(GLNX_CHASE_NO_AUTOMOUNT |
                       GLNX_CHASE_NOFOLLOW |
                       GLNX_CHASE_ALL_DEBUG_FLAGS)) == 0);

  /* First we try to actually use open_tree, and then fall back to the impl
   * using openat.
   * Technically racy (static, not synced), but both paths work fine so it
   * doesn't matter. */
  if (can_open_tree && (flags & GLNX_CHASE_DEBUG_NO_OPEN_TREE) == 0)
    {
      unsigned int open_tree_flags = 0;

      open_tree_flags = OPEN_TREE_CLOEXEC;
      if ((flags & GLNX_CHASE_NOFOLLOW) != 0)
        open_tree_flags |= AT_SYMLINK_NOFOLLOW;
      if ((flags & GLNX_CHASE_NO_AUTOMOUNT) != 0)
        open_tree_flags |= AT_NO_AUTOMOUNT;

      fd = open_tree (dirfd, path, open_tree_flags);

      /* If open_tree is not supported, or blocked (EPERM), we fall back to
       * openat */
      if (fd < 0 && G_IN_SET (errno,
                              EOPNOTSUPP,
                              ENOTTY,
                              ENOSYS,
                              EAFNOSUPPORT,
                              EPFNOSUPPORT,
                              EPROTONOSUPPORT,
                              ESOCKTNOSUPPORT,
                              ENOPROTOOPT,
                              EPERM))
        can_open_tree = FALSE;
      else if (fd < 0)
        return glnx_fd_throw_errno_prefix (error, "open_tree");
      else
        return g_steal_fd (&fd);
    }

  openat_flags = O_CLOEXEC | O_PATH;
  if ((flags & GLNX_CHASE_NOFOLLOW) != 0)
    openat_flags |= O_NOFOLLOW;

  fd = openat (dirfd, path, openat_flags);
  if (fd < 0)
    return glnx_fd_throw_errno_prefix (error, "openat in open_tree fallback");

  /* openat does not trigger automounts, so we have to manually do so
   * unless NO_AUTOMOUNT was specified */
  if ((flags & GLNX_CHASE_NO_AUTOMOUNT) == 0)
    {
      struct statfs stfs;

      if (fstatfs (fd, &stfs) < 0)
        return glnx_fd_throw_errno_prefix (error, "fstatfs in open_tree fallback");

      /* fstatfs(2) can then be used to determine if it is, in fact, an
       * untriggered automount point (.f_type == AUTOFS_SUPER_MAGIC). */
      if (stfs.f_type == AUTOFS_SUPER_MAGIC)
        {
          glnx_autofd int new_fd = -1;

          new_fd = openat (fd, ".", openat_flags | O_DIRECTORY);
          /* For some reason, openat with O_PATH | O_DIRECTORY does trigger
           * automounts, without us having to actually open the file, so let's
           * use this here. It only works for directories though. */
          if (new_fd >= 0)
            return g_steal_fd (&new_fd);

          if (errno != ENOTDIR)
            return glnx_fd_throw_errno_prefix (error, "openat(O_DIRECTORY) in autofs mount open_tree fallback");

          /* The automount is a directory, so let's try to open the file,
           * which can fail because we are missing permissions, but that's
           * okay, we only need to trigger automount. */
          new_fd = openat (fd, ".", (openat_flags & ~O_PATH) |
                                    O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
          glnx_close_fd (&new_fd);

          /* And try again with O_PATH */
          new_fd = openat (dirfd, path, openat_flags);
          if (new_fd < 0)
            return glnx_fd_throw_errno_prefix (error, "reopening in autofs mount open_tree fallback");

          if (fstatfs (new_fd, &stfs) < 0)
            return glnx_fd_throw_errno_prefix (error, "fstatfs in autofs mount open_tree fallback");

          /* bail if we didn't manage to trigger the automount */
          if (stfs.f_type == AUTOFS_SUPER_MAGIC)
            {
              errno = EOPNOTSUPP;
              return glnx_fd_throw_errno_prefix (error, "unable to trigger automount");
            }

          return g_steal_fd (&new_fd);
        }
    }

  return g_steal_fd (&fd);
}

static int
open_cwd (GlnxChaseFlags   flags,
          GError         **error)
{
  GLNX_AUTO_PREFIX_ERROR ("cannot open working directory", error);

  /* NO_AUTOMOUNT should be fine here because automount must have been
   * triggered already for the CWD */
  return chase_open_tree (AT_FDCWD, ".",
                          (flags & GLNX_CHASE_ALL_DEBUG_FLAGS) |
                          GLNX_CHASE_NO_AUTOMOUNT |
                          GLNX_CHASE_NOFOLLOW,
                          error);
}

static int
open_root (GlnxChaseFlags   flags,
           GError         **error)
{
  GLNX_AUTO_PREFIX_ERROR ("cannot open root directory", error);

  /* NO_AUTOMOUNT should be fine here because automount must have been
   * triggered already for the root */
  return chase_open_tree (AT_FDCWD, "/",
                          (flags & GLNX_CHASE_ALL_DEBUG_FLAGS) |
                          GLNX_CHASE_NO_AUTOMOUNT |
                          GLNX_CHASE_NOFOLLOW,
                          error);
}

/* This returns the next segment of a path and tells us if it is the last
 * segment.
 *
 * Importantly, a segment is anything after a "/", even if it is empty  or ".".
 *
 * For example:
 *   "" -> ""
 *   "/" -> ""
 *   "////" -> ""
 *   "foo/bar" -> "foo", "bar"
 *   "foo//bar" -> "foo", "bar"
 *   "///foo//bar" -> "foo", "bar"
 *   "///foo//bar/" -> "foo", "bar", ""
 *   "///foo//bar/." -> "foo", "bar", "."
 */
static char *
extract_next_segment (const char **remaining,
                      gboolean    *is_last)
{
  const char *r = *remaining;
  const char *s;
  size_t len = 0;

  while (r[0] != '\0' && G_IS_DIR_SEPARATOR (r[0]))
    r++;

  s = r;

  while (r[0] != '\0' && !G_IS_DIR_SEPARATOR (r[0]))
    {
      r++;
      len++;
    }

  *is_last = (r[0] == '\0');
  *remaining = r;
  return g_strndup (s, len);
}

/* This iterates over the segments of path and opens the corresponding
 * directories or files. This gives us the opportunity to implement openat2
 * like RESOLVE_ semantics, without actually needing openat2.
 * It also allows us to implement features which openat2 does not have because
 * we're in full control over the resolving.
 */
static int
chase_manual (int              dirfd,
              const char      *path,
              GlnxChaseFlags   flags,
              GError         **error)
{
  gboolean is_absolute;
  g_autofree char *buffer = NULL;
  const char *remaining;
  glnx_autofd int owned_root_fd = -1;
  int root_fd;
  glnx_autofd int owned_fd = -1;
  int fd;
  int remaining_follows = GLNX_CHASE_MAX;
  struct glnx_statx st;
  g_auto(GlnxStatxQueue) path_st = G_QUEUE_INIT;
  int no_automount;

  /* Take a shortcut if
   * - none of the resolve flags are set (they would require work here)
   * - NO_AUTOMOUNT is set (chase_open_tree only triggers the automount for
   *   last component in some cases)
   *
   * TODO: if we have a guarantee that the open_tree syscall works, we can
   * shortcut even without GLNX_CHASE_NO_AUTOMOUNT
   */
  if ((flags & (GLNX_CHASE_NO_AUTOMOUNT |
                GLNX_CHASE_RESOLVE_BENEATH |
                GLNX_CHASE_RESOLVE_IN_ROOT |
                GLNX_CHASE_RESOLVE_NO_SYMLINKS)) == GLNX_CHASE_NO_AUTOMOUNT)
    {
      GlnxChaseFlags open_tree_flags =
        (flags & (GLNX_CHASE_NOFOLLOW | GLNX_CHASE_ALL_DEBUG_FLAGS));

      return chase_open_tree (dirfd, path, open_tree_flags, error);
    }

  no_automount = (flags & GLNX_CHASE_NO_AUTOMOUNT) != 0 ? AT_NO_AUTOMOUNT : 0;

  is_absolute = g_path_is_absolute (path);

  if (is_absolute && (flags & GLNX_CHASE_RESOLVE_BENEATH) != 0)
    {
      /* Absolute paths always get rejected with RESOLVE_BENEATH with errno
       * EXDEV */

      errno = EXDEV;
      return glnx_fd_throw_errno_prefix (error, "absolute path not allowed for RESOLVE_BENEATH");
    }
  else if (!is_absolute ||
           (is_absolute && (flags & GLNX_CHASE_RESOLVE_IN_ROOT) != 0))
    {
      /* The absolute path is relative to dirfd with GLNX_CHASE_RESOLVE_IN_ROOT,
       * and a relative path is always relative. */

      /* In both cases we use dirfd as our chase root */
      if (dirfd == AT_FDCWD)
        {
          owned_root_fd = root_fd = open_cwd (flags, error);
          if (root_fd < 0)
            return -1;
        }
      else
        {
          root_fd = dirfd;
        }
    }
  else
    {
      /* For absolute paths, we ignore dirfd, we use the actual root / for our
       * chase root */
      g_assert (is_absolute);

      owned_root_fd = root_fd = open_root (flags, error);
      if (root_fd < 0)
        return -1;
    }

  /* At this point, we always have (a relative) path, relative to root_fd */
  is_absolute = FALSE;
  g_assert (root_fd >= 0);

  /* Add root to path_st, so we can verify if we get back to it */
  if (!glnx_chase_statx (root_fd, no_automount, &st, error))
    return -1;

  glnx_statx_queue_push (&path_st, &st);

  /* Let's start walking the path! */
  buffer = g_strdup (path);
  remaining = buffer;
  fd = root_fd;

  for (;;)
    {
      g_autofree char *segment = NULL;
      gboolean is_last;
      glnx_autofd int next_fd = -1;

      segment = extract_next_segment (&remaining, &is_last);

      /* If we encounter an empty segment ("", "."), we stay where we are and
       * ignore the segment, or just exit if it is the last segment. */
      if (g_strcmp0 (segment, "") == 0 || g_strcmp0 (segment, ".") == 0)
        {
          if (is_last)
            break;
          continue;
        }

      /* Special handling for going down the tree with RESOLVE_ flags */
      if (g_strcmp0 (segment, "..") == 0)
        {
          /* path_st contains the stat of the root if we're at root, so the
           * length is 1 in that case, and going lower than the root is not
           * allowed here! */

          if (path_st.length <= 1 && (flags & GLNX_CHASE_RESOLVE_BENEATH) != 0)
            {
              /* With RESOLVE_BENEATH, error out if we would end up above the
               * root fd */
              errno = EXDEV;
              return glnx_fd_throw_errno_prefix (error, "attempted to traverse above root path via \"..\"");
            }
          else if (path_st.length <= 1 && (flags & GLNX_CHASE_RESOLVE_IN_ROOT) != 0)
            {
              /* With RESOLVE_IN_ROOT, we pretend that we hit the real root,
               * and stay there, just like the kernel does. */
              continue;
            }
        }

      {
        /* Open the next segment. We always use GLNX_CHASE_NOFOLLOW here to be
         * able to ensure the RESOLVE flags, and automount behavior. */

        GlnxChaseFlags open_tree_flags =
          GLNX_CHASE_NOFOLLOW |
          (flags & (GLNX_CHASE_NO_AUTOMOUNT | GLNX_CHASE_ALL_DEBUG_FLAGS));

        next_fd = chase_open_tree (fd, segment, open_tree_flags, error);
        if (next_fd < 0)
          return -1;
      }

      if (!glnx_chase_statx (next_fd, no_automount, &st, error))
        return -1;

      /* We resolve links if: they are not in the last component, or if they
       * are the last component and NOFOLLOW is not set. */
      if (S_ISLNK (st.stx_mode) &&
          (!is_last || (flags & GLNX_CHASE_NOFOLLOW) == 0))
        {
          g_autofree char *link = NULL;
          g_autofree char *new_buffer = NULL;

          /* ...however, we do not resolve symlinks with NO_SYMLINKS, and use
           * remaining_follows to ensure we don't loop forever. */
          if ((flags & GLNX_CHASE_RESOLVE_NO_SYMLINKS) != 0 ||
              --remaining_follows <= 0)
            {
              errno = ELOOP;
              return glnx_fd_throw_errno_prefix (error, "followed too many symlinks");
            }

          /* AT_EMPTY_PATH is implied for readlinkat */
          link = glnx_readlinkat_malloc (next_fd, "", NULL, error);
          if (!link)
            return -1;

          if (g_path_is_absolute (link) &&
              (flags & GLNX_CHASE_RESOLVE_BENEATH) != 0)
            {
              errno = EXDEV;
              return glnx_fd_throw_errno_prefix (error, "absolute symlink not allowed for RESOLVE_BENEATH");
            }

          /* The link can be absolute, and we handle that below, by changing the
           * dirfd. The path *remains* and absolute path internally, but that is
           * okay because we always interpret any path (even absolute ones) as
           * being relative to the dirfd */
          new_buffer = g_strdup_printf ("%s/%s", link, remaining);
          g_clear_pointer (&buffer, g_free);
          buffer = g_steal_pointer (&new_buffer);
          remaining = buffer;

          if (g_path_is_absolute (link))
            {
              if ((flags & GLNX_CHASE_RESOLVE_IN_ROOT) != 0)
                {
                  /* If the path was absolute, and RESOLVE_IN_ROOT is set, we
                   * will resolve the remaining path relative to root_fd */

                  g_clear_fd (&owned_fd, NULL);
                  fd = root_fd;
                }
              else
                {
                  /* If the path was absolute, we will resolve the remaining
                   * path relative to the real root */

                  g_clear_fd (&owned_fd, NULL);
                  fd = owned_fd = open_root (flags, error);
                  if (fd < 0)
                    return -1;
                }

              /* path_st must only contain the new root at this point */
              if (!glnx_chase_statx (fd, no_automount, &st, error))
                return -1;

              glnx_statx_queue_free (&path_st);
              g_queue_init (&path_st);
              glnx_statx_queue_push (&path_st, &st);
            }

          continue;
        }

      /* Either adds an element to path_st or removes one if we got down the
       * tree. This also checks that going down the tree ends up at the inode
       * we saw before (if we saw it before). */
      if (g_strcmp0 (segment, "..") == 0)
        {
          g_autofree struct glnx_statx *old_tail = NULL;
          struct glnx_statx *lower_st;

          old_tail = g_queue_pop_tail (&path_st);

          lower_st = g_queue_peek_tail (&path_st);
          if (lower_st &&
              (!glnx_statx_mount_same (&st, lower_st) ||
               !glnx_statx_inode_same (&st, lower_st)))
            {
              errno = EXDEV;
              return glnx_fd_throw_errno_prefix (error, "a parent directory changed while traversing");
            }
        }
      else
        {
          glnx_statx_queue_push (&path_st, &st);
        }

      /* There is still another path component, but the next fd is not a
       * a directory. We need the fd to be a directory though, to open the next
       * segment from. So bail with the appropriate error. */
      if (!is_last && !S_ISDIR (st.stx_mode))
        {
          errno = ENOTDIR;
          return glnx_fd_throw_errno_prefix (error, "a non-final path segment is not a directory");
        }

      g_clear_fd (&owned_fd, NULL);
      fd = owned_fd = g_steal_fd (&next_fd);

      if (is_last)
        break;
    }

  /* We need an owned fd to return. Only having fd and not owned_fd can happen
   * if we never finished a single iteration, or if an absolute path with
   * RESOLVE_IN_ROOT makes us point at root_fd.
   * We just re-open fd to always get an owned fd.
   * Note that this only works because in all cases where owned_fd does not
   * exists, fd is a directory. */
  if (owned_fd < 0)
    {
      owned_fd = openat (fd, ".", O_PATH | O_CLOEXEC | O_NOFOLLOW);
      if (owned_fd < 0)
        return glnx_fd_throw_errno_prefix (error, "reopening failed");
    }

  return g_steal_fd (&owned_fd);
}

/**
 * glnx_chaseat:
 * @dirfd: a directory file descriptor
 * @path: a path
 * @flags: combination of GlnxChaseFlags flags
 * @error: a #GError
 *
 * Behaves similar to openat, but with a number of differences:
 *
 * - All file descriptors which get returned are O_PATH and O_CLOEXEC. If you
 *   want to actually open the file for reading or writing, use glnx_fd_reopen,
 *   openat, or other at-style functions.
 * - By default, automounts get triggered and the O_PATH fd will point to inodes
 *   in the newly mounted filesystem if an automount is encountered. This can be
 *   turned off with GLNX_CHASE_NO_AUTOMOUNT.
 * - The GLNX_CHASE_RESOLVE_ flags can be used to safely deal with symlinks.
 *
 * Returns: the chased file, or -1 with @error set on error
 */
int
glnx_chaseat (int              dirfd,
              const char      *path,
              GlnxChaseFlags   flags,
              GError         **error)
{
  static gboolean can_openat2 = TRUE;
  glnx_autofd int fd = -1;

  g_return_val_if_fail (dirfd >= 0 || dirfd == AT_FDCWD, -1);
  g_return_val_if_fail (path != NULL, -1);
  g_return_val_if_fail ((flags & ~(GLNX_CHASE_ALL_FLAGS)) == 0, -1);
  g_return_val_if_fail (error == NULL || *error == NULL, -1);

  {
    int must_flags = flags & (GLNX_CHASE_MUST_BE_REGULAR |
                              GLNX_CHASE_MUST_BE_DIRECTORY |
                              GLNX_CHASE_MUST_BE_SOCKET);
    /* check that no more than one bit is set (= power of two) */
    g_return_val_if_fail ((must_flags & (must_flags - 1)) == 0, -1);
  }

  /* TODO: Add a callback which is called for every resolved path segment, to
   * allow users to verify and expand the functionality safely. */

  /* We need the manual impl for NO_AUTOMOUNT, and we can skip this, if we don't
   * have openat2 at all.
   * Technically racy (static, not synced), but both paths work fine so it
   * doesn't matter. */
  if (can_openat2 && (flags & GLNX_CHASE_NO_AUTOMOUNT) == 0 &&
      (flags & GLNX_CHASE_DEBUG_NO_OPENAT2) == 0)
    {
      uint64_t openat2_flags = 0;
      uint64_t openat2_resolve = 0;
      struct open_how how;

      openat2_flags = O_PATH | O_CLOEXEC;
      if ((flags & GLNX_CHASE_NOFOLLOW) != 0)
        openat2_flags |= O_NOFOLLOW;

      openat2_resolve |= RESOLVE_NO_MAGICLINKS;
      if ((flags & GLNX_CHASE_RESOLVE_BENEATH) != 0)
        openat2_resolve |= RESOLVE_BENEATH;
      if ((flags & GLNX_CHASE_RESOLVE_IN_ROOT) != 0)
        openat2_resolve |= RESOLVE_IN_ROOT;
      if ((flags & GLNX_CHASE_RESOLVE_NO_SYMLINKS) != 0)
        openat2_resolve |= RESOLVE_NO_SYMLINKS;

      how = (struct open_how) {
        .flags = openat2_flags,
        .mode = 0,
        .resolve = openat2_resolve,
      };

      fd = openat2 (dirfd, path, &how, sizeof (how));
      if (fd < 0)
        {
          /* If the syscall is not implemented (ENOSYS) or blocked by
           * seccomp (EPERM), we need to fall back to the manual path chasing
           * via open_tree. */
          if (G_IN_SET (errno, ENOSYS, EPERM))
            can_openat2 = FALSE;
          else
            return glnx_fd_throw_errno (error);
        }
    }

  if (fd < 0)
    {
      fd = chase_manual (dirfd, path, flags, error);
      if (fd < 0)
        return -1;
    }

  if ((flags & (GLNX_CHASE_MUST_BE_REGULAR |
                GLNX_CHASE_MUST_BE_DIRECTORY |
                GLNX_CHASE_MUST_BE_SOCKET)) != 0)
    {
      struct glnx_statx st;

      if (!glnx_statx (fd, "",
                       AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW |
                       ((flags & GLNX_CHASE_NO_AUTOMOUNT) ? AT_NO_AUTOMOUNT : 0),
                       GLNX_STATX_TYPE,
                       &st,
                       error))
        return -1;

      if ((st.stx_mask & GLNX_STATX_TYPE) == 0)
        {
          errno = ENODATA;
          return glnx_fd_throw_errno_prefix (error, "unable to get file type");
        }

      if ((flags & GLNX_CHASE_MUST_BE_REGULAR) != 0 &&
          !S_ISREG (st.stx_mode))
        {
          if (S_ISDIR (st.stx_mode))
            errno = EISDIR;
          else
            errno = EBADFD;

          return glnx_fd_throw_errno_prefix (error, "not a regular file");
        }

      if ((flags & GLNX_CHASE_MUST_BE_DIRECTORY) != 0 &&
          !S_ISDIR (st.stx_mode))
        {
          errno = ENOTDIR;
          return glnx_fd_throw_errno_prefix (error, "not a directory");
        }

      if ((flags & GLNX_CHASE_MUST_BE_SOCKET) != 0 &&
          !S_ISSOCK (st.stx_mode))
        {
          errno = ENOTSOCK;
          return glnx_fd_throw_errno_prefix (error, "not a socket");
        }
    }

  return g_steal_fd (&fd);
}

/**
 * glnx_chase_and_statxat:
 * @dirfd: a directory file descriptor
 * @path: a path
 * @flags: combination of GlnxChaseFlags flags
 * @mask: combination of GLNX_STATX_ flags
 * @statbuf: a pointer to a struct glnx_statx which will be filled out
 * @error: a #GError
 *
 * Stats the file at @path relative to @dirfd and fills out @statbuf with the
 * result according to the interest mask @mask.
 *
 * See glnx_chaseat for the meaning of @dirfd, @path, and @flags.
 *
 * Returns: the chased file, or -1 with @error set on error
 */
int
glnx_chase_and_statxat (int                 dirfd,
                        const char         *path,
                        GlnxChaseFlags      flags,
                        unsigned int        mask,
                        struct glnx_statx  *statbuf,
                        GError            **error)
{
  glnx_autofd int fd = -1;

  /* other args are checked by glnx_chaseat */
  g_return_val_if_fail (statbuf != NULL, FALSE);

  fd = glnx_chaseat (dirfd, path, flags, error);
  if (fd < 0)
    return -1;

  if (!glnx_statx (fd, "",
                   AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW |
                   ((flags & GLNX_CHASE_NO_AUTOMOUNT) ? AT_NO_AUTOMOUNT : 0),
                   mask,
                   statbuf,
                   error))
    return -1;

  return g_steal_fd (&fd);
}
