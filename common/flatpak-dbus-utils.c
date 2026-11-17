/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 *
 * Utilities for D-Bus services
 *
 * Copyright 2026 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "flatpak-dbus-utils-private.h"

/*
 * FlatpakUniqueFDList:
 *
 * Wrapper for a #GUnixFDList which checks that each fd is used at most once.
 */
struct _FlatpakUniqueFDList
{
  GUnixFDList *fd_list;
  int *fds;
  int n;
};

void
flatpak_unique_fd_list_free (FlatpakUniqueFDList *self)
{
  g_clear_object (&self->fd_list);
  g_free (self->fds);
  g_free (self);
}


/*
 * flatpak_unique_fd_list_new:
 * @fd_list: File descriptors obtained from a D-Bus call, which
 *  must not be modified as long as the returned list exists
 * @error: Raises a %G_DBUS_ERROR on failure
 *
 * Create a new list of unique fds based on @fd_list.
 *
 * Returns: The new list, or %NULL with %G_DBUS_ERROR_INVALID_ARGS
 *  if the fds in @fd_list were somehow not all unique.
 */
FlatpakUniqueFDList *
flatpak_unique_fd_list_new (GUnixFDList *fd_list,
                            GError **error)
{
  g_autoptr(FlatpakUniqueFDList) self = g_new0 (FlatpakUniqueFDList, 1);
  const int *fds;

  self->fd_list = g_object_ref (fd_list);
  fds = g_unix_fd_list_peek_fds (fd_list, &self->n);
  g_assert (self->n >= 0);
  self->fds = g_new0 (int, self->n);

  for (int i = 0; i < self->n; i++)
    {
      for (int j = i + 1; j < self->n; j++)
        {
          /* This probably can't actually happen: fd-passing the same fd
           * more than once results in two distinct fds at the destination,
           * as if via dup(). */
          if (fds[i] == fds[j])
            {
              g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                           "Duplicate file descriptor for handle %d and %d",
                           i, j);
              return NULL;
            }
        }

      self->fds[i] = fds[i];
    }

  return g_steal_pointer (&self);
}

/*
 * flatpak_unique_fd_list_get:
 * @self: (nullable): The list of fds, or %NULL if there were none
 * @handle: An index into the fds, in practice the value of a #GVariant
 *  of type %G_VARIANT_TYPE_HANDLE, which might be out-of-range
 * @error: Raises a %G_DBUS_ERROR on failure
 *
 * If @handle is in range for @self and has not yet been used,
 * return the corresponding fd.
 * Otherwise report an error.
 *
 * The returned fd remains owned by the #GUnixFDList owned by @self,
 * and therefore must not be closed by the caller,
 * except perhaps in a subprocess that will not finalize @self.
 *
 * Returns: The fd, or -1 with %G_DBUS_ERROR_INVALID_ARGS
 */
int
flatpak_unique_fd_list_get (FlatpakUniqueFDList *self,
                             int handle,
                             GError **error)
{
  int ret;

  if (self == NULL || handle < 0 || handle >= self->n)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "No file descriptor for handle %d",
                   handle);
      return -1;
    }

  if (self->fds[handle] < 0)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "File descriptor for handle %d already used",
                   handle);
      return -1;
    }

  ret = self->fds[handle];
  self->fds[handle] = -1;
  return ret;
}
