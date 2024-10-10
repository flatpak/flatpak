/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2024 Red Hat, Inc
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
 *       Owen Taylor <otaylor@redhat.com>
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <gio/gunixoutputstream.h>

#include "libglnx.h"
#include <gpgme.h>

#include "flatpak-oci-signatures-private.h"
#include "flatpak-utils-private.h"


G_DEFINE_AUTO_CLEANUP_FREE_FUNC (gpgme_data_t, gpgme_data_release, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (gpgme_ctx_t, gpgme_release, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (gpgme_key_t, gpgme_key_unref, NULL)

static void
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

static gboolean
signature_is_valid (gpgme_signature_t signature)
{
  /* Mimic the way librepo tests for a valid signature, checking both
   * summary and status fields.
   *
   * - VALID summary flag means the signature is fully valid.
   * - GREEN summary flag means the signature is valid with caveats.
   * - No summary but also no error means the signature is valid but
   *   the signing key is not certified with a trusted signature.
   */
  return (signature->summary & GPGME_SIGSUM_VALID) ||
         (signature->summary & GPGME_SIGSUM_GREEN) ||
         (signature->summary == 0 && signature->status == GPG_ERR_NO_ERROR);
}

static GString *
read_gpg_buffer (gpgme_data_t buffer, GError **error)
{
  g_autoptr(GString) res = g_string_new ("");
  char buf[1024];
  int ret;

  ret = gpgme_data_seek (buffer, 0, SEEK_SET);
  if (ret)
    {
      flatpak_fail (error, "Can't seek in gpg plain text");
      return NULL;
    }
  while ((ret = gpgme_data_read (buffer, buf, sizeof (buf) - 1)) > 0)
    g_string_append_len (res, buf, ret);
  if (ret < 0)
    {
      flatpak_fail (error, "Can't read in gpg plain text");
      return NULL;
    }

  return g_steal_pointer (&res);
}

static gboolean
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

  g_return_val_if_fail (gpgme_ctx != NULL, FALSE);

  /* GPGME has no API for using multiple keyrings (aka, gpg --keyring),
   * so we create a temporary directory and tell GPGME to use it as the
   * home directory.  Then (optionally) create a pubring.gpg file there
   * and hand the caller an open output stream to concatenate necessary
   * keyring files. */

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

  if (g_file_query_exists (keyring_file, NULL) &&
      !glnx_file_copy_at (AT_FDCWD, flatpak_file_get_path_cached (keyring_file), NULL,
                          tmpdir->fd, "pubring.gpg",
                          GLNX_FILE_COPY_OVERWRITE | GLNX_FILE_COPY_NOXATTRS,
                          cancellable, error))
    return FALSE;

  return TRUE;
}

FlatpakOciSignature *
flatpak_oci_verify_signature (OstreeRepo *repo,
                              const char *remote_name,
                              GBytes     *signed_data,
                              GError    **error)
{
  gpgme_ctx_t context;
  gpgme_error_t gpg_error;
  g_auto(gpgme_data_t) signed_data_buffer = NULL;
  g_auto(gpgme_data_t) plain_buffer = NULL;
  gpgme_verify_result_t vresult;
  gpgme_signature_t sig;
  int valid_count;
  g_autoptr(GString) plain = NULL;
  g_autoptr(GBytes) plain_bytes = NULL;
  g_autoptr(FlatpakJson) json = NULL;
  g_auto(GLnxTmpDir) tmp_home_dir = { 0, };

  gpg_error = gpgme_new (&context);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to create context: ");
      return NULL;
    }

  if (!flatpak_gpgme_ctx_tmp_home_dir (context, &tmp_home_dir, repo, remote_name, NULL, error))
    return NULL;

  gpg_error = gpgme_data_new_from_mem (&signed_data_buffer,
                                       g_bytes_get_data (signed_data, NULL),
                                       g_bytes_get_size (signed_data),
                                       0 /* do not copy */);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to read signed data: ");
      return NULL;
    }

  gpg_error = gpgme_data_new (&plain_buffer);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to allocate plain buffer: ");
      return NULL;
    }

  gpg_error = gpgme_op_verify (context, signed_data_buffer, NULL, plain_buffer);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to complete signature verification: ");
      return NULL;
    }

  vresult = gpgme_op_verify_result (context);

  valid_count = 0;
  for (sig = vresult->signatures; sig != NULL; sig = sig->next)
    {
      if (signature_is_valid (sig))
        valid_count++;
    }

  if (valid_count == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "GPG signatures found, but none are in trusted keyring");
      return NULL;
    }

  plain = read_gpg_buffer (plain_buffer, error);
  if (plain == NULL)
    return NULL;
  plain_bytes = g_string_free_to_bytes (g_steal_pointer (&plain));
  json = flatpak_json_from_bytes (plain_bytes, FLATPAK_TYPE_OCI_SIGNATURE, error);
  if (json == NULL)
    return NULL;

  return (FlatpakOciSignature *) g_steal_pointer (&json);
}

