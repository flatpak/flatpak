/*
 * Copyright © 2019 Collabora Ltd.
 *
 * Originally from steam-runtime-tools.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "describe-fd.h"

#include "libglnx.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef union
{
  struct sockaddr addr;
  struct sockaddr_storage storage;
  struct sockaddr_in in;
  struct sockaddr_in6 in6;
  struct sockaddr_un un;
} any_sockaddr;

static void
string_append_escaped_len (GString *str,
                           const char *bytes,
                           size_t len)
{
  size_t i;

  for (i = 0; i < len; i++)
    {
      char c = bytes[i];

      if (c >= ' ' && c < 0x7f && c != '"' && c != '\\')
        g_string_append_c (str, c);
      else
        g_string_append_printf (str, "\\%03o", (unsigned char) c);
    }
}

static gboolean
string_append_sockaddr (GString *str,
                        const any_sockaddr *addr,
                        size_t addr_len)
{
  size_t path_size;
  char host[1024];

  switch (addr->storage.ss_family)
    {
      case AF_UNIX:
        if (addr_len <= offsetof (struct sockaddr_un, sun_path))
          {
            /* unnamed AF_UNIX socket */
            g_string_append (str, "AF_UNIX");
            return TRUE;
          }

        path_size = addr_len - offsetof (struct sockaddr_un, sun_path);
        g_string_append (str, "AF_UNIX \"");
        string_append_escaped_len (str, addr->un.sun_path, path_size);
        g_string_append_c (str, '"');
        return TRUE;

      case AF_INET:
      case AF_INET6:
        if (getnameinfo (&addr->addr, addr_len, host, sizeof (host), NULL, 0,
                         NI_NUMERICHOST) == 0)
          {
            if (addr->storage.ss_family == AF_INET6)
              g_string_append_c (str, '[');

            g_string_append (str, host);

            if (addr->storage.ss_family == AF_INET6)
              {
                g_string_append_printf (str, "]:%d",
                                        ntohs (addr->in6.sin6_port));
              }
            else
              {
                g_assert (addr->storage.ss_family == AF_INET);
                g_string_append_printf (str, ":%d", ntohs (addr->in.sin_port));
              }

            return TRUE;
          }

        return FALSE;

      default:
        return FALSE;
    }
}

/*
 * flatpak_describe_fd:
 *
 * Return some sort of human-readable description of @fd, suitable for
 * diagnostic use.
 *
 * If @fd is a regular file, the result will usually be its absolute path.
 *
 * If @fd is a socket, the result will usually show the local and peer
 * addresses.
 *
 * If @fd is not a valid file descriptor or its target cannot be discovered,
 * the result may be an error message, for example
 * "Bad file descriptor" or "readlinkat: No such file or directory".
 *
 * Returns: (type utf8): A diagnostic string. Free with g_free().
 */
gchar *
flatpak_describe_fd (int fd)
{
  g_autoptr(GError) local_error = NULL;
  g_autofree gchar *proc_self_fd_n = g_strdup_printf ("/proc/self/fd/%d", fd);
  g_autofree gchar *target = NULL;
  any_sockaddr addr = {};
  socklen_t addr_len = sizeof (addr);

  target = glnx_readlinkat_malloc (-1, proc_self_fd_n, NULL, &local_error);

  if (getsockname (fd, &addr.addr, &addr_len) == 0)
    {
      /* fd is a socket */
      g_autoptr(GString) ret = g_string_new (target);
      size_t position;

      position = ret->len;

      if (string_append_sockaddr (ret, &addr, addr_len))
        g_string_insert (ret, position, ": ");

      position = ret->len;
      memset (&addr, 0, sizeof (addr));
      addr_len = sizeof (addr);

      if (getpeername (fd, &addr.addr, &addr_len) == 0
          && string_append_sockaddr (ret, &addr, addr_len))
        g_string_insert (ret, position, " -> ");

      return g_string_free_and_steal (g_steal_pointer (&ret));
    }
  else if (errno == EBADF)
    {
      /* fd is not a valid file descriptor at all */
      return g_strdup (g_strerror (EBADF));
    }
  /* else fd is valid, but not a socket: maybe a regular file, or
   * some VFS object like a pipe */

  if (target == NULL)
    return g_strdup (local_error->message);

  return g_strescape (target, NULL);
}
