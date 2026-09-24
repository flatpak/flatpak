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

/* Convert a GPG key fingerprint (40-digit hex string) to a key ID (16-digit
 * hex string), which is its suffix. It would be nice if we could do all key
 * operations in terms of fingerprints, but the GPGME API doesn’t quite allow
 * that (yet?). */
static char *
fpr_to_keyid (const char *fpr)
{
  g_autofree char *keyid = NULL;

  g_assert (strlen (fpr) == 40);
  keyid = g_strdup (fpr + (40 - 16));
  g_assert (strlen (keyid) == 16);

  return g_steal_pointer (&keyid);
}

static int
sort_signatures_cb (const void *a,
                    const void *b)
{
  gpgme_key_sig_t sig_a = *((gpgme_key_sig_t *) a);
  gpgme_key_sig_t sig_b = *((gpgme_key_sig_t *) b);

  if (sig_a->timestamp != sig_b->timestamp)
    return sig_a->timestamp - sig_b->timestamp;

  /* In case of a timestamp tie (which can happen in the unit tests), reliably
   * order revocations after non-revocations as a failsafe. */
  if (sig_a->revoked != sig_b->revoked)
    return sig_a->revoked ? 1 : -1;

  /* Fall back to sorting by keyid, not because it’s relevant, but just to give
   * a stable sort. */
  return strcmp (sig_a->keyid, sig_b->keyid);
}

static GPtrArray *  /* (element-type gpgme_key_sig_t) (transfer container) */
sort_signatures (gpgme_key_sig_t signature)
{
  g_autoptr(GPtrArray) sorted = g_ptr_array_new ();

  for (gpgme_key_sig_t s = signature; s != NULL; s = s->next)
    g_ptr_array_add (sorted, s);

  g_ptr_array_sort (sorted, sort_signatures_cb);

  return g_steal_pointer (&sorted);
}

/**
 * flatpak_gpg_keys_validate_binding:
 * @repo: an OSTree repository
 * @remote_name: name of the remote the keys are for
 * @gpg_keys_file: GPG keyring to load
 * @cancellable: (nullable): a cancellable, or `NULL` to ignore
 * @error: return location for a `GError`
 *
 * Validate that the keys in @gpg_keys_file are suitable to be imported into the
 * @repo’s keyring and used to verify the signatures of flatpak commits.
 *
 * This can happen in two ways: either the existing primary key is updated to
 * contain new subkeys (perhaps with longer expiry dates), but the primary key
 * is still valid and is used to sign the new subkeys.
 *
 * Or the new key file contains a new primary key (and subkeys), with a
 * signature from an existing key to validate it
 *
 * @gpg_keys_file may contain multiple keys. All are processed identically, with
 * the caveat that they could form a chain of trust themselves which should be
 * allowed as long as it’s rooted as above. It may also contain revocation
 * certificates for keys, which are applied just after new keys are imported (so
 * a single update may contain a revocation certificate for K1, plus a new K2
 * which is signed by K1, and K2 will end up being trusted).
 *
 * If one of the keys in the file doesn’t pass validation, others may. The key
 * IDs of the valid keys will be returned, and the invalid ones will be ignored.
 * Typically this list of valid key IDs will then be passed to
 * ostree_repo_remote_gpg_import() to import them into the repository’s actual
 * keyring.
 *
 * If none of them are valid, or if @gpg_keys_file contains no keys, an error
 * will be set and `NULL` will be returned. If a non-`NULL` return value is
 * returned it is guaranteed to be a non-empty array.
 *
 * TOFU (Trust On First Use) is not considered as a measure of trust in this
 * code, since we have a trust authority we can query and hence we don’t need
 * to estimate trust from usage.
 *
 * The caller is responsible for preventing time-of-check-to-time-of-use
 * (TOCTTOU) races between calling this function and subsequently calling (for
 * example) ostree_repo_remote_gpg_import() afterwards on the same
 * @gpg_keys_file. For example, by locking the @repo around the operations and
 * ensuring that @gpg_keys_file cannot be modified after being passed to this
 * function.
 *
 * See the documentation for key rotation for details of the security model here.
 *
 * Returns: (array zero-terminated=1): a `NULL`-terminated array of key
 *   fingerprints which passed validation
 */
char **
flatpak_gpg_keys_validate_binding (OstreeRepo    *repo,
                                   const char    *remote_name,
                                   GFile         *gpg_keys_file,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  const unsigned int MAX_N_KEYS_TO_PROCESS = 20;  /* arbitrarily chosen; keep up to date with doc/flatpak-key-rotation.xml */
  g_auto(gpgme_ctx_t) context = NULL;
  gpgme_error_t gpg_error;
  g_auto(GLnxTmpDir) tmp_home_dir = { 0, };
  glnx_autofd int import_keyring_fd = -1;
  g_auto(gpgme_data_t) import_keyring_dh = NULL;
  gpgme_import_result_t import_result;
  g_autoptr(GHashTable) trust_root = NULL;  /* (element-type utf8) */
  g_autoptr(GHashTable) keys_to_check_trust = NULL;  /* (element-type utf8 gpgme_key_t) */
  g_autoptr(GPtrArray) successfully_imported_fingerprints = NULL;
  int64_t now_secs = g_get_real_time () / G_USEC_PER_SEC;

  g_return_val_if_fail (OSTREE_IS_REPO (repo), NULL);
  g_return_val_if_fail (remote_name != NULL, NULL);
  g_return_val_if_fail (G_IS_FILE (gpg_keys_file), NULL);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  gpg_error = gpgme_new (&context);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to create context: ");
      return NULL;
    }

  gpgme_set_offline (context, 1);

  /* Create a temporary context and import the existing
   * ${remote_name}.trustedkeys.gpg keyring into it. */
  if (!flatpak_gpgme_ctx_tmp_home_dir (context, &tmp_home_dir, repo, remote_name, cancellable, error))
    return NULL;

  /* Before we import the new keys, build a set of the existing keys which forms
   * our trust root. Ideally this would just contain fingerprints (40-digit hex)
   * but unfortunately we need to compare against gpgme_key_sig_t.keyid, which
   * is a key ID (16-digit hex), so insert that too. Key IDs are suffixes of
   * fingerprints. */
  trust_root = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  gpg_error = gpgme_set_keylist_mode (context, GPGME_KEYLIST_MODE_LOCAL);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to list existing keys: ");
      return NULL;
    }

  gpg_error = gpgme_op_keylist_start (context, NULL, 0);
  g_debug ("Building trust root:");

  while (gpg_error == GPG_ERR_NO_ERROR)
    {
      g_auto(gpgme_key_t) key = NULL;

      gpg_error = gpgme_op_keylist_next (context, &key);

      if (gpg_error != GPG_ERR_NO_ERROR)
        break;

      /* We don’t allow explicitly broken or untrusted keys in the trust root.
       * We do, however, allow expired keys, as they may have been used to sign
       * replacement keys *before* they expired. Expired means outdated, not
       * untrusted. If a key is not trusted, it should be revoked.
       *
       * While we check the ->disabled field, we never really expect it to be
       * set, as it comes from the trustdb.gpg, which flatpak doesn’t use (or
       * even store). It’s part of GPG’s web of trust model, and the only way it
       * could be set is if the user ran `--quick-set-ownertrust $KEYID disable`
       * on their local keyring (and then got flatpak to use the resulting
       * trustdb.gpg). We check it so we fail-safe. */
      if (key->revoked || key->disabled || key->invalid)
        continue;

      g_hash_table_add (trust_root, g_strdup (key->fpr));
      g_hash_table_add (trust_root, fpr_to_keyid (key->fpr));
      g_debug ("  %s%s", key->fpr, key->expired ? " (expired)" : "");
    }

  if (gpgme_err_code (gpg_error) != GPG_ERR_EOF)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to list existing keys: ");
      return NULL;
    }

  /* Open the new keys and import them into the temporary context. */
  if (!glnx_openat_rdonly (AT_FDCWD, flatpak_file_get_path_cached (gpg_keys_file),
                           FALSE, &import_keyring_fd, error))
    return NULL;

  gpg_error = gpgme_data_new_from_fd (&import_keyring_dh, import_keyring_fd);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to open keyring to import: ");
      return NULL;
    }

  gpg_error = gpgme_op_import (context, import_keyring_dh);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      flatpak_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to import keyring: ");
      return NULL;
    }

  /* Examine the import results and see what got imported. If it’s just new UIDs,
   * signatures, subkeys or secret keys, then GPG will have verified their
   * binding to the existing key, so there’s nothing more to check. If any new
   * keys have been added, add them to a list so we can check their chain of
   * trust later.
   *
   * We reference keys by their fingerprint, which is a hash of GPG key- and
   * implementation-dependent data. We don’t use the key ID (rightmost several
   * bytes of the fingerprint) or the keygrip (hash of protocol-independent data
   * which is mostly used internally by GPG). See
   * https://security.stackexchange.com/q/231295 to try and ease the confusion.
   *
   * As always, the canonical reference for anything to do with GPG is RFC 4880
   * and its follow-up RFCs. https://www.rfc-editor.org/info/rfc4880/
   */
  keys_to_check_trust = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) gpgme_key_unref);  /* mapping from fingerprint to gpgme_key_t */
  successfully_imported_fingerprints = g_ptr_array_new_with_free_func (g_free);
  import_result = gpgme_op_import_result (context);

  g_debug ("Results from considering import of keyring %s to update remote %s: "
           "%d keys considered, %d keys without UID, %d keys imported, %d keys unchanged, "
           "%d new UIDs, %d new subkeys, %d new signatures, %d new revocations, "
           "%d new secret keys, %d unchanged secret keys",
           flatpak_file_get_path_cached (gpg_keys_file),
           remote_name,
           import_result->considered,
           import_result->no_user_id,
           import_result->imported,
           import_result->unchanged,
           import_result->new_user_ids,
           import_result->new_sub_keys,
           import_result->new_signatures,
           import_result->new_revocations,
           import_result->secret_imported,
           import_result->secret_unchanged);

  for (gpgme_import_status_t import_status = import_result->imports;
       import_status != NULL;
       import_status = import_status->next)
    {
      if (import_status->result == GPG_ERR_NO_ERROR)
        {
          g_debug ("  Import of key %s succeeded with status %u",
                   import_status->fpr, import_status->status);

          if (import_status->status & GPGME_IMPORT_NEW)
            {
              g_hash_table_insert (keys_to_check_trust, g_strdup (import_status->fpr), NULL);
            }
          else
            {
              g_ptr_array_add (successfully_imported_fingerprints, g_strdup (import_status->fpr));
              g_hash_table_add (trust_root, g_strdup (import_status->fpr));
              g_hash_table_add (trust_root, fpr_to_keyid (import_status->fpr));
            }
        }
      else
        {
          g_debug ("  Import of key %s failed with error %d",
                   import_status->fpr, import_status->result);
        }
    }

  /* Check cancellation before we start the next (potentially) big operation. */
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  /* Are there too many new keys? Let’s be slightly paranoid and bail out if
   * the new keyring is unexpectedly big. Since it’s not signed, there is a
   * chance it’s malicious if it was served over HTTP (rather than HTTPS). */
  if (g_hash_table_size (keys_to_check_trust) > MAX_N_KEYS_TO_PROCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                   "Too many keys to check in keyring %s; is it corrupt?",
                   flatpak_file_get_path_cached (gpg_keys_file));
      return NULL;
    }

  /* Are there any new keys? If so, let’s validate their chain of trust back to
   * the existing keyring.
   *
   * Firstly, build a set of the gpgme_key_t objects representing the keys we
   * need to validate. Then iteratively check their signatures and add them to
   * the trust root (if they validate) until we make no more progress.
   *
   * An iterative approach is needed as the imported keys may be chained
   * together and we aren’t guaranteed to process them in depth order of the
   * tree. Iterating is fine as we expect a small number of keys in an import;
   * otherwise we’d be better off building the full signature tree and
   * validating that. */
  if (g_hash_table_size (keys_to_check_trust) > 0)
    {
      unsigned int prev_n_keys_to_check_trust;

      /* Note: We don’t pass GPGME_KEYLIST_MODE_VALIDATE here as it is
       * irrelevant — it triggers network based OCSP/CRL checks for S/MIME
       * certificates and is only implemented in gnupg2-smime. */
      gpg_error = gpgme_set_keylist_mode (context,
                                          GPGME_KEYLIST_MODE_LOCAL |
                                          GPGME_KEYLIST_MODE_SIGS);
      if (gpg_error != GPG_ERR_NO_ERROR)
        {
          flatpak_gpgme_error_to_gio_error (gpg_error, error);
          g_prefix_error (error, "Unable to list new keys: ");
          return NULL;
        }

      gpg_error = gpgme_op_keylist_start (context, NULL, 0);

      while (gpg_error == GPG_ERR_NO_ERROR)
        {
          g_auto(gpgme_key_t) key = NULL;
          gboolean was_not_in_table;
          const char *fingerprint;

          gpg_error = gpgme_op_keylist_next (context, &key);

          if (gpg_error != GPG_ERR_NO_ERROR)
            break;

          /* Is this a key we need to check? */
          fingerprint = key->fpr;
          if (!g_hash_table_contains (keys_to_check_trust, fingerprint))
            continue;

          was_not_in_table = g_hash_table_insert (keys_to_check_trust, g_strdup (fingerprint), g_steal_pointer (&key));
          g_assert (!was_not_in_table);
        }

      if (gpgme_err_code (gpg_error) != GPG_ERR_EOF)
        {
          flatpak_gpgme_error_to_gio_error (gpg_error, error);
          g_prefix_error (error, "Unable to list new keys: ");
          return NULL;
        }

      /* Now iteratively check the signatures until everything’s checked or
       * we’ve failed to make progress. */
      do
        {
          GHashTableIter iter;
          void *iter_key, *iter_value;

          /* Check cancellation before we dive into another iteration. */
          if (g_cancellable_set_error_if_cancelled (cancellable, error))
            return NULL;

          prev_n_keys_to_check_trust = g_hash_table_size (keys_to_check_trust);

          g_hash_table_iter_init (&iter, keys_to_check_trust);
          while (g_hash_table_iter_next (&iter, &iter_key, &iter_value))
            {
              const char *fingerprint = iter_key;
              gpgme_key_t key = iter_value;

              /* Somehow the key was listed by gpgme_op_import_*() but then
               * didn’t appear in the subsequent gpgme_op_keylist_*(). */
              if (key == NULL)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Key %s was considered for import but then not unexpectedly not included in the keylist from GPG",
                               fingerprint);
                  return NULL;
                }

              g_debug ("Checking chain of trust for key %s. Key details: "
                       "revoked: %u, expired: %u, disabled: %u, invalid: %u, "
                       "last update: %lu",
                       key->fpr,
                       key->revoked,
                       key->expired,
                       key->disabled,
                       key->invalid,
                       key->last_update);

              if (key->revoked || key->disabled || key->invalid)
                continue;
              else if (key->expired)
                g_debug ("  Processing expired key as apps may have been signed by it before expiration");

              /* Examine each of the key’s UIDs to check the signatures made on
               * the key. We don’t need to check key->subkeys because we’ll want
               * to either import all or none of them, and then let the normal
               * GPG logic at verify time work out which ones to trust. */
              for (gpgme_user_id_t uid = key->uids;
                   key != NULL && uid != NULL;
                   uid = uid->next)
                {
                  gboolean trust_this_uid = FALSE;

                  /* We ignore key->tofu as we have a trust authority we can
                   * use, so we don’t need to estimate trust from usage. */
                  g_debug ("  Found UID %s of key %s. UID details: revoked %u, "
                           "invalid %u, validity %u",
                           uid->uid,
                           key->fpr,
                           uid->revoked,
                           uid->invalid,
                           uid->validity);

                  if (uid->revoked || uid->invalid || uid->validity == GPGME_VALIDITY_NEVER)
                    continue;

                  /* Unfortunately GPGME doesn’t return the signature packets in
                   * timestamp order, which we need to work out when revocations
                   * happened. So sort them into a pointer array. */
                  g_autoptr(GPtrArray) sorted_signatures = sort_signatures (uid->signatures);

                  for (size_t i = 0; i < sorted_signatures->len; i++)
                    {
                      gpgme_key_sig_t signature = sorted_signatures->pdata[i];

                      g_debug ("  Found signature packet from key %s (UID %s) on UID %s of key %s. "
                               "Signature details: revoked %u, expired %u, invalid %u, "
                               "exportable %u, trust depth %u, trust value %u, algo %u, "
                               "timestamp %lu, expires %lu (now %lu), status %u",
                               signature->keyid,
                               signature->uid,
                               uid->uid,
                               key->fpr,
                               signature->revoked,
                               signature->expired,
                               signature->invalid,
                               signature->exportable,
                               signature->trust_depth,
                               signature->trust_value,
                               signature->pubkey_algo,
                               signature->timestamp,
                               signature->expires,
                               now_secs,
                               signature->status);

                      if (signature->revoked)
                        {
                          /* Revoked signatures don’t behave exactly how you’d
                           * expect, as we’re actually dealing with signature
                           * packets here, not a condensed listing of signatures.
                           * So we might see a valid gpgme_key_sig_t saying K1
                           * signed K2; and later in the same loop see another
                           * gpgme_key_sig_t which says K1’s signature of K2 has
                           * been revoked. If we see the packets in the opposite
                           * order it means there was a subsequent re-signing
                           * which supersedes the revocation. */
                          g_debug ("    Removing key as signature has been revoked");
                          trust_this_uid = FALSE;
                          continue;
                        }
                      else if (signature->expired ||
                               (signature->expires != 0 && signature->expires < now_secs) ||
                               signature->invalid || signature->status != GPG_ERR_NO_ERROR)
                        {
                          g_debug ("    Skipping signature as trust expired/timestamp expired/invalid/problematic");
                          continue;
                        }

                      if (g_hash_table_contains (trust_root, signature->keyid))
                        {
                          g_debug ("    Signing key is in trusted root");
                          trust_this_uid = TRUE;
                          continue;
                        }
                    }

                  /* If we have a later valid signature (which leads back to the
                   * trust root) than any invalid, untrusted or revoked
                   * signatures, then we can trust this UID and hence the whole
                   * key. */
                  if (trust_this_uid)
                    {
                      g_ptr_array_add (successfully_imported_fingerprints, g_strdup (key->fpr));
                      g_hash_table_add (trust_root, g_strdup (key->fpr));
                      g_hash_table_add (trust_root, fpr_to_keyid (key->fpr));
                      g_hash_table_iter_remove (&iter);
                      key = NULL;  /* @key is not valid after this point */
                      break;
                    }
                }

              /* Didn’t find a valid signature for any of the UIDs belonging to
               * this key? Keep iterating, maybe one will turn up once we’ve
               * built the trust root further. */
              if (key != NULL)
                g_debug ("  No chain of trust found yet");
            }
        }
      while (g_hash_table_size (keys_to_check_trust) > 0 &&
             prev_n_keys_to_check_trust > g_hash_table_size (keys_to_check_trust));

      /* If there are any keys left over then we cannot import them. */
      if (g_hash_table_size (keys_to_check_trust) > 0)
        {
          GHashTableIter iter;
          void *iter_key;

          g_debug ("Could not validate chain of trust for the following keys, so they will not be imported:");

          g_hash_table_iter_init (&iter, keys_to_check_trust);
          while (g_hash_table_iter_next (&iter, &iter_key, NULL))
            {
              const char *fingerprint = iter_key;
              g_debug ("  %s", fingerprint);
            }
        }
    }

  /* Return the list of successfully validated and imported keys. First, NULL
   * terminate it. It’s expected that this list will then be passed to
   * ostree_repo_remote_gpg_import(). */
  g_debug ("Recommending the import of the following keys as trusted:");
  for (size_t i = 0; i < successfully_imported_fingerprints->len; i++)
    g_debug ("  %s", (const char *) successfully_imported_fingerprints->pdata[i]);

  g_ptr_array_add (successfully_imported_fingerprints, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&successfully_imported_fingerprints), FALSE);
}
