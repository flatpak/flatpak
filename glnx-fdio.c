/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <stdio.h>

#include <glnx-fdio.h>
#include <glnx-errors.h>
#include <glnx-local-alloc.h>

static guint8*
glnx_fd_readall_malloc (int               fd,
                        gsize            *out_len,
                        gboolean          nul_terminate,
                        GCancellable     *cancellable,
                        GError          **error)
{
  gboolean success = FALSE;
  const guint maxreadlen = 4096;
  int res;
  struct stat stbuf;
  guint8* buf = NULL;
  gsize buf_allocated;
  gsize buf_size = 0;
  gssize bytes_read;

  do
    res = fstat (fd, &stbuf);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (S_ISREG (stbuf.st_mode) && stbuf.st_size > 0)
    buf_allocated = stbuf.st_size;
  else
    buf_allocated = 16;
        
  buf = g_malloc (buf_allocated);

  while (TRUE)
    {
      gsize readlen = MIN (buf_allocated - buf_size, maxreadlen);
      
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      do
        bytes_read = read (fd, buf + buf_size, readlen);
      while (G_UNLIKELY (bytes_read == -1 && errno == EINTR));
      if (G_UNLIKELY (bytes_read == -1))
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      if (bytes_read == 0)
        break;
      
      buf_size += bytes_read;
      if (buf_allocated - buf_size < maxreadlen)
        buf = g_realloc (buf, buf_allocated *= 2);
    }

  if (nul_terminate)
    {
      if (buf_allocated - buf_size == 0)
        buf = g_realloc (buf, buf_allocated + 1);
      buf[buf_size] = '\0';
    }

  success = TRUE;
 out:
  if (success)
    {
      *out_len = buf_size;
      return buf;
    }
  g_free (buf);
  return NULL;
}

/**
 * glnx_fd_readall_bytes:
 * @fd: A file descriptor
 * @cancellable: Cancellable:
 * @error: Error
 *
 * Read all data from file descriptor @fd into a #GBytes.  It's
 * recommended to only use this for small files.
 *
 * Returns: (transfer full): A newly allocated #GBytes
 */
GBytes *
glnx_fd_readall_bytes (int               fd,
                       GCancellable     *cancellable,
                       GError          **error)
{
  guint8 *buf;
  gsize len;
  
  buf = glnx_fd_readall_malloc (fd, &len, FALSE, cancellable, error);
  if (!buf)
    return NULL;
  
  return g_bytes_new_take (buf, len);
}

/**
 * glnx_fd_readall_utf8:
 * @fd: A file descriptor
 * @out_len: (out): Returned length
 * @cancellable: Cancellable:
 * @error: Error
 *
 * Read all data from file descriptor @fd, validating
 * the result as UTF-8.
 *
 * Returns: (transfer full): A string validated as UTF-8, or %NULL on error.
 */
char *
glnx_fd_readall_utf8 (int               fd,
                      gsize            *out_len,
                      GCancellable     *cancellable,
                      GError          **error)
{
  gboolean success = FALSE;
  guint8 *buf;
  gsize len;
  
  buf = glnx_fd_readall_malloc (fd, &len, TRUE, cancellable, error);
  if (!buf)
    goto out;

  if (!g_utf8_validate ((char*)buf, len, NULL))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Invalid UTF-8");
      goto out;
    }

  success = TRUE;
 out:
  if (success)
    {
      if (out_len)
        *out_len = len;
      return (char*)buf;
    }
  g_free (buf);
  return NULL;
}

/**
 * glnx_file_get_contents_utf8_at:
 * @dfd: Directory file descriptor
 * @subpath: Path relative to @dfd
 * @out_len: (out) (allow-none): Optional length
 * @cancellable: Cancellable
 * @error: Error
 *
 * Read the entire contents of the file referred
 * to by @dfd and @subpath, validate the result as UTF-8.
 * The length is optionally stored in @out_len.
 *
 * Returns: (transfer full): UTF-8 validated text, or %NULL on error
 */
char *
glnx_file_get_contents_utf8_at (int                   dfd,
                                const char           *subpath,
                                gsize                *out_len,
                                GCancellable         *cancellable,
                                GError              **error)
{
  gboolean success = FALSE;
  glnx_fd_close int fd = -1;
  char *buf;
  gsize len;

  do
    fd = openat (dfd, subpath, O_RDONLY | O_NOCTTY | O_CLOEXEC);
  while (G_UNLIKELY (fd == -1 && errno == EINTR));
  if (G_UNLIKELY (fd == -1))
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  buf = glnx_fd_readall_utf8 (fd, &len, cancellable, error);
  if (G_UNLIKELY(!buf))
    goto out;
  
  success = TRUE;
 out:
  if (success)
    {
      if (out_len)
        *out_len = len;
      return buf;
    }
  g_free (buf);
  return NULL;
}
