/*
 * Copyright 2026 Philip Withnall
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
 *       Philip Withnall <philip@tecnocode.co.uk>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <gio/gio.h>
#include <gpgme.h>
#include <ostree.h>

#include "libglnx.h"

#include "flatpak-gpg-keys-private.h"
#include "flatpak-utils-private.h"

void
flatpak_gpgme_error_to_gio_error (gpgme_error_t gpg_error,
                                  GError      **error)
{
  GIOErrorEnum errcode;

  /* XXX This list is incomplete.  Add cases as needed. */

  switch (gpgme_err_code (gpg_error))
    {
    /* special case - shouldn't be here */
    case GPG_ERR_NO_ERROR:
      g_return_if_reached ();

    /* special case - abort on out-of-memory */
    case GPG_ERR_ENOMEM:
      g_error ("%s: out of memory",
               gpgme_strsource (gpg_error));

    case GPG_ERR_INV_VALUE:
      errcode = G_IO_ERROR_INVALID_ARGUMENT;
      break;

    default:
      errcode = G_IO_ERROR_FAILED;
      break;
    }

  g_set_error (error, G_IO_ERROR, errcode, "%s: error code %d",
               gpgme_strsource (gpg_error), gpgme_err_code (gpg_error));
}

gboolean
flatpak_gpgme_ctx_tmp_home_dir (gpgme_ctx_t   gpgme_ctx,
                                GLnxTmpDir   *tmpdir,
                                OstreeRepo   *repo,
                                const char   *remote_name,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autofree char *tmp_home_dir_pattern = NULL;
  gpgme_error_t gpg_error;
  g_autoptr(GFile) keyring_file = NULL;
  g_autofree char *keyring_name = NULL;
  g_autoptr(GError) local_error = NULL;

  g_return_val_if_fail (gpgme_ctx != NULL, FALSE);

  /* GPGME has no API for using multiple keyrings (aka, gpg --keyring),
   * so we create a temporary directory and tell GPGME to use it as the
   * home directory.  Then copy in the ${remote_name}.trustedkeys.gpg keyring
   * from the repo to begin with (if it exists). */

  tmp_home_dir_pattern = g_build_filename (g_get_tmp_dir (), "flatpak-gpg-XXXXXX", NULL);

  if (!glnx_mkdtempat (AT_FDCWD, tmp_home_dir_pattern, 0700,
                       tmpdir, error))
    return FALSE;

  /* Not documented, but gpgme_ctx_set_engine_info() accepts NULL for
   * the executable file name, which leaves the old setting unchanged. */
  gpg_error = gpgme_ctx_set_engine_info (gpgme_ctx,
                                         GPGME_PROTOCOL_OpenPGP,
                                         NULL, tmpdir->path);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      return FALSE;
    }

  keyring_name = g_strdup_printf ("%s.trustedkeys.gpg", remote_name);
  keyring_file = g_file_get_child (ostree_repo_get_path (repo), keyring_name);

  if (!glnx_file_copy_at (AT_FDCWD, flatpak_file_get_path_cached (keyring_file), NULL,
                          tmpdir->fd, "pubring.gpg",
                          GLNX_FILE_COPY_OVERWRITE | GLNX_FILE_COPY_NOXATTRS,
                          cancellable, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          glnx_autofd int fd = -1;

          /* Create an empty pubring.gpg file prior to importing keys.  This
           * prevents gpg2 from creating a pubring.kbx file in the new keybox
           * format [1].  We want to stay with the older keyring format since
           * its performance issues are not relevant here.
           *
           * [1] https://gnupg.org/faq/whats-new-in-2.1.html#keybox
           */
          fd = openat (tmpdir->fd, "pubring.gpg", O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY, 0644);
          if (fd == -1)
            {
              glnx_set_prefix_error_from_errno (error, "%s", "Unable to create pubring.gpg");
              return FALSE;
            }
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  return TRUE;
}

