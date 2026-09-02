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

#include "flatpak-gpg-keys-private.h"
#include "flatpak-oci-signatures-private.h"
#include "flatpak-uri-private.h"
#include "flatpak-utils-private.h"

gboolean
flatpak_remote_has_gpg_key (OstreeRepo   *repo,
                            const char   *remote_name)
{
  g_autoptr(GFile) keyring_file = NULL;
  g_autofree char *keyring_name = NULL;

  keyring_name = g_strdup_printf ("%s.trustedkeys.gpg", remote_name);
  keyring_file = g_file_get_child (ostree_repo_get_path (repo), keyring_name);

  return g_file_query_exists (keyring_file, NULL);
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

/* WARNING: This verifies that the data is signed with the correct key, but
 * does not verify the payload matches what is being installed. The caller
 * must do that.
 */
static FlatpakOciSignature *
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

struct _FlatpakOciSignatures
{
  GPtrArray *signatures;
};

FlatpakOciSignatures *
flatpak_oci_signatures_new (void)
{
  FlatpakOciSignatures *self = g_new0 (FlatpakOciSignatures, 1);

  self->signatures = g_ptr_array_new_with_free_func ((GDestroyNotify)g_bytes_unref);

  return self;
}

void
flatpak_oci_signatures_free (FlatpakOciSignatures *self)
{
  g_clear_pointer (&self->signatures, g_ptr_array_unref);
  g_free (self);
}

void
flatpak_oci_signatures_add_signature (FlatpakOciSignatures *self,
                                      GBytes               *signature)
{
  g_ptr_array_add (self->signatures, g_bytes_ref (signature));
}

gboolean
flatpak_oci_signatures_load_from_dfd (FlatpakOciSignatures *self,
                                      int                   dfd,
                                      GCancellable         *cancellable,
                                      GError              **error)
{
  for (guint i = 1; i < G_MAXUINT; i++)
    {
      g_autofree char *filename = g_strdup_printf ("signature-%u", i);
      g_autoptr(GError) local_error = NULL;
      GBytes *signature;

      signature = flatpak_load_file_at (dfd, filename, cancellable, &local_error);
      if (local_error)
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            break;

          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      flatpak_oci_signatures_add_signature (self, g_steal_pointer (&signature));
    }

  return TRUE;
}

gboolean
flatpak_oci_signatures_save_to_dfd (FlatpakOciSignatures *self,
                                    int                   dfd,
                                    GCancellable         *cancellable,
                                    GError              **error)
{
  for (guint i = 0; i < self->signatures->len; i++)
    {
      g_autofree char *filename = g_strdup_printf ("signature-%u", i + 1);
      GBytes *signature = g_ptr_array_index (self->signatures, i);
      gconstpointer data;
      gsize size;

      data = g_bytes_get_data (signature, &size);

      if (!glnx_file_replace_contents_at (dfd, filename, data, size,
                                          0, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/*
 * Strip the tag and or digest off of a docker-reference. Regular expressions
 * based off of:
 *
 *   https://github.com/containers/image/tree/main/docker/reference
 *
 * For simplicity we don't do normalization - if the signature claims that it
 * matches 'ubuntu', we don't match turn that into docker.io/library/ubuntu
 * before matching against the domain and repository.
 */

#define TAG "[0-9A-Za-z_][0-9A-Za-z_-]{0,127}"
#define DIGEST "[A-Za-z][A-Za-z0-9]*(?:[-_+.][A-Za-z][A-Za-z0-9]*)*[:][[:xdigit:]]{32,}"
#define STRIP_TAG_AND_DIGEST_RE "^(.*?)(?::" TAG ")?" "(?:@" DIGEST ")?$"

static char *
reference_strip_tag_and_digest (const char *str)
{
  static gsize regex = 0;
  g_autoptr(GMatchInfo) match_info = NULL;
  gboolean matched;

  if (g_once_init_enter (&regex))
    {
      g_autoptr(GRegex) compiled = g_regex_new (STRIP_TAG_AND_DIGEST_RE,
                                                G_REGEX_DEFAULT,
                                                G_REGEX_MATCH_DEFAULT,
                                                NULL);
      g_assert (compiled);
      g_once_init_leave (&regex, (gsize) g_steal_pointer (&compiled));
    }

  matched = g_regex_match ((GRegex *) regex, str,
                           G_REGEX_MATCH_DEFAULT, &match_info);
  if (!matched)
    return NULL;

  return g_match_info_fetch (match_info, 1);
}

gboolean
flatpak_oci_signatures_verify (FlatpakOciSignatures *self,
                               OstreeRepo           *repo,
                               const char           *remote_name,
                               const char           *registry_url,
                               const char           *repository_name,
                               const char           *digest,
                               GError              **error)
{
  g_autoptr(GUri) uri = NULL;
  int port;
  g_autofree char *port_prefix = NULL;
  g_autofree char *expected_identity = NULL;

  if (!flatpak_remote_has_gpg_key (repo, remote_name))
    {
      g_info ("%s: no GPG key, skipping verification", remote_name);
      return TRUE;
    }

  uri = g_uri_parse (registry_url, FLATPAK_HTTP_URI_FLAGS | G_URI_FLAGS_PARSE_RELAXED, error);
  if (uri == NULL)
    return FALSE;

  port = g_uri_get_port (uri);
  if (port != -1 && port != 80 && port != 443)
    port_prefix = g_strdup_printf (":%d", g_uri_get_port (uri));

  expected_identity = g_strdup_printf ("%s%s/%s",
                                       g_uri_get_host (uri),
                                       port_prefix ? port_prefix : "",
                                       repository_name);

  for (guint i = 0; i < self->signatures->len; i++)
    {
      g_autoptr(FlatpakOciSignature) signature = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autofree char *stripped_reference = NULL;

      signature = flatpak_oci_verify_signature (repo, remote_name,
                                                g_ptr_array_index (self->signatures, i),
                                                &local_error);

      if (signature == NULL)
        {
          g_info ("Couldn't verify signature: %s", local_error->message);
          continue;
        }

      if (signature->critical.image.digest == NULL)
        {
          g_warning ("Signature is missing digest");
          continue;
        }

      if (strcmp (signature->critical.image.digest, digest) != 0)
        {
          g_warning ("Digest in signature (%s) does not match %s",
                     signature->critical.image.digest, digest);
          continue;
        }

      /* checked during demarshaling via mandatory properties */
      g_assert (signature->critical.identity.reference != NULL);

      stripped_reference = reference_strip_tag_and_digest (signature->critical.identity.reference);

      if (g_strcmp0 (expected_identity, stripped_reference) != 0)
        {
          g_info ("Identity in signature (%s) does not match %s",
                  signature->critical.identity.reference,
                  expected_identity);
          continue;
        }

      g_info ("%s: found valid signature for %s@%s",
              remote_name, expected_identity, digest);
      return TRUE;
    }

  return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED,
                             "%s: no valid signatures for %s@%s",
                             remote_name, expected_identity, digest);
}
