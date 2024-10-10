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

/**** The functions below are based on seahorse-gpgme-data.c ****/

static void
set_errno_from_gio_error (GError *error)
{
  /* This is the reverse of g_io_error_from_errno() */

  g_return_if_fail (error != NULL);

  switch (error->code)
    {
    case G_IO_ERROR_FAILED:
      errno = EIO;
      break;

    case G_IO_ERROR_NOT_FOUND:
      errno = ENOENT;
      break;

    case G_IO_ERROR_EXISTS:
      errno = EEXIST;
      break;

    case G_IO_ERROR_IS_DIRECTORY:
      errno = EISDIR;
      break;

    case G_IO_ERROR_NOT_DIRECTORY:
      errno = ENOTDIR;
      break;

    case G_IO_ERROR_NOT_EMPTY:
      errno = ENOTEMPTY;
      break;

    case G_IO_ERROR_NOT_REGULAR_FILE:
    case G_IO_ERROR_NOT_SYMBOLIC_LINK:
    case G_IO_ERROR_NOT_MOUNTABLE_FILE:
      errno = EBADF;
      break;

    case G_IO_ERROR_FILENAME_TOO_LONG:
      errno = ENAMETOOLONG;
      break;

    case G_IO_ERROR_INVALID_FILENAME:
      errno = EINVAL;
      break;

    case G_IO_ERROR_TOO_MANY_LINKS:
      errno = EMLINK;
      break;

    case G_IO_ERROR_NO_SPACE:
      errno = ENOSPC;
      break;

    case G_IO_ERROR_INVALID_ARGUMENT:
      errno = EINVAL;
      break;

    case G_IO_ERROR_PERMISSION_DENIED:
      errno = EPERM;
      break;

    case G_IO_ERROR_NOT_SUPPORTED:
      errno = ENOTSUP;
      break;

    case G_IO_ERROR_NOT_MOUNTED:
      errno = ENOENT;
      break;

    case G_IO_ERROR_ALREADY_MOUNTED:
      errno = EALREADY;
      break;

    case G_IO_ERROR_CLOSED:
      errno = EBADF;
      break;

    case G_IO_ERROR_CANCELLED:
      errno = EINTR;
      break;

    case G_IO_ERROR_PENDING:
      errno = EALREADY;
      break;

    case G_IO_ERROR_READ_ONLY:
      errno = EACCES;
      break;

    case G_IO_ERROR_CANT_CREATE_BACKUP:
      errno = EIO;
      break;

    case G_IO_ERROR_WRONG_ETAG:
      errno = EACCES;
      break;

    case G_IO_ERROR_TIMED_OUT:
      errno = EIO;
      break;

    case G_IO_ERROR_WOULD_RECURSE:
      errno = ELOOP;
      break;

    case G_IO_ERROR_BUSY:
      errno = EBUSY;
      break;

    case G_IO_ERROR_WOULD_BLOCK:
      errno = EWOULDBLOCK;
      break;

    case G_IO_ERROR_HOST_NOT_FOUND:
      errno = EHOSTDOWN;
      break;

    case G_IO_ERROR_WOULD_MERGE:
      errno = EIO;
      break;

    case G_IO_ERROR_FAILED_HANDLED:
      errno = 0;
      break;

    default:
      errno = EIO;
      break;
    }
}

static ssize_t
data_write_cb (void *handle, const void *buffer, size_t size)
{
  GOutputStream *output_stream = handle;
  gsize bytes_written;
  GError *local_error = NULL;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (output_stream), -1);

  if (g_output_stream_write_all (output_stream, buffer, size,
                                 &bytes_written, NULL, &local_error))
    {
      g_output_stream_flush (output_stream, NULL, &local_error);
    }

  if (local_error != NULL)
    {
      set_errno_from_gio_error (local_error);
      g_clear_error (&local_error);
      bytes_written = -1;
    }

  return bytes_written;
}

static off_t
data_seek_cb (void *handle, off_t offset, int whence)
{
  GObject *stream = handle;
  GSeekable *seekable;
  GSeekType seek_type = 0;
  off_t position = -1;
  GError *local_error = NULL;

  g_return_val_if_fail (G_IS_INPUT_STREAM (stream) ||
                        G_IS_OUTPUT_STREAM (stream), -1);

  if (!G_IS_SEEKABLE (stream))
    {
      errno = EOPNOTSUPP;
      goto out;
    }

  switch (whence)
    {
    case SEEK_SET:
      seek_type = G_SEEK_SET;
      break;

    case SEEK_CUR:
      seek_type = G_SEEK_CUR;
      break;

    case SEEK_END:
      seek_type = G_SEEK_END;
      break;

    default:
      g_assert_not_reached ();
    }

  seekable = G_SEEKABLE (stream);

  if (!g_seekable_seek (seekable, offset, seek_type, NULL, &local_error))
    {
      set_errno_from_gio_error (local_error);
      g_clear_error (&local_error);
      goto out;
    }

  position = g_seekable_tell (seekable);

out:
  return position;
}

static void
data_release_cb (void *handle)
{
  GObject *stream = handle;

  g_return_if_fail (G_IS_INPUT_STREAM (stream) ||
                    G_IS_OUTPUT_STREAM (stream));

  g_object_unref (stream);
}

static struct gpgme_data_cbs data_output_cbs = {
  NULL,
  data_write_cb,
  data_seek_cb,
  data_release_cb
};

static gpgme_data_t
flatpak_gpgme_data_output (GOutputStream *output_stream)
{
  gpgme_data_t data = NULL;
  gpgme_error_t gpg_error;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (output_stream), NULL);

  gpg_error = gpgme_data_new_from_cbs (&data, &data_output_cbs, output_stream);

  /* The only possible error is ENOMEM, which we abort on. */
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      g_assert (gpgme_err_code (gpg_error) == GPG_ERR_ENOMEM);
      flatpak_gpgme_error_to_gio_error (gpg_error, NULL);
      g_assert_not_reached ();
    }

  g_object_ref (output_stream);

  return data;
}

static gpgme_ctx_t
flatpak_gpgme_new_ctx (const char *homedir,
                       GError    **error)
{
  gpgme_error_t err;
  g_auto(gpgme_ctx_t) context = NULL;

  if ((err = gpgme_new (&context)) != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (err, error);
      g_prefix_error (error, "Unable to create gpg context: ");
      return NULL;
    }

  if (homedir != NULL)
    {
      gpgme_engine_info_t info;

      info = gpgme_ctx_get_engine_info (context);

      if ((err = gpgme_ctx_set_engine_info (context, info->protocol, NULL, homedir))
          != GPG_ERR_NO_ERROR)
        {
          flatpak_gpgme_error_to_gio_error (err, error);
          g_prefix_error (error, "Unable to set gpg homedir to '%s': ",
                          homedir);
          return NULL;
        }
    }

  return g_steal_pointer (&context);
}

GBytes *
flatpak_oci_sign_data (GBytes       *data,
                       const gchar **key_ids,
                       const char   *homedir,
                       GError      **error)
{
  g_auto(GLnxTmpfile) tmpf = { 0 };
  g_autoptr(GOutputStream) tmp_signature_output = NULL;
  g_auto(gpgme_ctx_t) context = NULL;
  gpgme_error_t err;
  g_auto(gpgme_data_t) commit_buffer = NULL;
  g_auto(gpgme_data_t) signature_buffer = NULL;
  g_autoptr(GMappedFile) signature_file = NULL;
  int i;

  if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, "/tmp", O_RDWR | O_CLOEXEC,
                                      &tmpf, error))
    return NULL;

  tmp_signature_output = g_unix_output_stream_new (tmpf.fd, FALSE);

  context = flatpak_gpgme_new_ctx (homedir, error);
  if (!context)
    return NULL;

  for (i = 0; key_ids[i] != NULL; i++)
    {
      g_auto(gpgme_key_t) key = NULL;

      /* Get the secret keys with the given key id */
      err = gpgme_get_key (context, key_ids[i], &key, 1);
      if (gpgme_err_code (err) == GPG_ERR_EOF)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED,
                              _("No gpg key found with ID %s (homedir: %s)"),
                              key_ids[i], homedir ? homedir : "<default>");
          return NULL;
        }
      else if (err != GPG_ERR_NO_ERROR)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED,
                              _("Unable to lookup key ID %s: %d"),
                              key_ids[i], err);
          return NULL;
        }

      /* Add the key to the context as a signer */
      if ((err = gpgme_signers_add (context, key)) != GPG_ERR_NO_ERROR)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Error signing commit: %d"), err);
          return NULL;
        }
    }

  {
    gsize len;
    const char *buf = g_bytes_get_data (data, &len);
    if ((err = gpgme_data_new_from_mem (&commit_buffer, buf, len, FALSE)) != GPG_ERR_NO_ERROR)
      {
        flatpak_gpgme_error_to_gio_error (err, error);
        g_prefix_error (error, "Failed to create buffer from commit file: ");
        return NULL;
      }
  }

  signature_buffer = flatpak_gpgme_data_output (tmp_signature_output);

  if ((err = gpgme_op_sign (context, commit_buffer, signature_buffer, GPGME_SIG_MODE_NORMAL))
      != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (err, error);
      g_prefix_error (error, "Failure signing commit file: ");
      return NULL;
    }

  if (!g_output_stream_close (tmp_signature_output, NULL, error))
    return NULL;

  signature_file = g_mapped_file_new_from_fd (tmpf.fd, FALSE, error);
  if (!signature_file)
    return NULL;

  return g_mapped_file_get_bytes (signature_file);
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
      return FALSE;
    }

  plain = read_gpg_buffer (plain_buffer, error);
  if (plain == NULL)
    return NULL;
  plain_bytes = g_string_free_to_bytes (g_steal_pointer (&plain));
  json = flatpak_json_from_bytes (plain_bytes, FLATPAK_TYPE_OCI_SIGNATURE, error);
  if (json == NULL)
    return FALSE;

  return (FlatpakOciSignature *) g_steal_pointer (&json);
}

