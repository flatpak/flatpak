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

#include <glnx-xattrs.h>
#include <glnx-errors.h>
#include <glnx-local-alloc.h>

static GVariant *
variant_new_ay_bytes (GBytes *bytes)
{
  gsize size;
  gconstpointer data;
  data = g_bytes_get_data (bytes, &size);
  g_bytes_ref (bytes);
  return g_variant_new_from_data (G_VARIANT_TYPE ("ay"), data, size,
                                  TRUE, (GDestroyNotify)g_bytes_unref, bytes);
}

static char *
canonicalize_xattrs (char    *xattr_string,
                     size_t   len)
{
  char *p;
  GSList *xattrs = NULL;
  GSList *iter;
  GString *result;

  result = g_string_new (0);

  p = xattr_string;
  while (p < xattr_string+len)
    {
      xattrs = g_slist_prepend (xattrs, p);
      p += strlen (p) + 1;
    }

  xattrs = g_slist_sort (xattrs, (GCompareFunc) strcmp);
  for (iter = xattrs; iter; iter = iter->next) {
    g_string_append (result, iter->data);
    g_string_append_c (result, '\0');
  }

  g_slist_free (xattrs);
  return g_string_free (result, FALSE);
}

static gboolean
read_xattr_name_array (const char *path,
                       int         fd,
                       const char *xattrs,
                       size_t      len,
                       GVariantBuilder *builder,
                       GError  **error)
{
  gboolean ret = FALSE;
  const char *p;
  int r;
  const char *funcstr;

  g_assert (path != NULL || fd != -1);

  funcstr = fd != -1 ? "fgetxattr" : "lgetxattr";

  for (p = xattrs; p < xattrs+len; p = p + strlen (p) + 1)
    {
      ssize_t bytes_read;
      g_autofree char *buf = NULL;
      g_autoptr(GBytes) bytes = NULL;

    again:
      if (fd != -1)
        bytes_read = fgetxattr (fd, p, NULL, 0);
      else
        bytes_read = lgetxattr (path, p, NULL, 0);
      if (bytes_read < 0)
        {
          if (errno == ENODATA)
            continue;

          glnx_set_prefix_error_from_errno (error, "%s", funcstr);
          goto out;
        }
      if (bytes_read == 0)
        continue;

      buf = g_malloc (bytes_read);
      if (fd != -1)
        r = fgetxattr (fd, p, buf, bytes_read);
      else
        r = lgetxattr (path, p, buf, bytes_read);
      if (r < 0)
        {
          if (errno == ERANGE)
            {
              g_free (g_steal_pointer (&buf));
              goto again;
            }
          else if (errno == ENODATA)
            continue;

          glnx_set_prefix_error_from_errno (error, "%s", funcstr);
          goto out;
        }

      bytes = g_bytes_new_take (g_steal_pointer (&buf), bytes_read);
      g_variant_builder_add (builder, "(@ay@ay)",
                             g_variant_new_bytestring (p),
                             variant_new_ay_bytes (bytes));
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
get_xattrs_impl (const char      *path,
                 int              fd,
                 GVariant       **out_xattrs,
                 GCancellable    *cancellable,
                 GError         **error)
{
  gboolean ret = FALSE;
  ssize_t bytes_read, real_size;
  glnx_free char *xattr_names = NULL;
  glnx_free char *xattr_names_canonical = NULL;
  GVariantBuilder builder;
  gboolean builder_initialized = FALSE;
  g_autoptr(GVariant) ret_xattrs = NULL;

  g_assert (path != NULL || fd != -1);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));
  builder_initialized = TRUE;

 again:
  if (path)
    bytes_read = llistxattr (path, NULL, 0);
  else
    bytes_read = flistxattr (fd, NULL, 0);

  if (bytes_read < 0)
    {
      if (errno != ENOTSUP)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "llistxattr");
          goto out;
        }
    }
  else if (bytes_read > 0)
    {
      xattr_names = g_malloc (bytes_read);
      if (path)
        real_size = llistxattr (path, xattr_names, bytes_read);
      else
        real_size = flistxattr (fd, xattr_names, bytes_read);
      if (real_size < 0)
        {
          if (errno == ERANGE)
            {
              g_free (xattr_names);
              goto again;
            }
          glnx_set_prefix_error_from_errno (error, "%s", "llistxattr");
          goto out;
        }
      else if (real_size > 0)
        {
          xattr_names_canonical = canonicalize_xattrs (xattr_names, real_size);

          if (!read_xattr_name_array (path, fd, xattr_names_canonical, real_size, &builder, error))
            goto out;
        }
    }

  ret_xattrs = g_variant_builder_end (&builder);
  builder_initialized = FALSE;
  g_variant_ref_sink (ret_xattrs);
  
  ret = TRUE;
  if (out_xattrs)
    *out_xattrs = g_steal_pointer (&ret_xattrs);
 out:
  if (!builder_initialized)
    g_variant_builder_clear (&builder);
  return ret;
}

/**
 * glnx_fd_get_all_xattrs:
 * @fd: a file descriptor
 * @out_xattrs: (out): A new #GVariant containing the extended attributes
 * @cancellable: Cancellable
 * @error: Error
 *
 * Read all extended attributes from @fd in a canonical sorted order, and
 * set @out_xattrs with the result.
 *
 * If the filesystem does not support extended attributes, @out_xattrs
 * will have 0 elements, and this function will return successfully.
 */
gboolean
glnx_fd_get_all_xattrs (int            fd,
                        GVariant     **out_xattrs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  return get_xattrs_impl (NULL, fd, out_xattrs,
                          cancellable, error);
}

/**
 * glnx_dfd_name_get_all_xattrs:
 * @dfd: Parent directory file descriptor
 * @name: File name
 * @out_xattrs: (out): Extended attribute set
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load all extended attributes for the file named @name residing in
 * directory @dfd.
 */
gboolean
glnx_dfd_name_get_all_xattrs (int            dfd,
                              const char    *name,
                              GVariant     **out_xattrs,
                              GCancellable  *cancellable,
                              GError       **error)
{
  if (dfd == AT_FDCWD || dfd == -1)
    {
      return get_xattrs_impl (name, -1, out_xattrs, cancellable, error);
    }
  else
    {
      char buf[PATH_MAX];
      /* A workaround for the lack of lgetxattrat(), thanks to Florian Weimer:
       * https://mail.gnome.org/archives/ostree-list/2014-February/msg00017.html
       */
      snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", dfd, name);
      return get_xattrs_impl (buf, -1, out_xattrs, cancellable, error);
    }
}

static gboolean
set_all_xattrs_for_path (const char    *path,
                         GVariant      *xattrs,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  int i, n;

  n = g_variant_n_children (xattrs);
  for (i = 0; i < n; i++)
    {
      const guint8* name;
      g_autoptr(GVariant) value = NULL;
      const guint8* value_data;
      gsize value_len;

      g_variant_get_child (xattrs, i, "(^&ay@ay)",
                           &name, &value);
      value_data = g_variant_get_fixed_array (value, &value_len, 1);
      
      if (lsetxattr (path, (char*)name, (char*)value_data, value_len, 0) < 0)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "lsetxattr");
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * glnx_dfd_name_set_all_xattrs:
 * @dfd: Parent directory file descriptor
 * @name: File name
 * @xattrs: Extended attribute set
 * @cancellable: Cancellable
 * @error: Error
 *
 * Set all extended attributes for the file named @name residing in
 * directory @dfd.
 */
gboolean
glnx_dfd_name_set_all_xattrs (int            dfd,
                              const char    *name,
                              GVariant      *xattrs,
                              GCancellable  *cancellable,
                              GError       **error)
{
  if (dfd == AT_FDCWD || dfd == -1)
    {
      return set_all_xattrs_for_path (name, xattrs, cancellable, error);
    }
  else
    {
      char buf[PATH_MAX];
      /* A workaround for the lack of lsetxattrat(), thanks to Florian Weimer:
       * https://mail.gnome.org/archives/ostree-list/2014-February/msg00017.html
       */
      snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", dfd, name);
      return set_all_xattrs_for_path (buf, xattrs, cancellable, error);
    }
}

/**
 * glnx_fd_set_all_xattrs:
 * @fd: File descriptor
 * @xattrs: Extended attributes
 * @cancellable: Cancellable
 * @error: Error
 *
 * For each attribute in @xattrs, set its value on the file or
 * directory referred to by @fd.  This function does not remove any
 * attributes not in @xattrs.
 */
gboolean
glnx_fd_set_all_xattrs (int            fd,
                        GVariant      *xattrs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  int i, n;

  n = g_variant_n_children (xattrs);
  for (i = 0; i < n; i++)
    {
      const guint8* name;
      const guint8* value_data;
      g_autoptr(GVariant) value = NULL;
      gsize value_len;
      int res;

      g_variant_get_child (xattrs, i, "(^&ay@ay)",
                           &name, &value);
      value_data = g_variant_get_fixed_array (value, &value_len, 1);
      
      do
        res = fsetxattr (fd, (char*)name, (char*)value_data, value_len, 0);
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (G_UNLIKELY (res == -1))
        {
          glnx_set_prefix_error_from_errno (error, "%s", "fsetxattr");
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * glnx_lgetxattrat:
 * @dfd: Directory file descriptor
 * @subpath: Subpath
 * @attribute: Extended attribute to retrieve
 * @error: Error
 *
 * Retrieve an extended attribute value, relative to a directory file
 * descriptor.
 */
GBytes *
glnx_lgetxattrat (int            dfd,
                  const char    *subpath,
                  const char    *attribute,
                  GError       **error)
{
  char pathbuf[PATH_MAX];
  GBytes *bytes = NULL;
  ssize_t bytes_read, real_size;
  guint8 *buf;

  snprintf (pathbuf, sizeof (pathbuf), "/proc/self/fd/%d/%s", dfd, subpath);

  do
    bytes_read = lgetxattr (pathbuf, attribute, NULL, 0);
  while (G_UNLIKELY (bytes_read < 0 && errno == EINTR));
  if (G_UNLIKELY (bytes_read < 0))
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  buf = g_malloc (bytes_read);
  do
    real_size = lgetxattr (pathbuf, attribute, buf, bytes_read);
  while (G_UNLIKELY (real_size < 0 && errno == EINTR));
  if (G_UNLIKELY (real_size < 0))
    {
      glnx_set_error_from_errno (error);
      g_free (buf);
      goto out;
    }

  bytes = g_bytes_new_take (buf, real_size);
 out:
  return bytes;
}

/**
 * glnx_fgetxattr_bytes:
 * @fd: Directory file descriptor
 * @attribute: Extended attribute to retrieve
 * @error: Error
 *
 * Returns: (transfer full): An extended attribute value, or %NULL on error
 */
GBytes *
glnx_fgetxattr_bytes (int            fd,
                      const char    *attribute,
                      GError       **error)
{
  GBytes *bytes = NULL;
  ssize_t bytes_read, real_size;
  guint8 *buf;

  do
    bytes_read = fgetxattr (fd, attribute, NULL, 0);
  while (G_UNLIKELY (bytes_read < 0 && errno == EINTR));
  if (G_UNLIKELY (bytes_read < 0))
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  buf = g_malloc (bytes_read);
  do
    real_size = fgetxattr (fd, attribute, buf, bytes_read);
  while (G_UNLIKELY (real_size < 0 && errno == EINTR));
  if (G_UNLIKELY (real_size < 0))
    {
      glnx_set_error_from_errno (error);
      g_free (buf);
      goto out;
    }

  bytes = g_bytes_new_take (buf, real_size);
 out:
  return bytes;
}

/**
 * glnx_lsetxattrat:
 * @dfd: Directory file descriptor
 * @subpath: Path
 * @attribute: An attribute name
 * @value: (array length=len) (element-type guint8): Attribute value
 * @len: Length of @value
 * @flags: Flags, containing either XATTR_CREATE or XATTR_REPLACE
 * @error: Error
 *
 * Set an extended attribute, relative to a directory file descriptor.
 */
gboolean
glnx_lsetxattrat (int            dfd,
                  const char    *subpath,
                  const char    *attribute,
                  const guint8  *value,
                  gsize          len,
                  int            flags,
                  GError       **error)
{
  char pathbuf[PATH_MAX];
  int res;

  snprintf (pathbuf, sizeof (pathbuf), "/proc/self/fd/%d/%s", dfd, subpath);

  do
    res = lsetxattr (subpath, attribute, value, len, flags);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (G_UNLIKELY (res == -1))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return TRUE;
}

