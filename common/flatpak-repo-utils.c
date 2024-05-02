/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 1995-1998 Free Software Foundation, Inc.
 * Copyright © 2014-2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"
#include "flatpak-repo-utils-private.h"

#include <gio/gunixinputstream.h>

#include <glib/gi18n-lib.h>

#include "flatpak-utils-private.h"
#include "flatpak-variant-private.h"
#include "flatpak-variant-impl-private.h"
#include "flatpak-xml-utils-private.h"

gboolean
flatpak_repo_set_title (OstreeRepo *repo,
                        const char *title,
                        GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (title)
    g_key_file_set_string (config, "flatpak", "title", title);
  else
    g_key_file_remove_key (config, "flatpak", "title", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_comment (OstreeRepo *repo,
                          const char *comment,
                          GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (comment)
    g_key_file_set_string (config, "flatpak", "comment", comment);
  else
    g_key_file_remove_key (config, "flatpak", "comment", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_description (OstreeRepo *repo,
                              const char *description,
                              GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (description)
    g_key_file_set_string (config, "flatpak", "description", description);
  else
    g_key_file_remove_key (config, "flatpak", "description", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}


gboolean
flatpak_repo_set_icon (OstreeRepo *repo,
                       const char *icon,
                       GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (icon)
    g_key_file_set_string (config, "flatpak", "icon", icon);
  else
    g_key_file_remove_key (config, "flatpak", "icon", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_homepage (OstreeRepo *repo,
                           const char *homepage,
                           GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (homepage)
    g_key_file_set_string (config, "flatpak", "homepage", homepage);
  else
    g_key_file_remove_key (config, "flatpak", "homepage", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_redirect_url (OstreeRepo *repo,
                               const char *redirect_url,
                               GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (redirect_url)
    g_key_file_set_string (config, "flatpak", "redirect-url", redirect_url);
  else
    g_key_file_remove_key (config, "flatpak", "redirect-url", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_authenticator_name (OstreeRepo *repo,
                                     const char *authenticator_name,
                                     GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (authenticator_name)
    g_key_file_set_string (config, "flatpak", "authenticator-name", authenticator_name);
  else
    g_key_file_remove_key (config, "flatpak", "authenticator-name", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_authenticator_install (OstreeRepo *repo,
                                        gboolean authenticator_install,
                                        GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  g_key_file_set_boolean (config, "flatpak", "authenticator-install", authenticator_install);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_authenticator_option (OstreeRepo *repo,
                                       const char *key,
                                       const char *value,
                                       GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;
  g_autofree char *full_key = g_strdup_printf ("authenticator-options.%s", key);

  config = ostree_repo_copy_config (repo);

  if (value)
    g_key_file_set_string (config, "flatpak", full_key, value);
  else
    g_key_file_remove_key (config, "flatpak", full_key, NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_deploy_collection_id (OstreeRepo *repo,
                                       gboolean    deploy_collection_id,
                                       GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);
  g_key_file_set_boolean (config, "flatpak", "deploy-collection-id", deploy_collection_id);
  return ostree_repo_write_config (repo, config, error);
}

gboolean
flatpak_repo_set_deploy_sideload_collection_id (OstreeRepo *repo,
                                           gboolean    deploy_collection_id,
                                           GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);
  g_key_file_set_boolean (config, "flatpak", "deploy-sideload-collection-id", deploy_collection_id);
  return ostree_repo_write_config (repo, config, error);
}

gboolean
flatpak_repo_set_gpg_keys (OstreeRepo *repo,
                           GBytes     *bytes,
                           GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;
  g_autofree char *value_base64 = NULL;

  config = ostree_repo_copy_config (repo);

  value_base64 = g_base64_encode (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));

  g_key_file_set_string (config, "flatpak", "gpg-keys", value_base64);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_default_branch (OstreeRepo *repo,
                                 const char *branch,
                                 GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (branch)
    g_key_file_set_string (config, "flatpak", "default-branch", branch);
  else
    g_key_file_remove_key (config, "flatpak", "default-branch", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_collection_id (OstreeRepo *repo,
                                const char *collection_id,
                                GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  if (!ostree_repo_set_collection_id (repo, collection_id, error))
    return FALSE;

  config = ostree_repo_copy_config (repo);
  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_summary_history_length (OstreeRepo *repo,
                                         guint       length,
                                         GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (length)
    g_key_file_set_integer (config, "flatpak", "summary-history-length", length);
  else
    g_key_file_remove_key (config, "flatpak", "summary-history-length", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

guint
flatpak_repo_get_summary_history_length (OstreeRepo *repo)
{
  GKeyFile *config = ostree_repo_get_config (repo);
  int length;

  length = g_key_file_get_integer (config, "flatpak", "sumary-history-length", NULL);

  if (length <= 0)
    return FLATPAK_SUMMARY_HISTORY_LENGTH_DEFAULT;

  return length;
}

GVariant *
flatpak_commit_get_extra_data_sources (GVariant *commitv,
                                       GError  **error)
{
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autoptr(GVariant) extra_data_sources = NULL;

  commit_metadata = g_variant_get_child_value (commitv, 0);
  extra_data_sources = g_variant_lookup_value (commit_metadata,
                                               "xa.extra-data-sources",
                                               G_VARIANT_TYPE ("a(ayttays)"));

  if (extra_data_sources == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("No extra data sources"));
      return NULL;
    }

  return g_steal_pointer (&extra_data_sources);
}

GVariant *
flatpak_repo_get_extra_data_sources (OstreeRepo   *repo,
                                     const char   *rev,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GVariant) commitv = NULL;

  if (!ostree_repo_load_variant (repo,
                                 OSTREE_OBJECT_TYPE_COMMIT,
                                 rev, &commitv, error))
    return NULL;

  return flatpak_commit_get_extra_data_sources (commitv, error);
}

void
flatpak_repo_parse_extra_data_sources (GVariant      *extra_data_sources,
                                       int            index,
                                       const char   **name,
                                       guint64       *download_size,
                                       guint64       *installed_size,
                                       const guchar **sha256,
                                       const char   **uri)
{
  g_autoptr(GVariant) sha256_v = NULL;
  g_variant_get_child (extra_data_sources, index, "(^aytt@ay&s)",
                       name,
                       download_size,
                       installed_size,
                       &sha256_v,
                       uri);

  if (download_size)
    *download_size = GUINT64_FROM_BE (*download_size);

  if (installed_size)
    *installed_size = GUINT64_FROM_BE (*installed_size);

  if (sha256)
    *sha256 = ostree_checksum_bytes_peek (sha256_v);
}

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

static gboolean
_flatpak_repo_collect_sizes (OstreeRepo   *repo,
                             GFile        *file,
                             GFileInfo    *file_info,
                             guint64      *installed_size,
                             guint64      *download_size,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  GFileInfo *child_info_tmp;
  g_autoptr(GError) temp_error = NULL;

  if (file_info != NULL && g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      const char *checksum = ostree_repo_file_get_checksum (OSTREE_REPO_FILE (file));
      guint64 obj_size;
      guint64 file_size = g_file_info_get_size (file_info);

      if (installed_size)
        *installed_size += ((file_size + 511) / 512) * 512;

      if (download_size)
        {
          g_autoptr(GInputStream) input = NULL;
          GInputStream *base_input;
          g_autoptr(GError) local_error = NULL;

          if (!ostree_repo_query_object_storage_size (repo,
                                                      OSTREE_OBJECT_TYPE_FILE, checksum,
                                                      &obj_size, cancellable, &local_error))
            {
              int fd;
              struct stat stbuf;

              /* Ostree does not look at the staging directory when querying storage
                 size, so may return a NOT_FOUND error here. We work around this
                 by loading the object and walking back until we find the original
                 fd which we can fstat(). */
              if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                return FALSE;

              if (!ostree_repo_load_file (repo, checksum,  &input, NULL, NULL, NULL, error))
                return FALSE;

              base_input = input;
              while (G_IS_FILTER_INPUT_STREAM (base_input))
                base_input = g_filter_input_stream_get_base_stream (G_FILTER_INPUT_STREAM (base_input));

              if (!G_IS_UNIX_INPUT_STREAM (base_input))
                return flatpak_fail (error, "Unable to find size of commit %s, not an unix stream", checksum);

              fd = g_unix_input_stream_get_fd (G_UNIX_INPUT_STREAM (base_input));

              if (fstat (fd, &stbuf) != 0)
                return glnx_throw_errno_prefix (error, "Can't find commit size: ");

              obj_size = stbuf.st_size;
            }

          *download_size += obj_size;
        }
    }

  if (file_info == NULL || g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      dir_enum = g_file_enumerate_children (file, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, error);
      if (!dir_enum)
        return FALSE;


      while ((child_info_tmp = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
        {
          g_autoptr(GFileInfo) child_info = child_info_tmp;
          const char *name = g_file_info_get_name (child_info);
          g_autoptr(GFile) child = g_file_get_child (file, name);

          if (!_flatpak_repo_collect_sizes (repo, child, child_info, installed_size, download_size, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
flatpak_repo_collect_sizes (OstreeRepo   *repo,
                            GFile        *root,
                            guint64      *installed_size,
                            guint64      *download_size,
                            GCancellable *cancellable,
                            GError      **error)
{
  /* Initialize the sums */
  if (installed_size)
    *installed_size = 0;
  if (download_size)
    *download_size = 0;
  return _flatpak_repo_collect_sizes (repo, root, NULL, installed_size, download_size, cancellable, error);
}

static void
flatpak_repo_collect_extra_data_sizes (OstreeRepo *repo,
                                       const char *rev,
                                       guint64    *installed_size,
                                       guint64    *download_size)
{
  g_autoptr(GVariant) extra_data_sources = NULL;
  gsize n_extra_data;
  int i;

  extra_data_sources = flatpak_repo_get_extra_data_sources (repo, rev, NULL, NULL);
  if (extra_data_sources == NULL)
    return;

  n_extra_data = g_variant_n_children (extra_data_sources);
  if (n_extra_data == 0)
    return;

  for (i = 0; i < n_extra_data; i++)
    {
      guint64 extra_download_size;
      guint64 extra_installed_size;

      flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                             NULL,
                                             &extra_download_size,
                                             &extra_installed_size,
                                             NULL, NULL);
      if (installed_size)
        *installed_size += extra_installed_size;
      if (download_size)
        *download_size += extra_download_size;
    }
}

/* Loads the old compat summary file from a local repo */
GVariant *
flatpak_repo_load_summary (OstreeRepo *repo,
                           GError    **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) bytes = NULL;

  fd = openat (ostree_repo_get_dfd (repo), "summary", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return NULL;

  bytes = g_mapped_file_get_bytes (mfile);

  return g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, TRUE));
}

GVariant *
flatpak_repo_load_summary_index (OstreeRepo *repo,
                                 GError    **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) bytes = NULL;

  fd = openat (ostree_repo_get_dfd (repo), "summary.idx", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return NULL;

  bytes = g_mapped_file_get_bytes (mfile);

  return g_variant_ref_sink (g_variant_new_from_bytes (FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT, bytes, TRUE));
}

static gboolean
flatpak_repo_save_compat_summary (OstreeRepo   *repo,
                                  GVariant     *summary,
                                  time_t       *out_old_sig_mtime,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  struct stat stbuf;
  time_t old_sig_mtime = 0;
  GLnxFileReplaceFlags flags;

  flags = GLNX_FILE_REPLACE_INCREASING_MTIME;
  if (ostree_repo_get_disable_fsync (repo))
    flags |= GLNX_FILE_REPLACE_NODATASYNC;
  else
    flags |= GLNX_FILE_REPLACE_DATASYNC_NEW;

  if (!glnx_file_replace_contents_at (repo_dfd, "summary",
                                      g_variant_get_data (summary),
                                      g_variant_get_size (summary),
                                      flags,
                                      cancellable, error))
    return FALSE;

  if (fstatat (repo_dfd, "summary.sig", &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    old_sig_mtime = stbuf.st_mtime;

  if (unlinkat (repo_dfd, "summary.sig", 0) != 0 &&
      G_UNLIKELY (errno != ENOENT))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  *out_old_sig_mtime = old_sig_mtime;
  return TRUE;
}

static gboolean
flatpak_repo_save_summary_index (OstreeRepo   *repo,
                                 GVariant     *index,
                                 const char   *index_digest,
                                 GBytes       *index_sig,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  GLnxFileReplaceFlags  flags;

  if (index == NULL)
    {
      if (unlinkat (repo_dfd, "summary.idx", 0) != 0 &&
          G_UNLIKELY (errno != ENOENT))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
      if (unlinkat (repo_dfd, "summary.idx.sig", 0) != 0 &&
          G_UNLIKELY (errno != ENOENT))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      return TRUE;
    }

  flags = GLNX_FILE_REPLACE_INCREASING_MTIME;
  if (ostree_repo_get_disable_fsync (repo))
    flags |= GLNX_FILE_REPLACE_NODATASYNC;
  else
    flags |= GLNX_FILE_REPLACE_DATASYNC_NEW;

  if (index_sig)
    {
      g_autofree char *path = g_strconcat ("summaries/", index_digest, ".idx.sig", NULL);

      if (!glnx_shutil_mkdir_p_at (repo_dfd, "summaries",
                                   0775, cancellable, error))
        return FALSE;

      if (!glnx_file_replace_contents_at (repo_dfd, path,
                                          g_bytes_get_data (index_sig, NULL),
                                          g_bytes_get_size (index_sig),
                                          flags,
                                          cancellable, error))
        return FALSE;
    }

  if (!glnx_file_replace_contents_at (repo_dfd, "summary.idx",
                                      g_variant_get_data (index),
                                      g_variant_get_size (index),
                                      flags,
                                      cancellable, error))
    return FALSE;

  /* Update the non-indexed summary.idx.sig file that was introduced in 1.9.1 but
   * was made unnecessary in 1.9.3. Lets keep it for a while until everyone updates
   */
  if (index_sig)
    {
      if (!glnx_file_replace_contents_at (repo_dfd, "summary.idx.sig",
                                          g_bytes_get_data (index_sig, NULL),
                                          g_bytes_get_size (index_sig),
                                          flags,
                                          cancellable, error))
        return FALSE;
    }
  else
    {
      if (unlinkat (repo_dfd, "summary.idx.sig", 0) != 0 &&
          G_UNLIKELY (errno != ENOENT))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  return TRUE;
}

GVariant *
flatpak_repo_load_digested_summary (OstreeRepo *repo,
                                   const char *digest,
                                   GError    **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GBytes) compressed_bytes = NULL;
  g_autofree char *path = NULL;
  g_autofree char *filename = NULL;

  filename = g_strconcat (digest, ".gz", NULL);
  path = g_build_filename ("summaries", filename, NULL);

  fd = openat (ostree_repo_get_dfd (repo), path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return NULL;

  compressed_bytes = g_mapped_file_get_bytes (mfile);
  bytes = flatpak_zlib_decompress_bytes (compressed_bytes, error);
  if (bytes == NULL)
    return NULL;

  return g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, TRUE));
}

static char *
flatpak_repo_save_digested_summary (OstreeRepo   *repo,
                                    const char   *name,
                                    GVariant     *summary,
                                    GCancellable *cancellable,
                                    GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  g_autofree char *digest = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GBytes) data = NULL;
  g_autoptr(GBytes) compressed_data = NULL;
  struct stat stbuf;

  if (!glnx_shutil_mkdir_p_at (repo_dfd, "summaries",
                               0775,
                               cancellable,
                               error))
    return NULL;

  digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                        g_variant_get_data (summary),
                                        g_variant_get_size (summary));
  filename = g_strconcat (digest, ".gz", NULL);

  path = g_build_filename ("summaries", filename, NULL);

  /* Check for pre-existing (non-truncated) copy and avoid re-writing it */
  if (fstatat (repo_dfd, path, &stbuf, 0) == 0 &&
      stbuf.st_size != 0)
    {
      g_info ("Reusing digested summary at %s for %s", path, name);
      return g_steal_pointer (&digest);
    }

  data = g_variant_get_data_as_bytes (summary);
  compressed_data = flatpak_zlib_compress_bytes (data, -1, error);
  if (compressed_data == NULL)
    return NULL;

  if (!glnx_file_replace_contents_at (repo_dfd, path,
                                      g_bytes_get_data (compressed_data, NULL),
                                      g_bytes_get_size (compressed_data),
                                      ostree_repo_get_disable_fsync (repo) ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    return NULL;

  g_info ("Wrote digested summary at %s for %s", path, name);
  return g_steal_pointer (&digest);
}

static gboolean
flatpak_repo_save_digested_summary_delta (OstreeRepo   *repo,
                                          const char   *from_digest,
                                          const char   *to_digest,
                                          GBytes       *delta,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  g_autofree char *path = NULL;
  g_autofree char *filename = g_strconcat (from_digest, "-", to_digest, ".delta", NULL);
  struct stat stbuf;

  if (!glnx_shutil_mkdir_p_at (repo_dfd, "summaries",
                               0775,
                               cancellable,
                               error))
    return FALSE;

  path = g_build_filename ("summaries", filename, NULL);

  /* Check for pre-existing copy of same size and avoid re-writing it */
  if (fstatat (repo_dfd, path, &stbuf, 0) == 0 &&
      stbuf.st_size == g_bytes_get_size (delta))
    {
      g_info ("Reusing digested summary-diff for %s", filename);
      return TRUE;
    }

  if (!glnx_file_replace_contents_at (repo_dfd, path,
                                      g_bytes_get_data (delta, NULL),
                                      g_bytes_get_size (delta),
                                      ostree_repo_get_disable_fsync (repo) ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    return FALSE;

  g_info ("Wrote digested summary delta at %s", path);
  return TRUE;
}

typedef struct
{
  guint64    installed_size;
  guint64    download_size;
  char      *metadata_contents;
  GPtrArray *subsets;
  GVariant  *sparse_data;
  gsize      commit_size;
  guint64    commit_timestamp;
} CommitData;

static void
commit_data_free (gpointer data)
{
  CommitData *rev_data = data;

  if (rev_data->subsets)
    g_ptr_array_unref (rev_data->subsets);
  g_free (rev_data->metadata_contents);
  if (rev_data->sparse_data)
    g_variant_unref (rev_data->sparse_data);
  g_free (rev_data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CommitData, commit_data_free);

static GHashTable *
commit_data_cache_new (void)
{
  return g_hash_table_new_full (g_str_hash, g_str_equal, g_free, commit_data_free);
}

static GHashTable *
populate_commit_data_cache (OstreeRepo *repo,
                            GVariant *index_v)
{

  VarSummaryIndexRef index = var_summary_index_from_gvariant (index_v);
  VarMetadataRef index_metadata = var_summary_index_get_metadata (index);
  VarSummaryIndexSubsummariesRef subsummaries = var_summary_index_get_subsummaries (index);
  gsize n_subsummaries = var_summary_index_subsummaries_get_length (subsummaries);
  guint32 cache_version;
  g_autoptr(GHashTable) commit_data_cache = commit_data_cache_new ();

  cache_version = GUINT32_FROM_LE (var_metadata_lookup_uint32 (index_metadata, "xa.cache-version", 0));
  if (cache_version < FLATPAK_XA_CACHE_VERSION)
    {
      /* Need to re-index to get all data */
      g_info ("Old summary cache version %d, not using cache", cache_version);
      return NULL;
    }

  for (gsize i = 0; i < n_subsummaries; i++)
    {
      VarSummaryIndexSubsummariesEntryRef entry = var_summary_index_subsummaries_get_at (subsummaries, i);
      const char *name = var_summary_index_subsummaries_entry_get_key (entry);
      const char *s;
      g_autofree char *subset = NULL;
      VarSubsummaryRef subsummary = var_summary_index_subsummaries_entry_get_value (entry);
      gsize checksum_bytes_len;
      const guchar *checksum_bytes;
      g_autofree char *digest = NULL;
      g_autoptr(GVariant) summary_v = NULL;
      VarSummaryRef summary;
      VarRefMapRef ref_map;
      gsize n_refs;

      checksum_bytes = var_subsummary_peek_checksum (subsummary, &checksum_bytes_len);
      if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
        {
          g_info ("Invalid checksum for digested summary, not using cache");
          return NULL;
        }
      digest = ostree_checksum_from_bytes (checksum_bytes);

      s = strrchr (name, '-');
      if (s != NULL)
        subset = g_strndup (name, s - name);
      else
        subset = g_strdup ("");

      summary_v = flatpak_repo_load_digested_summary (repo, digest, NULL);
      if (summary_v == NULL)
        {
          g_info ("Failed to load digested summary %s, not using cache", digest);
          return NULL;
        }

      /* Note that all summaries refered to by the index is in new format */
      summary = var_summary_from_gvariant (summary_v);
      ref_map = var_summary_get_ref_map (summary);
      n_refs = var_ref_map_get_length (ref_map);
      for (gsize j = 0; j < n_refs; j++)
        {
          VarRefMapEntryRef e = var_ref_map_get_at (ref_map, j);
          const char *ref = var_ref_map_entry_get_ref (e);
          VarRefInfoRef info = var_ref_map_entry_get_info (e);
          VarMetadataRef commit_metadata = var_ref_info_get_metadata (info);
          guint64 commit_size = var_ref_info_get_commit_size (info);
          const guchar *commit_bytes;
          gsize commit_bytes_len;
          g_autofree char *rev = NULL;
          CommitData *rev_data;
          VarVariantRef xa_data_v;
          VarCacheDataRef xa_data;

          if (!flatpak_is_app_runtime_or_appstream_ref (ref))
            continue;

          commit_bytes = var_ref_info_peek_checksum (info, &commit_bytes_len);
          if (G_UNLIKELY (commit_bytes_len != OSTREE_SHA256_DIGEST_LEN))
            continue;

          if (!var_metadata_lookup (commit_metadata, "xa.data", NULL, &xa_data_v) ||
              !var_variant_is_type (xa_data_v, G_VARIANT_TYPE ("(tts)")))
            {
              g_info ("Missing xa.data for ref %s, not using cache", ref);
              return NULL;
            }

          xa_data = var_cache_data_from_variant (xa_data_v);

          rev = ostree_checksum_from_bytes (commit_bytes);
          rev_data = g_hash_table_lookup (commit_data_cache, rev);
          if (rev_data == NULL)
            {
              g_auto(GVariantBuilder) sparse_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
              g_variant_builder_init (&sparse_builder, G_VARIANT_TYPE_VARDICT);
              gboolean has_sparse = FALSE;

              rev_data = g_new0 (CommitData, 1);
              rev_data->installed_size = var_cache_data_get_installed_size (xa_data);
              rev_data->download_size = var_cache_data_get_download_size (xa_data);
              rev_data->metadata_contents = g_strdup (var_cache_data_get_metadata (xa_data));
              rev_data->commit_size = commit_size;
              rev_data->commit_timestamp = GUINT64_FROM_BE (var_metadata_lookup_uint64 (commit_metadata, OSTREE_COMMIT_TIMESTAMP2, 0));

              /* Get sparse data */
              gsize len = var_metadata_get_length (commit_metadata);
              for (gsize k = 0; k < len; k++)
                {
                  VarMetadataEntryRef m = var_metadata_get_at (commit_metadata, k);
                  const char *m_key = var_metadata_entry_get_key (m);
                  if (!g_str_has_prefix (m_key, "ot.") &&
                      !g_str_has_prefix (m_key, "ostree.") &&
                      strcmp (m_key, "xa.data") != 0)
                    {
                      VarVariantRef v = var_metadata_entry_get_value (m);
                      g_autoptr(GVariant) vv = g_variant_ref_sink (var_variant_dup_to_gvariant (v));
                      g_autoptr(GVariant) child = g_variant_get_child_value (vv, 0);
                      g_variant_builder_add (&sparse_builder, "{sv}", m_key, child);
                      has_sparse = TRUE;
                    }
                }

              if (has_sparse)
                rev_data->sparse_data = g_variant_ref_sink (g_variant_builder_end (&sparse_builder));

              g_hash_table_insert (commit_data_cache, g_strdup (rev), (CommitData *)rev_data);
            }

          if (*subset != 0)
            {
              if (rev_data->subsets == NULL)
                rev_data->subsets = g_ptr_array_new_with_free_func (g_free);

              if (!flatpak_g_ptr_array_contains_string (rev_data->subsets, subset))
                g_ptr_array_add (rev_data->subsets, g_strdup (subset));
            }
        }
    }

  return g_steal_pointer (&commit_data_cache);
}

static CommitData *
read_commit_data (OstreeRepo   *repo,
                  const char   *ref,
                  const char   *rev,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) metadata = NULL;
  guint64 installed_size = 0;
  guint64 download_size = 0;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *commit = NULL;
  g_autoptr(GVariant) commit_v = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autoptr(GPtrArray) subsets = NULL;
  CommitData *rev_data;
  const char *eol = NULL;
  const char *eol_rebase = NULL;
  int token_type = -1;
  g_autoptr(GVariant) extra_data_sources = NULL;
  guint32 n_extra_data = 0;
  guint64 total_extra_data_download_size = 0;
  g_autoptr(GVariantIter) subsets_iter = NULL;

  if (!ostree_repo_read_commit (repo, rev, &root, &commit, NULL, error))
    return NULL;

  if (!ostree_repo_load_commit (repo, commit, &commit_v, NULL, error))
    return NULL;

  commit_metadata = g_variant_get_child_value (commit_v, 0);
  if (!g_variant_lookup (commit_metadata, "xa.metadata", "s", &metadata_contents))
    {
      metadata = g_file_get_child (root, "metadata");
      if (!g_file_load_contents (metadata, cancellable, &metadata_contents, NULL, NULL, NULL))
        metadata_contents = g_strdup ("");
    }

  if (g_variant_lookup (commit_metadata, "xa.installed-size", "t", &installed_size) &&
      g_variant_lookup (commit_metadata, "xa.download-size", "t", &download_size))
    {
      installed_size = GUINT64_FROM_BE (installed_size);
      download_size = GUINT64_FROM_BE (download_size);
    }
  else
    {
      if (!flatpak_repo_collect_sizes (repo, root, &installed_size, &download_size, cancellable, error))
        return NULL;
    }

  if (g_variant_lookup (commit_metadata, "xa.subsets", "as", &subsets_iter))
    {
      const char *subset;
      subsets = g_ptr_array_new_with_free_func (g_free);
      while (g_variant_iter_next (subsets_iter, "&s", &subset))
        g_ptr_array_add (subsets, g_strdup (subset));
    }

  flatpak_repo_collect_extra_data_sizes (repo, rev, &installed_size, &download_size);

  rev_data = g_new0 (CommitData, 1);
  rev_data->installed_size = installed_size;
  rev_data->download_size = download_size;
  rev_data->metadata_contents = g_steal_pointer (&metadata_contents);
  rev_data->subsets = g_steal_pointer (&subsets);
  rev_data->commit_size = g_variant_get_size (commit_v);
  rev_data->commit_timestamp = ostree_commit_get_timestamp (commit_v);

  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "&s", &eol);
  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, "&s", &eol_rebase);
  if (g_variant_lookup (commit_metadata, "xa.token-type", "i", &token_type))
    token_type = GINT32_FROM_LE(token_type);

  extra_data_sources = flatpak_commit_get_extra_data_sources (commit_v, NULL);
  if (extra_data_sources)
    {
      n_extra_data = g_variant_n_children (extra_data_sources);
      for (int i = 0; i < n_extra_data; i++)
        {
          guint64 extra_download_size;
          flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                                 NULL,
                                                 &extra_download_size,
                                                 NULL,
                                                 NULL,
                                                 NULL);
          total_extra_data_download_size += extra_download_size;
        }
    }

  if (eol || eol_rebase || token_type >= 0 || n_extra_data > 0)
    {
      g_auto(GVariantBuilder) sparse_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      g_variant_builder_init (&sparse_builder, G_VARIANT_TYPE_VARDICT);
      if (eol)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE, g_variant_new_string (eol));
      if (eol_rebase)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE, g_variant_new_string (eol_rebase));
      if (token_type >= 0)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_TOKEN_TYPE, g_variant_new_int32 (GINT32_TO_LE(token_type)));
      if (n_extra_data > 0)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_EXTRA_DATA_SIZE,
                               g_variant_new ("(ut)", GUINT32_TO_LE(n_extra_data), GUINT64_TO_LE(total_extra_data_download_size)));

      rev_data->sparse_data = g_variant_ref_sink (g_variant_builder_end (&sparse_builder));
    }

  return rev_data;
}

static void
_ostree_parse_delta_name (const char *delta_name,
                          char      **out_from,
                          char      **out_to)
{
  g_auto(GStrv) parts = g_strsplit (delta_name, "-", 2);

  if (parts[0] && parts[1])
    {
      *out_from = g_steal_pointer (&parts[0]);
      *out_to = g_steal_pointer (&parts[1]);
    }
  else
    {
      *out_from = NULL;
      *out_to = g_steal_pointer (&parts[0]);
    }
}

static GString *
static_delta_path_base (const char *dir,
                        const char *from,
                        const char *to)
{
  guint8 csum_to[OSTREE_SHA256_DIGEST_LEN];
  char to_b64[44];
  guint8 csum_to_copy[OSTREE_SHA256_DIGEST_LEN];
  GString *ret = g_string_new (dir);

  ostree_checksum_inplace_to_bytes (to, csum_to);
  ostree_checksum_b64_inplace_from_bytes (csum_to, to_b64);
  ostree_checksum_b64_inplace_to_bytes (to_b64, csum_to_copy);

  g_assert (memcmp (csum_to, csum_to_copy, OSTREE_SHA256_DIGEST_LEN) == 0);

  if (from != NULL)
    {
      guint8 csum_from[OSTREE_SHA256_DIGEST_LEN];
      char from_b64[44];

      ostree_checksum_inplace_to_bytes (from, csum_from);
      ostree_checksum_b64_inplace_from_bytes (csum_from, from_b64);

      g_string_append_c (ret, from_b64[0]);
      g_string_append_c (ret, from_b64[1]);
      g_string_append_c (ret, '/');
      g_string_append (ret, from_b64 + 2);
      g_string_append_c (ret, '-');
    }

  g_string_append_c (ret, to_b64[0]);
  g_string_append_c (ret, to_b64[1]);
  if (from == NULL)
    g_string_append_c (ret, '/');
  g_string_append (ret, to_b64 + 2);

  return ret;
}

static char *
_ostree_get_relative_static_delta_path (const char *from,
                                        const char *to,
                                        const char *target)
{
  GString *ret = static_delta_path_base ("deltas/", from, to);

  if (target != NULL)
    {
      g_string_append_c (ret, '/');
      g_string_append (ret, target);
    }

  return g_string_free (ret, FALSE);
}

static char *
_ostree_get_relative_static_delta_superblock_path (const char        *from,
                                                   const char        *to)
{
  return _ostree_get_relative_static_delta_path (from, to, "superblock");
}

static GVariant *
_ostree_repo_static_delta_superblock_digest (OstreeRepo    *repo,
                                             const char    *from,
                                             const char    *to,
                                             GCancellable  *cancellable,
                                             GError       **error)
{
  g_autofree char *superblock = _ostree_get_relative_static_delta_superblock_path ((from && from[0]) ? from : NULL, to);
  glnx_autofd int fd = -1;
  guint8 digest[OSTREE_SHA256_DIGEST_LEN];
  gsize len;
  gpointer data = NULL;

  if (!glnx_openat_rdonly (ostree_repo_get_dfd (repo), superblock, TRUE, &fd, error))
    return NULL;

  g_autoptr(GBytes) superblock_content = glnx_fd_readall_bytes (fd, cancellable, error);
  if (!superblock_content)
    return NULL;

  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, g_bytes_get_data (superblock_content, NULL), g_bytes_get_size (superblock_content));
  len = sizeof digest;
  g_checksum_get_digest (checksum, digest, &len);

  data = g_memdup2 (digest, len);
  return g_variant_new_from_data (G_VARIANT_TYPE ("ay"),
                                  data, len,
                                  FALSE, g_free, data);
}

typedef enum {
  DIFF_OP_KIND_RESUSE_OLD,
  DIFF_OP_KIND_SKIP_OLD,
  DIFF_OP_KIND_DATA,
} DiffOpKind;

typedef struct {
  DiffOpKind kind;
  gsize size;
} DiffOp;

typedef struct {
  const guchar *old_data;
  const guchar *new_data;

  GArray *ops;
  GArray *data;

  gsize last_old_offset;
  gsize last_new_offset;
} DiffData;

static gsize
match_bytes_at_start (const guchar *data1,
                      gsize data1_len,
                      const guchar *data2,
                      gsize data2_len)
{
  gsize len = 0;
  gsize max_len = MIN (data1_len, data2_len);

  while (len < max_len)
    {
      if (*data1 != *data2)
        break;
      data1++;
      data2++;
      len++;
    }
  return len;
}

static gsize
match_bytes_at_end (const guchar *data1,
                    gsize data1_len,
                    const guchar *data2,
                    gsize data2_len)
{
  gsize len = 0;
  gsize max_len = MIN (data1_len, data2_len);

  data1 += data1_len - 1;
  data2 += data2_len - 1;

  while (len < max_len)
    {
      if (*data1 != *data2)
        break;
      data1--;
      data2--;
      len++;
    }
  return len;
}

static DiffOp *
diff_ensure_op (DiffData *data,
                DiffOpKind kind)
{
  if (data->ops->len == 0 ||
      g_array_index (data->ops, DiffOp, data->ops->len-1).kind != kind)
    {
      DiffOp op = {kind, 0};
      g_array_append_val (data->ops, op);
    }

  return &g_array_index (data->ops, DiffOp, data->ops->len-1);
}

static void
diff_emit_reuse (DiffData *data,
                 gsize size)
{
  DiffOp *op;

  if (size == 0)
    return;

  op = diff_ensure_op (data, DIFF_OP_KIND_RESUSE_OLD);
  op->size += size;
}

static void
diff_emit_skip (DiffData *data,
                gsize size)
{
  DiffOp *op;

  if (size == 0)
    return;

  op = diff_ensure_op (data, DIFF_OP_KIND_SKIP_OLD);
  op->size += size;
}

static void
diff_emit_data (DiffData *data,
                gsize size,
                const guchar *new_data)
{
  DiffOp *op;

  if (size == 0)
    return;

  op = diff_ensure_op (data, DIFF_OP_KIND_DATA);
  op->size += size;

  g_array_append_vals (data->data, new_data, size);
}

static GBytes *
diff_encode (DiffData *data, GError **error)
{
  g_autoptr(GOutputStream) mem = g_memory_output_stream_new_resizable ();
  g_autoptr(GDataOutputStream) out = g_data_output_stream_new (mem);
  gsize ops_count = 0;

  g_data_output_stream_set_byte_order (out, G_DATA_STREAM_BYTE_ORDER_LITTLE_ENDIAN);

  /* Header */
  if (!g_output_stream_write_all (G_OUTPUT_STREAM (out),
                                  FLATPAK_SUMMARY_DIFF_HEADER, 4,
                                  NULL, NULL, error))
    return NULL;

  /* Write the ops count placeholder */
  if (!g_data_output_stream_put_uint32 (out, 0, NULL, error))
    return NULL;

  for (gsize i = 0; i < data->ops->len; i++)
    {
      DiffOp *op = &g_array_index (data->ops, DiffOp, i);
      gsize size = op->size;

      while (size > 0)
        {
          /* We leave a nibble at the top for the op */
          guint32 opdata = (guint64)size & 0x0fffffff;
          size -= opdata;

          opdata = opdata | ((0xf & op->kind) << 28);

          if (!g_data_output_stream_put_uint32 (out, opdata, NULL, error))
            return NULL;
          ops_count++;
        }
    }

  /* Then add the data */
  if (data->data->len > 0 &&
      !g_output_stream_write_all (G_OUTPUT_STREAM (out),
                                  data->data->data, data->data->len,
                                  NULL, NULL, error))
    return NULL;

  /* Back-patch in the ops count */
  if (!g_seekable_seek (G_SEEKABLE(out), 4, G_SEEK_SET, NULL, error))
    return NULL;

  if (!g_data_output_stream_put_uint32 (out, ops_count, NULL, error))
    return NULL;

  if (!g_output_stream_close (G_OUTPUT_STREAM (out), NULL, error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));
}

static void
diff_consume_block2 (DiffData *data,
                     gsize consume_old_offset,
                     gsize consume_old_size,
                     gsize produce_new_offset,
                     gsize produce_new_size)
{
  /* We consumed $consume_old_size bytes from $consume_old_offset to
     produce $produce_new_size bytes at $produce_new_size */

  /* First we copy old data for any matching prefix of the block */

  gsize prefix_len = match_bytes_at_start (data->old_data + consume_old_offset, consume_old_size,
                                           data->new_data + produce_new_offset, produce_new_size);
  diff_emit_reuse (data, prefix_len);

  consume_old_size -= prefix_len;
  consume_old_offset += prefix_len;

  produce_new_size -= prefix_len;
  produce_new_offset += prefix_len;

  /* Then we find the matching suffix for the rest */
  gsize suffix_len = match_bytes_at_end (data->old_data + consume_old_offset, consume_old_size,
                                         data->new_data + produce_new_offset, produce_new_size);

  /* Skip source data until suffix match */
  diff_emit_skip (data, consume_old_size - suffix_len);

  /* Copy new data until suffix match */
  diff_emit_data (data, produce_new_size - suffix_len, data->new_data + produce_new_offset);

  diff_emit_reuse (data, suffix_len);
}

static void
diff_consume_block (DiffData *data,
                    gssize consume_old_offset,
                    gsize consume_old_size,
                    gssize produce_new_offset,
                    gsize produce_new_size)
{
  if (consume_old_offset == -1)
    consume_old_offset = data->last_old_offset;
  if (produce_new_offset == -1)
    produce_new_offset = data->last_new_offset;

  /* We consumed $consume_old_size bytes from $consume_old_offset to
   * produce $produce_new_size bytes at $produce_new_size, however
   * while the emitted blocks are in order they may not cover the
   * every byte, so we emit the inbetwen blocks separately. */

  if (consume_old_offset != data->last_old_offset ||
      produce_new_offset != data->last_new_offset)
    diff_consume_block2 (data,
                         data->last_old_offset, consume_old_offset - data->last_old_offset ,
                         data->last_new_offset, produce_new_offset - data->last_new_offset);

  diff_consume_block2 (data,
                       consume_old_offset, consume_old_size,
                       produce_new_offset, produce_new_size);

  data->last_old_offset = consume_old_offset + consume_old_size;
  data->last_new_offset = produce_new_offset + produce_new_size;
}

GBytes *
flatpak_summary_apply_diff (GBytes *old,
                            GBytes *diff,
                            GError **error)
{
  g_autoptr(GBytes) uncompressed = NULL;
  const guchar *diffdata;
  gsize diff_size;
  guint32 *ops;
  guint32 n_ops;
  gsize data_offset;
  gsize data_size;
  const guchar *data;
  const guchar *old_data = g_bytes_get_data (old, NULL);
  gsize old_size = g_bytes_get_size (old);
  g_autoptr(GByteArray) res = g_byte_array_new ();

  uncompressed = flatpak_zlib_decompress_bytes (diff, error);
  if (uncompressed == NULL)
    {
      g_prefix_error (error, "Invalid summary diff: ");
      return NULL;
    }

  diffdata = g_bytes_get_data (uncompressed, NULL);
  diff_size = g_bytes_get_size (uncompressed);

  if (diff_size < 8 ||
      memcmp (diffdata, FLATPAK_SUMMARY_DIFF_HEADER, 4) != 0)
    {
      flatpak_fail (error, "Invalid summary diff");
      return NULL;
    }

  n_ops = GUINT32_FROM_LE (*(guint32 *)(diffdata+4));
  ops = (guint32 *)(diffdata+8);

  data_offset = 4 + 4 + 4 * n_ops;

  /* All ops must fit in diff, and avoid wrapping the multiply */
  if (data_offset > diff_size ||
      (data_offset - 4 - 4) / 4 != n_ops)
    {
      flatpak_fail (error, "Invalid summary diff");
      return NULL;
    }

  data = diffdata + data_offset;
  data_size = diff_size - data_offset;

  for (gsize i = 0; i < n_ops; i++)
    {
      guint32 opdata = GUINT32_FROM_LE (ops[i]);
      guint32 kind = (opdata & 0xf0000000) >> 28;
      guint32 size = opdata & 0x0fffffff;

      switch (kind)
        {
        case DIFF_OP_KIND_RESUSE_OLD:
          if (size > old_size)
            {
              flatpak_fail (error, "Invalid summary diff");
              return NULL;
            }
          g_byte_array_append (res, old_data, size);
          old_data += size;
          old_size -= size;
          break;
        case DIFF_OP_KIND_SKIP_OLD:
          if (size > old_size)
            {
              flatpak_fail (error, "Invalid summary diff");
              return NULL;
            }
          old_data += size;
          old_size -= size;
          break;
        case DIFF_OP_KIND_DATA:
          if (size > data_size)
            {
              flatpak_fail (error, "Invalid summary diff");
              return NULL;
            }
          g_byte_array_append (res, data, size);
          data += size;
          data_size -= size;
          break;
        default:
          flatpak_fail (error, "Invalid summary diff");
          return NULL;
        }
    }

  return g_byte_array_free_to_bytes (g_steal_pointer (&res));
}


static GBytes *
flatpak_summary_generate_diff (GVariant *old_v,
                               GVariant *new_v,
                               GError **error)
{
  VarSummaryRef new, old;
  VarRefMapRef new_refs, old_refs;
  VarRefMapEntryRef new_entry, old_entry;
  gsize new_len, old_len;
  int new_i, old_i;
  const char *old_ref, *new_ref;
  g_autoptr(GArray) ops = g_array_new (FALSE, TRUE, sizeof (DiffOp));
  g_autoptr(GArray) data_bytes = g_array_new (FALSE, TRUE, 1);
  g_autoptr(GBytes) diff_uncompressed = NULL;
  g_autoptr(GBytes) diff_compressed = NULL;
  DiffData data = {
    g_variant_get_data (old_v),
    g_variant_get_data (new_v),
    ops,
    data_bytes,
  };

  new = var_summary_from_gvariant (new_v);
  old = var_summary_from_gvariant (old_v);

  new_refs = var_summary_get_ref_map (new);
  old_refs = var_summary_get_ref_map (old);

  new_len = var_ref_map_get_length (new_refs);
  old_len = var_ref_map_get_length (old_refs);

  new_i = old_i = 0;
  while (new_i < new_len && old_i < old_len)
    {
      if (new_i == new_len)
        {
          /* Just old left */
          old_entry = var_ref_map_get_at (old_refs, old_i);
          old_ref = var_ref_map_entry_get_ref (old_entry);
          old_i++;
          diff_consume_block (&data,
                              -1, 0,
                              (const guchar *)new_entry.base - (const guchar *)new.base, new_entry.size);
        }
      else if (old_i == old_len)
        {
          /* Just new left */
          new_entry = var_ref_map_get_at (new_refs, new_i);
          new_ref = var_ref_map_entry_get_ref (new_entry);
          diff_consume_block (&data,
                              (const guchar *)old_entry.base - (const guchar *)old.base, old_entry.size,
                              -1, 0);

          new_i++;
        }
      else
        {
          new_entry = var_ref_map_get_at (new_refs, new_i);
          new_ref = var_ref_map_entry_get_ref (new_entry);

          old_entry = var_ref_map_get_at (old_refs, old_i);
          old_ref = var_ref_map_entry_get_ref (old_entry);

          int cmp = strcmp (new_ref, old_ref);
          if (cmp == 0)
            {
              /* same ref */
              diff_consume_block (&data,
                                  (const guchar *)old_entry.base - (const guchar *)old.base, old_entry.size,
                                  (const guchar *)new_entry.base - (const guchar *)new.base, new_entry.size);
              old_i++;
              new_i++;
            }
          else if (cmp < 0)
            {
              /* new added */
              diff_consume_block (&data,
                                  -1, 0,
                                  (const guchar *)new_entry.base - (const guchar *)new.base, new_entry.size);
              new_i++;
            }
          else
            {
              /* old removed */
              diff_consume_block (&data,
                                  (const guchar *)old_entry.base - (const guchar *)old.base, old_entry.size,
                                  -1, 0);
              old_i++;
            }
        }
    }

  /* Flush till the end */
  diff_consume_block2 (&data,
                       data.last_old_offset, old.size - data.last_old_offset,
                       data.last_new_offset, new.size - data.last_new_offset);

  diff_uncompressed = diff_encode (&data, error);
  if (diff_uncompressed == NULL)
    return NULL;

  diff_compressed = flatpak_zlib_compress_bytes (diff_uncompressed, 9, error);
  if (diff_compressed == NULL)
    return NULL;

#ifdef VALIDATE_DIFF
  {
    g_autoptr(GError) apply_error = NULL;
    g_autoptr(GBytes) old_bytes = g_variant_get_data_as_bytes (old_v);
    g_autoptr(GBytes) new_bytes = g_variant_get_data_as_bytes (new_v);
    g_autoptr(GBytes) applied = flatpak_summary_apply_diff (old_bytes, diff_compressed, &apply_error);
    g_assert (applied != NULL);
    g_assert (g_bytes_equal (applied, new_bytes));
  }
#endif

  return g_steal_pointer (&diff_compressed);
}

static void
variant_dict_merge (GVariantDict *dict,
                    GVariant *to_merge)
{
  GVariantIter iter;
  gchar *key;
  GVariant *value;

  if (to_merge)
    {
      g_variant_iter_init (&iter, to_merge);
      while (g_variant_iter_next (&iter, "{sv}", &key, &value))
        {
          g_variant_dict_insert_value (dict, key, value);
          g_variant_unref (value);
          g_free (key);
        }
    }
}

static void
add_summary_metadata (OstreeRepo   *repo,
                      GVariantBuilder *metadata_builder)
{
  GKeyFile *config;
  g_autofree char *title = NULL;
  g_autofree char *comment = NULL;
  g_autofree char *description = NULL;
  g_autofree char *homepage = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *redirect_url = NULL;
  g_autofree char *default_branch = NULL;
  g_autofree char *remote_mode_str = NULL;
  g_autofree char *authenticator_name = NULL;
  g_autofree char *gpg_keys = NULL;
  g_auto(GStrv) config_keys = NULL;
  int authenticator_install = -1;
  const char *collection_id;
  gboolean deploy_collection_id = FALSE;
  gboolean deploy_sideload_collection_id = FALSE;
  gboolean tombstone_commits = FALSE;

  config = ostree_repo_get_config (repo);

  if (config)
    {
      remote_mode_str = g_key_file_get_string (config, "core", "mode", NULL);
      tombstone_commits = g_key_file_get_boolean (config, "core", "tombstone-commits", NULL);

      title = g_key_file_get_string (config, "flatpak", "title", NULL);
      comment = g_key_file_get_string (config, "flatpak", "comment", NULL);
      description = g_key_file_get_string (config, "flatpak", "description", NULL);
      homepage = g_key_file_get_string (config, "flatpak", "homepage", NULL);
      icon = g_key_file_get_string (config, "flatpak", "icon", NULL);
      default_branch = g_key_file_get_string (config, "flatpak", "default-branch", NULL);
      gpg_keys = g_key_file_get_string (config, "flatpak", "gpg-keys", NULL);
      redirect_url = g_key_file_get_string (config, "flatpak", "redirect-url", NULL);
      deploy_sideload_collection_id = g_key_file_get_boolean (config, "flatpak", "deploy-sideload-collection-id", NULL);
      deploy_collection_id = g_key_file_get_boolean (config, "flatpak", "deploy-collection-id", NULL);
      authenticator_name = g_key_file_get_string (config, "flatpak", "authenticator-name", NULL);
      if (g_key_file_has_key (config, "flatpak", "authenticator-install", NULL))
        authenticator_install = g_key_file_get_boolean (config, "flatpak", "authenticator-install", NULL);

      config_keys = g_key_file_get_keys (config, "flatpak", NULL, NULL);
    }

  collection_id = ostree_repo_get_collection_id (repo);

  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.mode",
                         g_variant_new_string (remote_mode_str ? remote_mode_str : "bare"));
  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.tombstone-commits",
                         g_variant_new_boolean (tombstone_commits));
  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.indexed-deltas",
                         g_variant_new_boolean (TRUE));
  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.last-modified",
                         g_variant_new_uint64 (GUINT64_TO_BE (g_get_real_time () / G_USEC_PER_SEC)));

  if (collection_id)
    g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.collection-id",
                           g_variant_new_string (collection_id));

  if (title)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.title",
                           g_variant_new_string (title));

  if (comment)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.comment",
                           g_variant_new_string (comment));

  if (description)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.description",
                           g_variant_new_string (description));

  if (homepage)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.homepage",
                           g_variant_new_string (homepage));

  if (icon)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.icon",
                           g_variant_new_string (icon));

  if (redirect_url)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.redirect-url",
                           g_variant_new_string (redirect_url));

  if (default_branch)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.default-branch",
                           g_variant_new_string (default_branch));

  if (deploy_collection_id && collection_id != NULL)
    g_variant_builder_add (metadata_builder, "{sv}", OSTREE_META_KEY_DEPLOY_COLLECTION_ID,
                           g_variant_new_string (collection_id));
  else if (deploy_sideload_collection_id && collection_id != NULL)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.deploy-collection-id",
                           g_variant_new_string (collection_id));
  else if (deploy_collection_id)
    g_info ("Ignoring deploy-collection-id=true because no collection ID is set.");

  if (authenticator_name)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.authenticator-name",
                           g_variant_new_string (authenticator_name));

  if (authenticator_install != -1)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.authenticator-install",
                           g_variant_new_boolean (authenticator_install));

  g_variant_builder_add (metadata_builder, "{sv}", "xa.cache-version",
                         g_variant_new_uint32 (GUINT32_TO_LE (FLATPAK_XA_CACHE_VERSION)));

  if (config_keys != NULL)
    {
      for (int i = 0; config_keys[i] != NULL; i++)
        {
          const char *key = config_keys[i];
          g_autofree char *xa_key = NULL;
          g_autofree char *value = NULL;

          if (!g_str_has_prefix (key, "authenticator-options."))
            continue;

          value = g_key_file_get_string (config, "flatpak", key, NULL);
          if (value == NULL)
            continue;

          xa_key = g_strconcat ("xa.", key, NULL);
          g_variant_builder_add (metadata_builder, "{sv}", xa_key,
                                 g_variant_new_string (value));
        }
    }

  if (gpg_keys)
    {
      guchar *decoded;
      gsize decoded_len;

      gpg_keys = g_strstrip (gpg_keys);
      decoded = g_base64_decode (gpg_keys, &decoded_len);

      g_variant_builder_add (metadata_builder, "{sv}", "xa.gpg-keys",
                             g_variant_new_from_data (G_VARIANT_TYPE ("ay"), decoded, decoded_len,
                                                      TRUE, (GDestroyNotify) g_free, decoded));
    }
}

static char *
appstream_ref_get_subset (const char *ref)
{
  if (!g_str_has_prefix (ref, "appstream2/"))
    return NULL;

  const char *rest = ref + strlen ("appstream2/");
  const char *dash = strrchr (rest, '-');
  if (dash == NULL)
    return NULL;

  return g_strndup (rest, dash - rest);
}

static GVariant *
generate_summary (OstreeRepo   *repo,
                  gboolean      compat_format,
                  GHashTable   *refs,
                  GHashTable   *commit_data_cache,
                  GPtrArray    *delta_names,
                  const char   *subset,
                  const char  **summary_arches,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariantBuilder) ref_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{s(tts)}"));
  g_autoptr(GVariantBuilder) ref_sparse_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sa{sv}}"));
  g_autoptr(GVariantBuilder) refs_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(s(taya{sv}))"));
  g_autoptr(GVariantBuilder) summary_builder = g_variant_builder_new (OSTREE_SUMMARY_GVARIANT_FORMAT);
  g_autoptr(GHashTable) summary_arches_ht = NULL;
  g_autoptr(GHashTable) commits = NULL;
  g_autoptr(GList) ordered_keys = NULL;
  GList *l = NULL;

  /* In the new format this goes in the summary index instead */
  if (compat_format)
    add_summary_metadata (repo, metadata_builder);

  ordered_keys = g_hash_table_get_keys (refs);
  ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);

  if (summary_arches)
    {
      summary_arches_ht = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
      for (int i = 0; summary_arches[i] != NULL; i++)
        {
          const char *arch = summary_arches[i];

          g_hash_table_add (summary_arches_ht, (char *)arch);
        }
    }

  /* Compute which commits to keep */
  commits = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL); /* strings owned by ref */
  for (l = ordered_keys; l; l = l->next)
    {
      const char *ref = l->data;
      const char *rev = g_hash_table_lookup (refs, ref);
      g_autofree char *arch = NULL;
      const CommitData *rev_data = NULL;

      if (summary_arches)
        {
          /* NOTE: Non-arched (unknown) refs get into all summary versions */
          arch = flatpak_get_arch_for_ref (ref);
          if (arch != NULL && !g_hash_table_contains (summary_arches_ht, arch))
            continue; /* Filter this ref by arch */
        }

      rev_data = g_hash_table_lookup (commit_data_cache, rev);
      if (*subset != 0)
        {
          /* Subset summaries keep the appstream2/$subset-$arch, and have no appstream/ compat branch */

          if (g_str_has_prefix (ref, "appstream/"))
            {
              continue; /* No compat branch in subsets */
            }
          else if (g_str_has_prefix (ref, "appstream2/"))
            {
              g_autofree char *ref_subset = appstream_ref_get_subset (ref);
              if (ref_subset == NULL)
                continue; /* Non-subset, ignore */

              if (strcmp (subset, ref_subset) != 0)
                continue; /* Different subset, ignore */

              /* Otherwise, keep */
            }
          else if (rev_data)
            {
              if (rev_data->subsets == NULL ||
                  !flatpak_g_ptr_array_contains_string (rev_data->subsets, subset))
                continue; /* Ref is not in this subset */
            }
        }
      else
        {
          /* non-subset, keep everything but subset appstream refs */

          g_autofree char *ref_subset = appstream_ref_get_subset (ref);
          if (ref_subset != NULL)
            continue; /* Subset appstream ref, ignore */
        }

      g_hash_table_add (commits, (char *)rev);
    }

  /* Create refs list, metadata and sparse_data */
  for (l = ordered_keys; l; l = l->next)
    {
      const char *ref = l->data;
      const char *rev = g_hash_table_lookup (refs, ref);
      const CommitData *rev_data = NULL;
      g_auto(GVariantDict) commit_metadata_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      guint64 commit_size;
      guint64 commit_timestamp;

      if (!g_hash_table_contains (commits, rev))
        continue; /* Filter out commit (by arch & subset) */

      if (flatpak_is_app_runtime_or_appstream_ref (ref))
        rev_data = g_hash_table_lookup (commit_data_cache, rev);

      if (rev_data != NULL)
        {
          commit_size = rev_data->commit_size;
          commit_timestamp = rev_data->commit_timestamp;
        }
      else
        {
          g_autoptr(GVariant) commit_obj = NULL;
          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, rev, &commit_obj, error))
            return NULL;
          commit_size = g_variant_get_size (commit_obj);
          commit_timestamp = ostree_commit_get_timestamp (commit_obj);
        }

      g_variant_dict_init (&commit_metadata_builder, NULL);
      if (!compat_format && rev_data)
        {
          g_variant_dict_insert (&commit_metadata_builder, "xa.data", "(tts)",
                                 GUINT64_TO_BE (rev_data->installed_size),
                                 GUINT64_TO_BE (rev_data->download_size),
                                 rev_data->metadata_contents);
          variant_dict_merge (&commit_metadata_builder, rev_data->sparse_data);
        }

      /* For the new format summary we use a shorter name for the timestamp to save space */
      g_variant_dict_insert_value (&commit_metadata_builder,
                                   compat_format ? OSTREE_COMMIT_TIMESTAMP  : OSTREE_COMMIT_TIMESTAMP2,
                                   g_variant_new_uint64 (GUINT64_TO_BE (commit_timestamp)));

      g_variant_builder_add_value (refs_builder,
                                   g_variant_new ("(s(t@ay@a{sv}))", ref,
                                                  commit_size,
                                                  ostree_checksum_to_bytes_v (rev),
                                                  g_variant_dict_end (&commit_metadata_builder)));

      if (compat_format && rev_data)
        {
          g_variant_builder_add (ref_data_builder, "{s(tts)}",
                                 ref,
                                 GUINT64_TO_BE (rev_data->installed_size),
                                 GUINT64_TO_BE (rev_data->download_size),
                                 rev_data->metadata_contents);
          if (rev_data->sparse_data)
            g_variant_builder_add (ref_sparse_data_builder, "{s@a{sv}}",
                                   ref, rev_data->sparse_data);
        }
    }

  if (delta_names)
    {
      g_auto(GVariantDict) deltas_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;

      g_variant_dict_init (&deltas_builder, NULL);
      for (guint i = 0; i < delta_names->len; i++)
        {
          g_autofree char *from = NULL;
          g_autofree char *to = NULL;
          GVariant *digest;

          _ostree_parse_delta_name (delta_names->pdata[i], &from, &to);

          /* Only keep deltas going to a ref that is in the summary
           * (i.e. not arch filtered or random) */
          if (!g_hash_table_contains (commits, to))
            continue;

          digest = _ostree_repo_static_delta_superblock_digest (repo,
                                                                (from && from[0]) ? from : NULL,
                                                                to, cancellable, error);
          if (digest == NULL)
            return FALSE;

          g_variant_dict_insert_value (&deltas_builder, delta_names->pdata[i], digest);
        }

      if (delta_names->len > 0)
        g_variant_builder_add (metadata_builder, "{sv}", "ostree.static-deltas", g_variant_dict_end (&deltas_builder));
    }

  if (compat_format)
    {
      /* Note: xa.cache doesn’t need to support collection IDs for the refs listed
       * in it, because the xa.cache metadata is stored on the ostree-metadata ref,
       * which is itself strongly bound to a collection ID — so that collection ID
       * is bound to all the refs in xa.cache. If a client is using the xa.cache
       * data from a summary file (rather than an ostree-metadata branch), they are
       * too old to care about collection IDs anyway. */
      g_variant_builder_add (metadata_builder, "{sv}", "xa.cache",
                             g_variant_new_variant (g_variant_builder_end (ref_data_builder)));
      g_variant_builder_add (metadata_builder, "{sv}", "xa.sparse-cache",
                             g_variant_builder_end (ref_sparse_data_builder));
    }
  else
    {
      g_variant_builder_add (metadata_builder, "{sv}", "xa.summary-version",
                             g_variant_new_uint32 (GUINT32_TO_LE (FLATPAK_XA_SUMMARY_VERSION)));
    }

  g_variant_builder_add_value (summary_builder, g_variant_builder_end (refs_builder));
  g_variant_builder_add_value (summary_builder, g_variant_builder_end (metadata_builder));

  return g_variant_ref_sink (g_variant_builder_end (summary_builder));
}

static GVariant *
read_digested_summary (OstreeRepo   *repo,
                       const char   *digest,
                       GHashTable   *digested_summary_cache,
                       GCancellable *cancellable,
                       GError      **error)
{
  GVariant *cached;
  g_autoptr(GVariant) loaded = NULL;

  cached = g_hash_table_lookup (digested_summary_cache, digest);
  if (cached)
    return g_variant_ref (cached);

  loaded = flatpak_repo_load_digested_summary (repo, digest, error);
  if (loaded == NULL)
    return NULL;

  g_hash_table_insert (digested_summary_cache, g_strdup (digest), g_variant_ref (loaded));

  return g_steal_pointer (&loaded);
}

static gboolean
add_to_history (OstreeRepo      *repo,
                GVariantBuilder *history_builder,
                VarChecksumRef   old_digest_vv,
                GVariant        *current_digest_v,
                GVariant        *current_content,
                GHashTable      *digested_summary_cache,
                guint           *history_len,
                guint            max_history_length,
                GCancellable    *cancellable,
                GError         **error)
{
  g_autoptr(GVariant) old_digest_v = g_variant_ref_sink (var_checksum_dup_to_gvariant (old_digest_vv));
  g_autofree char *old_digest = NULL;
  g_autoptr(GVariant) old_content = NULL;
  g_autofree char *current_digest = NULL;
  g_autoptr(GBytes) subsummary_diff = NULL;

  /* Limit history length */
  if (*history_len >= max_history_length)
    return TRUE;

  /* Avoid repeats in the history (in case nothing changed in subsummary) */
  if (g_variant_equal (old_digest_v, current_digest_v))
    return TRUE;

  old_digest = ostree_checksum_from_bytes_v (old_digest_v);
  old_content = read_digested_summary (repo, old_digest, digested_summary_cache, cancellable, NULL);
  if  (old_content == NULL)
    return TRUE; /* Only add parents that still exist */

  subsummary_diff = flatpak_summary_generate_diff (old_content, current_content, error);
  if  (subsummary_diff == NULL)
    return FALSE;

  current_digest = ostree_checksum_from_bytes_v (current_digest_v);

  if (!flatpak_repo_save_digested_summary_delta (repo, old_digest, current_digest,
                                                 subsummary_diff, cancellable, error))
    return FALSE;

  *history_len += 1;
  g_variant_builder_add_value (history_builder, old_digest_v);

  return TRUE;
}

static GVariant *
generate_summary_index (OstreeRepo   *repo,
                        GVariant     *old_index_v,
                        GHashTable   *summaries,
                        GHashTable   *digested_summaries,
                        GHashTable   *digested_summary_cache,
                        const char  **gpg_key_ids,
                        const char   *gpg_homedir,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariantBuilder) subsummary_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{s(ayaaya{sv})}"));
  g_autoptr(GVariantBuilder) index_builder = g_variant_builder_new (FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT);
  g_autoptr(GVariant) index = NULL;
  g_autoptr(GList) ordered_summaries = NULL;
  guint max_history_length = flatpak_repo_get_summary_history_length (repo);
  GList *l;

  add_summary_metadata (repo, metadata_builder);

  ordered_summaries = g_hash_table_get_keys (summaries);
  ordered_summaries = g_list_sort (ordered_summaries, (GCompareFunc) strcmp);
  for (l = ordered_summaries; l; l = l->next)
    {
      g_auto(GVariantDict) subsummary_metadata_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      const char *subsummary = l->data;
      const char *digest = g_hash_table_lookup (summaries, subsummary);
      g_autoptr(GVariant) digest_v = g_variant_ref_sink (ostree_checksum_to_bytes_v (digest));
      g_autoptr(GVariantBuilder) history_builder = g_variant_builder_new (G_VARIANT_TYPE ("aay"));
      g_autoptr(GVariant) subsummary_content = NULL;

      subsummary_content = read_digested_summary (repo, digest, digested_summary_cache, cancellable, error);
      if  (subsummary_content == NULL)
        return NULL;  /* This really should always be there as we're supposed to index it */

      if (old_index_v)
        {
          VarSummaryIndexRef old_index = var_summary_index_from_gvariant (old_index_v);
          VarSummaryIndexSubsummariesRef old_subsummaries = var_summary_index_get_subsummaries (old_index);
          VarSubsummaryRef old_subsummary;
          guint history_len = 0;

          if (var_summary_index_subsummaries_lookup (old_subsummaries, subsummary, NULL, &old_subsummary))
            {
              VarChecksumRef parent = var_subsummary_get_checksum (old_subsummary);

              /* Add current as first in history */
              if (!add_to_history (repo, history_builder, parent, digest_v, subsummary_content, digested_summary_cache,
                                   &history_len, max_history_length, cancellable, error))
                return FALSE;

              /* Add previous history */
              VarArrayofChecksumRef history = var_subsummary_get_history (old_subsummary);
              gsize len = var_arrayof_checksum_get_length (history);
              for (gsize i = 0; i < len; i++)
                {
                  VarChecksumRef c = var_arrayof_checksum_get_at (history, i);
                  if (!add_to_history (repo, history_builder, c, digest_v, subsummary_content, digested_summary_cache,
                                       &history_len, max_history_length, cancellable, error))
                    return FALSE;
                }
            }
        }

      g_variant_dict_init (&subsummary_metadata_builder, NULL);
      g_variant_builder_add (subsummary_builder, "{s(@ay@aay@a{sv})}",
                             subsummary,
                             digest_v,
                             g_variant_builder_end (history_builder),
                             g_variant_dict_end (&subsummary_metadata_builder));
    }

  g_variant_builder_add_value (index_builder, g_variant_builder_end (subsummary_builder));
  g_variant_builder_add_value (index_builder, g_variant_builder_end (metadata_builder));

  index = g_variant_ref_sink (g_variant_builder_end (index_builder));

  return g_steal_pointer (&index);
}

static gboolean
flatpak_repo_gc_digested_summaries (OstreeRepo *repo,
                                    const char *index_digest,           /* The digest of the current (new) index (if any) */
                                    const char *old_index_digest,       /* The digest of the previous index (if any) */
                                    GHashTable *digested_summaries,     /* generated */
                                    GHashTable *digested_summary_cache, /* generated + referenced */
                                    GCancellable *cancellable,
                                    GError **error)
{
  g_auto(GLnxDirFdIterator) iter = {0};
  int repo_fd = ostree_repo_get_dfd (repo);
  struct dirent *dent;
  const char *ext;
  g_autoptr(GError) local_error = NULL;

  if (!glnx_dirfd_iterator_init_at (repo_fd, "summaries", FALSE, &iter, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  while (TRUE)
    {
      gboolean remove = FALSE;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iter, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (dent->d_type != DT_REG)
        continue;

      /* Keep it if its an unexpected type */
      ext = strchr (dent->d_name, '.');
      if (ext != NULL)
        {
          if (strcmp (ext, ".gz") == 0 && strlen (dent->d_name) == 64 + 3)
            {
              g_autofree char *sha256 = g_strndup (dent->d_name, 64);

              /* Keep all the referenced summaries */
              if (g_hash_table_contains (digested_summary_cache, sha256))
                {
                  g_info ("Keeping referenced summary %s", dent->d_name);
                  continue;
                }
              /* Remove rest */
              remove = TRUE;
            }
          else if (strcmp (ext, ".delta") == 0)
            {
              const char *dash = strchr (dent->d_name, '-');
              if (dash != NULL && dash < ext && (ext - dash) == 1 + 64)
                {
                  g_autofree char *to_sha256 = g_strndup (dash + 1, 64);

                  /* Only keep deltas going to a generated summary */
                  if (g_hash_table_contains (digested_summaries, to_sha256))
                    {
                      g_info ("Keeping delta to generated summary %s", dent->d_name);
                      continue;
                    }
                  /* Remove rest */
                  remove = TRUE;
                }
            }
          else if (strcmp (ext, ".idx.sig") == 0)
            {
              g_autofree char *digest = g_strndup (dent->d_name, strlen (dent->d_name) - strlen (".idx.sig"));

              if (g_strcmp0 (digest, index_digest) == 0)
                continue; /* Always keep current */

              if (g_strcmp0 (digest, old_index_digest) == 0)
                continue; /* Always keep previous one, to avoid some races */

              /* Remove the rest */
              remove = TRUE;
            }
        }

      if (remove)
        {
          g_info ("Removing old digested summary file %s", dent->d_name);
          if (unlinkat (iter.fd, dent->d_name, 0) != 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }
      else
        g_info ("Keeping unexpected summary file %s", dent->d_name);
    }

  return TRUE;
}

/* Update the metadata in the summary file for @repo, and then re-sign the file.
 * If the repo has a collection ID set, additionally store the metadata on a
 * contentless commit in a well-known branch, which is the preferred way of
 * broadcasting per-repo metadata (putting it in the summary file is deprecated,
 * but kept for backwards compatibility).
 *
 * Note that there are two keys for the collection ID: collection-id, and
 * ostree.deploy-collection-id. If a client does not currently have a
 * collection ID configured for this remote, it will *only* update its
 * configuration from ostree.deploy-collection-id.  This allows phased
 * deployment of collection-based repositories. Clients will only update their
 * configuration from an unset to a set collection ID once (otherwise the
 * security properties of collection IDs are broken). */
gboolean
flatpak_repo_update (OstreeRepo   *repo,
                     FlatpakRepoUpdateFlags flags,
                     const char  **gpg_key_ids,
                     const char   *gpg_homedir,
                     GCancellable *cancellable,
                     GError      **error)
{
  g_autoptr(GHashTable) commit_data_cache = NULL;
  g_autoptr(GVariant) compat_summary = NULL;
  g_autoptr(GVariant) summary_index = NULL;
  g_autoptr(GVariant) old_index = NULL;
  g_autoptr(GPtrArray) delta_names = NULL;
  g_auto(GStrv) summary_arches = NULL;
  g_autoptr(GHashTable) refs = NULL;
  g_autoptr(GHashTable) arches = NULL;
  g_autoptr(GHashTable) subsets = NULL;
  g_autoptr(GHashTable) summaries = NULL;
  g_autoptr(GHashTable) digested_summaries = NULL;
  g_autoptr(GHashTable) digested_summary_cache = NULL;
  g_autoptr(GBytes) index_sig = NULL;
  time_t old_compat_sig_mtime;
  GKeyFile *config;
  gboolean disable_index = (flags & FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX) != 0;
  g_autofree char *index_digest = NULL;
  g_autofree char *old_index_digest = NULL;

  config = ostree_repo_get_config (repo);

  if (!ostree_repo_list_refs_ext (repo, NULL, &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES | OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_MIRRORS,
                                  cancellable, error))
    return FALSE;

  old_index = flatpak_repo_load_summary_index (repo, NULL);
  if (old_index)
    commit_data_cache = populate_commit_data_cache (repo, old_index);

  if (commit_data_cache == NULL) /* No index or failed to load it */
    commit_data_cache = commit_data_cache_new ();

  if (!ostree_repo_list_static_delta_names (repo, &delta_names, cancellable, error))
    return FALSE;

  if (config)
    summary_arches = g_key_file_get_string_list (config, "flatpak", "summary-arches", NULL, NULL);

  summaries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  /* These are the ones we generated */
  digested_summaries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  /* These are the ones generated or references */
  digested_summary_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

  arches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  subsets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add (subsets, g_strdup ("")); /* Always have everything subset */

  GLNX_HASH_TABLE_FOREACH_KV (refs, const char *, ref, const char *, rev)
    {
      g_autofree char *arch = flatpak_get_arch_for_ref (ref);
      CommitData *rev_data = NULL;

      if (arch != NULL &&
          !g_hash_table_contains (arches, arch))
        g_hash_table_add (arches, g_steal_pointer (&arch));

      /* Add CommitData for flatpak refs that we didn't already pre-populate */
      if (flatpak_is_app_runtime_or_appstream_ref (ref))
        {
          rev_data = g_hash_table_lookup (commit_data_cache, rev);
          if (rev_data == NULL)
            {
              rev_data = read_commit_data (repo, ref, rev, cancellable, error);
              if (rev_data == NULL)
                return FALSE;

              g_hash_table_insert (commit_data_cache, g_strdup (rev), (CommitData *)rev_data);
            }

          for (int i = 0; rev_data->subsets != NULL && i < rev_data->subsets->len; i++)
            {
              const char *subset = g_ptr_array_index (rev_data->subsets, i);
              if (!g_hash_table_contains (subsets, subset))
                g_hash_table_add (subsets, g_strdup (subset));
            }
        }
    }

  compat_summary = generate_summary (repo, TRUE, refs, commit_data_cache, delta_names,
                                     "", (const char **)summary_arches,
                                     cancellable, error);
  if (compat_summary == NULL)
    return FALSE;

  if (!disable_index)
    {
      GLNX_HASH_TABLE_FOREACH (subsets, const char *, subset)
        {
          GLNX_HASH_TABLE_FOREACH (arches, const char *, arch)
            {
              const char *arch_v[] = { arch, NULL };
              g_autofree char *name = NULL;
              g_autofree char *digest = NULL;

              if (*subset == 0)
                name = g_strdup (arch);
              else
                name = g_strconcat (subset, "-", arch, NULL);

              g_autoptr(GVariant) arch_summary = generate_summary (repo, FALSE, refs, commit_data_cache, NULL, subset, arch_v,
                                                                   cancellable, error);
              if (arch_summary == NULL)
                return FALSE;

              digest = flatpak_repo_save_digested_summary (repo, name, arch_summary, cancellable, error);
              if (digest == NULL)
                return FALSE;

              g_hash_table_insert (digested_summaries, g_strdup (digest), g_variant_ref (arch_summary));
              /* Prime summary cache with generated summaries */
              g_hash_table_insert (digested_summary_cache, g_strdup (digest), g_variant_ref (arch_summary));
              g_hash_table_insert (summaries, g_steal_pointer (&name), g_steal_pointer (&digest));
            }
        }

      summary_index = generate_summary_index (repo, old_index, summaries, digested_summaries, digested_summary_cache,
                                              gpg_key_ids, gpg_homedir,
                                              cancellable, error);
      if (summary_index == NULL)
        return FALSE;
    }

  if (!ostree_repo_static_delta_reindex (repo, 0, NULL, cancellable, error))
    return FALSE;

  if (summary_index && gpg_key_ids)
    {
      g_autoptr(GBytes) index_bytes = g_variant_get_data_as_bytes (summary_index);

      if (!ostree_repo_gpg_sign_data (repo, index_bytes,
                                      NULL,
                                      gpg_key_ids,
                                      gpg_homedir,
                                      &index_sig,
                                      cancellable,
                                      error))
        return FALSE;
    }

  if (summary_index)
    index_digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                                g_variant_get_data (summary_index),
                                                g_variant_get_size (summary_index));
  if (old_index)
    old_index_digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                                    g_variant_get_data (old_index),
                                                    g_variant_get_size (old_index));

  /* Release the memory-mapped summary index file before replacing it,
     to avoid failure on filesystems like cifs */
  g_clear_pointer (&old_index, g_variant_unref);

  if (!flatpak_repo_save_summary_index (repo, summary_index, index_digest, index_sig, cancellable, error))
    return FALSE;

  if (!flatpak_repo_save_compat_summary (repo, compat_summary, &old_compat_sig_mtime, cancellable, error))
    return FALSE;

  if (gpg_key_ids)
    {
      if (!ostree_repo_add_gpg_signature_summary (repo,
                                                  gpg_key_ids,
                                                  gpg_homedir,
                                                  cancellable,
                                                  error))
        return FALSE;


      if (old_compat_sig_mtime != 0)
        {
          int repo_dfd = ostree_repo_get_dfd (repo);
          struct stat stbuf;

          /* Ensure we increase (in sec precision) */
          if (fstatat (repo_dfd, "summary.sig", &stbuf, AT_SYMLINK_NOFOLLOW) == 0 &&
              stbuf.st_mtime <= old_compat_sig_mtime)
            {
              struct timespec ts[2] = { {0, UTIME_OMIT}, {old_compat_sig_mtime + 1, 0} };
              (void) utimensat (repo_dfd, "summary.sig", ts, AT_SYMLINK_NOFOLLOW);
            }
        }
    }

  if (!disable_index &&
      !flatpak_repo_gc_digested_summaries (repo, index_digest, old_index_digest, digested_summaries, digested_summary_cache, cancellable, error))
    return FALSE;

  return TRUE;
}

/* Wrapper that uses ostree_repo_resolve_collection_ref() and on failure falls
 * back to using ostree_repo_resolve_rev() for backwards compatibility. This
 * means we support refs/heads/, refs/remotes/, and refs/mirrors/. */
gboolean
flatpak_repo_resolve_rev (OstreeRepo    *repo,
                          const char    *collection_id, /* nullable */
                          const char    *remote_name, /* nullable */
                          const char    *ref_name,
                          gboolean       allow_noent,
                          char         **out_rev,
                          GCancellable  *cancellable,
                          GError       **error)
{
  g_autoptr(GError) local_error = NULL;

  if (collection_id != NULL)
    {
      /* Do a version check to ensure we have these:
       * https://github.com/ostreedev/ostree/pull/1821
       * https://github.com/ostreedev/ostree/pull/1825 */
#if OSTREE_CHECK_VERSION (2019, 2)
      const OstreeCollectionRef c_r =
        {
          .collection_id = (char *) collection_id,
          .ref_name = (char *) ref_name,
        };
      OstreeRepoResolveRevExtFlags flags = remote_name == NULL ?
                                           OSTREE_REPO_RESOLVE_REV_EXT_LOCAL_ONLY :
                                           OSTREE_REPO_RESOLVE_REV_EXT_NONE;
      if (ostree_repo_resolve_collection_ref (repo, &c_r,
                                              allow_noent,
                                              flags,
                                              out_rev,
                                              cancellable, NULL))
        return TRUE;
#endif
    }

  /* There may be several remotes with the same branch (if we for
   * instance changed the origin) so prepend the current origin to
   * make sure we get the right one */
  if (remote_name != NULL)
    {
      g_autofree char *refspec = g_strdup_printf ("%s:%s", remote_name, ref_name);
      ostree_repo_resolve_rev (repo, refspec, allow_noent, out_rev, &local_error);
    }
  else
    ostree_repo_resolve_rev_ext (repo, ref_name, allow_noent,
                                 OSTREE_REPO_RESOLVE_REV_EXT_NONE, out_rev, &local_error);

  if (local_error != NULL)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND, "%s", local_error->message);
      else
        g_propagate_error (error, g_steal_pointer (&local_error));

      return FALSE;
    }

  return TRUE;
}

/* This special cases the ref lookup which by doing a
   bsearch since the array is sorted */
gboolean
flatpak_var_ref_map_lookup_ref (VarRefMapRef   ref_map,
                                const char    *ref,
                                VarRefInfoRef *out_info)
{
  gsize imax, imin;
  gsize imid;
  gsize n;

  g_return_val_if_fail (out_info != NULL, FALSE);

  n = var_ref_map_get_length (ref_map);
  if (n == 0)
    return FALSE;

  imax = n - 1;
  imin = 0;
  while (imax >= imin)
    {
      VarRefMapEntryRef entry;
      const char *cur;
      int cmp;

      imid = (imin + imax) / 2;

      entry = var_ref_map_get_at (ref_map, imid);
      cur = var_ref_map_entry_get_ref (entry);

      cmp = strcmp (cur, ref);
      if (cmp < 0)
        {
          imin = imid + 1;
        }
      else if (cmp > 0)
        {
          if (imid == 0)
            break;
          imax = imid - 1;
        }
      else
        {
          *out_info = var_ref_map_entry_get_info (entry);
          return TRUE;
        }
    }

  return FALSE;
}

/* Find the list of refs which belong to the given @collection_id in @summary.
 * If @collection_id is %NULL, the main refs list from the summary will be
 * returned. If @collection_id doesn’t match any collection IDs in the summary
 * file, %FALSE will be returned. */
gboolean
flatpak_summary_find_ref_map (VarSummaryRef summary,
                              const char *collection_id,
                              VarRefMapRef *refs_out)
{
  VarMetadataRef metadata = var_summary_get_metadata (summary);
  const char *summary_collection_id;

  summary_collection_id = var_metadata_lookup_string (metadata, "ostree.summary.collection-id", NULL);

  if (collection_id == NULL || g_strcmp0 (collection_id, summary_collection_id) == 0)
    {
      if (refs_out)
        *refs_out = var_summary_get_ref_map (summary);
      return TRUE;
    }
  else if (collection_id != NULL)
    {
      VarVariantRef collection_map_v;
      if (var_metadata_lookup (metadata, "ostree.summary.collection-map", NULL, &collection_map_v))
        {
          VarCollectionMapRef collection_map = var_collection_map_from_variant (collection_map_v);
          return var_collection_map_lookup (collection_map, collection_id, NULL, refs_out);
        }
    }

  return FALSE;
}

/* This matches all refs from @collection_id that have ref, followed by '.'  as prefix */
GPtrArray *
flatpak_summary_match_subrefs (GVariant          *summary_v,
                               const char        *collection_id,
                               FlatpakDecomposed *ref)
{
  GPtrArray *res = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);
  gsize n, i;
  g_autofree char *parts_prefix = NULL;
  g_autofree char *ref_prefix = NULL;
  g_autofree char *ref_suffix = NULL;
  VarSummaryRef summary;
  VarRefMapRef ref_map;

  summary = var_summary_from_gvariant (summary_v);

  /* Work out which refs list to use, based on the @collection_id. */
  if (flatpak_summary_find_ref_map (summary, collection_id, &ref_map))
    {
      /* Match against the refs. */
      g_autofree char *id = flatpak_decomposed_dup_id (ref);
      g_autofree char *arch = flatpak_decomposed_dup_arch (ref);
      g_autofree char *branch = flatpak_decomposed_dup_branch (ref);
      parts_prefix = g_strconcat (id, ".", NULL);

      ref_prefix = g_strconcat (flatpak_decomposed_get_kind_str (ref), "/", NULL);
      ref_suffix = g_strconcat ("/", arch, "/", branch, NULL);

      n = var_ref_map_get_length (ref_map);
      for (i = 0; i < n; i++)
        {
          VarRefMapEntryRef entry = var_ref_map_get_at (ref_map, i);
          const char *cur;
          const char *id_start;
          const char *id_suffix;
          const char *id_end;

          cur = var_ref_map_entry_get_ref (entry);

          /* Must match type */
          if (!g_str_has_prefix (cur, ref_prefix))
            continue;

          /* Must match arch & branch */
          if (!g_str_has_suffix (cur, ref_suffix))
            continue;

          id_start = strchr (cur, '/');
          if (id_start == NULL)
            continue;
          id_start += 1;

          id_end = strchr (id_start, '/');
          if (id_end == NULL)
            continue;

          /* But only prefix of id */
          if (!g_str_has_prefix (id_start, parts_prefix))
            continue;

          /* And no dots (we want to install prefix.$ID, but not prefix.$ID.Sources) */
          id_suffix = id_start + strlen (parts_prefix);
          if (memchr (id_suffix, '.', id_end - id_suffix) != NULL)
            continue;

          FlatpakDecomposed *d = flatpak_decomposed_new_from_ref (cur, NULL);
          if (d)
            g_ptr_array_add (res, d);
        }
    }

  return g_steal_pointer (&res);
}

gboolean
flatpak_summary_lookup_ref (GVariant      *summary_v,
                            const char    *collection_id,
                            const char    *ref,
                            char         **out_checksum,
                            VarRefInfoRef *out_info)
{
  VarSummaryRef summary;
  VarRefMapRef ref_map;
  VarRefInfoRef info;
  const guchar *checksum_bytes;
  gsize checksum_bytes_len;

  summary = var_summary_from_gvariant (summary_v);

  /* Work out which refs list to use, based on the @collection_id. */
  if (!flatpak_summary_find_ref_map (summary, collection_id, &ref_map))
    return FALSE;

  if (!flatpak_var_ref_map_lookup_ref (ref_map, ref, &info))
    return FALSE;

  checksum_bytes = var_ref_info_peek_checksum (info, &checksum_bytes_len);
  if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
    return FALSE;

  if (out_checksum)
    *out_checksum = ostree_checksum_from_bytes (checksum_bytes);

  if (out_info)
    *out_info = info;

  return TRUE;
}

GKeyFile *
flatpak_parse_repofile (const char   *remote_name,
                        gboolean      from_ref,
                        GKeyFile     *keyfile,
                        GBytes      **gpg_data_out,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *uri = NULL;
  g_autofree char *title = NULL;
  g_autofree char *gpg_key = NULL;
  g_autofree char *collection_id = NULL;
  g_autofree char *default_branch = NULL;
  g_autofree char *comment = NULL;
  g_autofree char *description = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *homepage = NULL;
  g_autofree char *filter = NULL;
  g_autofree char *subset = NULL;
  g_autofree char *authenticator_name = NULL;
  gboolean nodeps;
  const char *source_group;
  g_autofree char *version = NULL;

  if (from_ref)
    source_group = FLATPAK_REF_GROUP;
  else
    source_group = FLATPAK_REPO_GROUP;

  GKeyFile *config = g_key_file_new ();
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  if (!g_key_file_has_group (keyfile, source_group))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid %s: Missing group ‘%s’"),
                          from_ref ? ".flatpakref" : ".flatpakrepo", source_group);
      return NULL;
    }

  uri = g_key_file_get_string (keyfile, source_group,
                               FLATPAK_REPO_URL_KEY, NULL);
  if (uri == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid %s: Missing key ‘%s’"),
                          from_ref ? ".flatpakref" : ".flatpakrepo", FLATPAK_REPO_URL_KEY);
      return NULL;
    }

  version = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_VERSION_KEY, NULL);
  if (version != NULL && strcmp (version, "1") != 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                          _("Invalid version %s, only 1 supported"), version);
      return NULL;
    }

  g_key_file_set_string (config, group, "url", uri);

  subset = g_key_file_get_locale_string (keyfile, source_group,
                                         FLATPAK_REPO_SUBSET_KEY, NULL, NULL);
  if (subset != NULL)
    g_key_file_set_string (config, group, "xa.subset", subset);

  /* Don't use the title from flatpakref files; that's the title of the app */
  if (!from_ref)
    title = g_key_file_get_locale_string (keyfile, FLATPAK_REPO_GROUP,
                                          FLATPAK_REPO_TITLE_KEY, NULL, NULL);
  if (title != NULL)
    g_key_file_set_string (config, group, "xa.title", title);

  default_branch = g_key_file_get_locale_string (keyfile, source_group,
                                                 FLATPAK_REPO_DEFAULT_BRANCH_KEY, NULL, NULL);
  if (default_branch != NULL)
    g_key_file_set_string (config, group, "xa.default-branch", default_branch);

  nodeps = g_key_file_get_boolean (keyfile, source_group,
                                   FLATPAK_REPO_NODEPS_KEY, NULL);
  if (nodeps)
    g_key_file_set_boolean (config, group, "xa.nodeps", TRUE);

  gpg_key = g_key_file_get_string (keyfile, source_group,
                                   FLATPAK_REPO_GPGKEY_KEY, NULL);
  if (gpg_key != NULL)
    {
      guchar *decoded;
      gsize decoded_len;

      gpg_key = g_strstrip (gpg_key);
      decoded = g_base64_decode (gpg_key, &decoded_len);
      if (decoded_len < 10) /* Check some minimal size so we don't get crap */
        {
          g_free (decoded);
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid gpg key"));
          return NULL;
        }

      gpg_data = g_bytes_new_take (decoded, decoded_len);
      g_key_file_set_boolean (config, group, "gpg-verify", TRUE);
    }
  else
    {
      g_key_file_set_boolean (config, group, "gpg-verify", FALSE);
    }

  /* We have a hierarchy of keys for setting the collection ID, which all have
   * the same effect. The only difference is which versions of Flatpak support
   * them, and therefore what P2P implementation is enabled by them:
   * DeploySideloadCollectionID: supported by Flatpak >= 1.12.8 (1.7.1
   *   introduced sideload support but this key was added late)
   * DeployCollectionID: supported by Flatpak >= 1.0.6 (but fully supported in
   *   >= 1.2.0)
   * CollectionID: supported by Flatpak >= 0.9.8
   */
  collection_id = flatpak_keyfile_get_string_non_empty (keyfile, source_group,
                                                        FLATPAK_REPO_DEPLOY_SIDELOAD_COLLECTION_ID_KEY);
  if (collection_id == NULL)
    collection_id = flatpak_keyfile_get_string_non_empty (keyfile, source_group,
                                                          FLATPAK_REPO_DEPLOY_COLLECTION_ID_KEY);
  if (collection_id == NULL)
    collection_id = flatpak_keyfile_get_string_non_empty (keyfile, source_group,
                                                          FLATPAK_REPO_COLLECTION_ID_KEY);
  if (collection_id != NULL)
    {
      if (gpg_key == NULL)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ID requires GPG key to be provided"));
          return NULL;
        }

      g_key_file_set_string (config, group, "collection-id", collection_id);
    }

  g_key_file_set_boolean (config, group, "gpg-verify-summary",
                          (gpg_key != NULL));

  authenticator_name = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                              FLATPAK_REPO_AUTHENTICATOR_NAME_KEY, NULL);
  if (authenticator_name)
    g_key_file_set_string (config, group, "xa.authenticator-name", authenticator_name);

  if (g_key_file_has_key (keyfile, FLATPAK_REPO_GROUP, FLATPAK_REPO_AUTHENTICATOR_INSTALL_KEY, NULL))
    {
      gboolean authenticator_install = g_key_file_get_boolean (keyfile, FLATPAK_REPO_GROUP,
                                                               FLATPAK_REPO_AUTHENTICATOR_INSTALL_KEY, NULL);
      g_key_file_set_boolean (config, group, "xa.authenticator-install", authenticator_install);
    }

  comment = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_COMMENT_KEY, NULL);
  if (comment)
    g_key_file_set_string (config, group, "xa.comment", comment);

  description = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                       FLATPAK_REPO_DESCRIPTION_KEY, NULL);
  if (description)
    g_key_file_set_string (config, group, "xa.description", description);

  icon = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                FLATPAK_REPO_ICON_KEY, NULL);
  if (icon)
    g_key_file_set_string (config, group, "xa.icon", icon);

  homepage  = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                     FLATPAK_REPO_HOMEPAGE_KEY, NULL);
  if (homepage)
    g_key_file_set_string (config, group, "xa.homepage", homepage);

  filter = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_FILTER_KEY, NULL);
  if (filter)
    g_key_file_set_string (config, group, "xa.filter", filter);
  else
    g_key_file_set_string (config, group, "xa.filter", ""); /* Default to override any pre-existing filters */

  *gpg_data_out = g_steal_pointer (&gpg_data);

  return g_steal_pointer (&config);
}

gboolean
flatpak_mtree_create_dir (OstreeRepo         *repo,
                          OstreeMutableTree  *parent,
                          const char         *name,
                          OstreeMutableTree **dir_out,
                          GError            **error)
{
  g_autoptr(OstreeMutableTree) dir = NULL;

  if (!ostree_mutable_tree_ensure_dir (parent, name, &dir, error))
    return FALSE;

  if (!flatpak_mtree_ensure_dir_metadata (repo, dir, NULL, error))
    return FALSE;

  *dir_out = g_steal_pointer (&dir);
  return TRUE;
}

gboolean
flatpak_mtree_create_symlink (OstreeRepo         *repo,
                              OstreeMutableTree  *parent,
                              const char         *filename,
                              const char         *target,
                              GError            **error)
{
  g_autoptr(GFileInfo) file_info = g_file_info_new ();
  g_autoptr(GInputStream) content_stream = NULL;
  g_autofree guchar *raw_checksum = NULL;
  g_autofree char *checksum = NULL;
  guint64 length;

  g_file_info_set_name (file_info, filename);
  g_file_info_set_file_type (file_info, G_FILE_TYPE_SYMBOLIC_LINK);
  g_file_info_set_size (file_info, 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", S_IFLNK | 0777);

  g_file_info_set_attribute_boolean (file_info, "standard::is-symlink", TRUE);
  g_file_info_set_attribute_byte_string (file_info, "standard::symlink-target", target);

  if (!ostree_raw_file_to_content_stream (NULL, file_info, NULL,
                                          &content_stream, &length,
                                          NULL, error))
    return FALSE;

  if (!ostree_repo_write_content (repo, NULL, content_stream, length,
                                  &raw_checksum, NULL, error))
    return FALSE;

  checksum = ostree_checksum_from_bytes (raw_checksum);

  if (!ostree_mutable_tree_replace_file (parent, filename, checksum, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_mtree_add_file_from_bytes (OstreeRepo *repo,
                                   GBytes *bytes,
                                   OstreeMutableTree *parent,
                                   const char *filename,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_autoptr(GFileInfo) info = g_file_info_new ();
  g_autoptr(GInputStream) memstream = NULL;
  g_autoptr(GInputStream) content_stream = NULL;
  g_autofree guchar *raw_checksum = NULL;
  g_autofree char *checksum = NULL;
  guint64 length;

  g_file_info_set_attribute_uint32 (info, "standard::type", G_FILE_TYPE_REGULAR);
  g_file_info_set_attribute_uint64 (info, "standard::size", g_bytes_get_size (bytes));
  g_file_info_set_attribute_uint32 (info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (info, "unix::mode", S_IFREG | 0644);

  memstream = g_memory_input_stream_new_from_bytes (bytes);

  if (!ostree_raw_file_to_content_stream (memstream, info, NULL,
                                          &content_stream, &length,
                                          cancellable, error))
    return FALSE;

  if (!ostree_repo_write_content (repo, NULL, content_stream, length,
                                  &raw_checksum, cancellable, error))
    return FALSE;

  checksum = ostree_checksum_from_bytes (raw_checksum);

  if (!ostree_mutable_tree_replace_file (parent, filename, checksum, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_mtree_ensure_dir_metadata (OstreeRepo        *repo,
                                   OstreeMutableTree *mtree,
                                   GCancellable      *cancellable,
                                   GError           **error)
{
  g_autoptr(GVariant) dirmeta = NULL;
  g_autoptr(GFileInfo) file_info = g_file_info_new ();
  g_autofree guchar *csum = NULL;
  g_autofree char *checksum = NULL;

  g_file_info_set_name (file_info, "/");
  g_file_info_set_file_type (file_info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", 040755);

  dirmeta = ostree_create_directory_metadata (file_info, NULL);
  if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                   dirmeta, &csum, cancellable, error))
    return FALSE;

  checksum = ostree_checksum_from_bytes (csum);
  ostree_mutable_tree_set_metadata_checksum (mtree, checksum);

  return TRUE;
}

static gboolean
copy_icon (const char        *id,
           GFile             *icons_dir,
           OstreeRepo        *repo,
           OstreeMutableTree *size_mtree,
           const char        *size,
           GError           **error)
{
  g_autofree char *icon_name = g_strconcat (id, ".png", NULL);
  g_autoptr(GFile) size_dir = g_file_get_child (icons_dir, size);
  g_autoptr(GFile) icon_file = g_file_get_child (size_dir, icon_name);
  const char *checksum;

  if (!ostree_repo_file_ensure_resolved (OSTREE_REPO_FILE(icon_file), NULL))
    {
      g_info ("No icon at size %s for %s", size, id);
      return TRUE;
    }

  checksum = ostree_repo_file_get_checksum (OSTREE_REPO_FILE(icon_file));
  if (!ostree_mutable_tree_replace_file (size_mtree, icon_name, checksum, error))
    return FALSE;

  return TRUE;
}

static gboolean
extract_appstream (OstreeRepo        *repo,
                   FlatpakXml        *appstream_root,
                   FlatpakDecomposed *ref,
                   const char        *id,
                   OstreeMutableTree *size1_mtree,
                   OstreeMutableTree *size2_mtree,
                   GCancellable       *cancellable,
                   GError            **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) app_info_dir = NULL;
  g_autoptr(GFile) xmls_dir = NULL;
  g_autoptr(GFile) icons_dir = NULL;
  g_autoptr(GFile) appstream_file = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *appstream_basename = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(FlatpakXml) xml_root = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;

  if (!ostree_repo_read_commit (repo, flatpak_decomposed_get_ref (ref), &root, NULL, NULL, error))
    return FALSE;

  keyfile = g_key_file_new ();
  metadata = g_file_get_child (root, "metadata");
  if (g_file_query_exists (metadata, cancellable))
    {
      g_autofree char *content = NULL;
      gsize len;

      if (!g_file_load_contents (metadata, cancellable, &content, &len, NULL, error))
        return FALSE;

      if (!g_key_file_load_from_data (keyfile, content, len, G_KEY_FILE_NONE, error))
        return FALSE;
    }

  app_info_dir = g_file_resolve_relative_path (root, "files/share/app-info");

  xmls_dir = g_file_resolve_relative_path (app_info_dir, "xmls");
  icons_dir = g_file_resolve_relative_path (app_info_dir, "icons/flatpak");

  appstream_basename = g_strconcat (id, ".xml.gz", NULL);
  appstream_file = g_file_get_child (xmls_dir, appstream_basename);

  in = (GInputStream *) g_file_read (appstream_file, cancellable, error);
  if (!in)
    return FALSE;

  xml_root = flatpak_xml_parse (in, TRUE, cancellable, error);
  if (xml_root == NULL)
    return FALSE;

  if (flatpak_appstream_xml_migrate (xml_root, appstream_root,
                                     flatpak_decomposed_get_ref (ref), id, keyfile))
    {
      g_autoptr(GError) my_error = NULL;
      FlatpakXml *components = appstream_root->first_child;
      FlatpakXml *component = components->first_child;

      while (component != NULL)
        {
          FlatpakXml *component_id, *component_id_text_node;
          g_autofree char *component_id_text = NULL;
          char *component_id_suffix;

          if (g_strcmp0 (component->element_name, "component") != 0)
            {
              component = component->next_sibling;
              continue;
            }

          component_id = flatpak_xml_find (component, "id", NULL);
          component_id_text_node = flatpak_xml_find (component_id, NULL, NULL);

          component_id_text = g_strstrip (g_strdup (component_id_text_node->text));

          /* We're looking for a component that matches the app-id (id), but it
             may have some further elements (separated by dot) and can also have
             ".desktop" at the end which we need to strip out. Further complicating
             things, some actual app ids ends in .desktop, such as org.telegram.desktop. */

          component_id_suffix = component_id_text + strlen (id); /* Don't deref before we check for prefix match! */
          if (!g_str_has_prefix (component_id_text, id) ||
              (component_id_suffix[0] != 0 && component_id_suffix[0] != '.'))
            {
              component = component->next_sibling;
              continue;
            }

          if (g_str_has_suffix (component_id_suffix, ".desktop"))
            component_id_suffix[strlen (component_id_suffix) - strlen (".desktop")] = 0;

          if (!copy_icon (component_id_text, icons_dir, repo, size1_mtree, "64x64", &my_error))
            {
              g_print (_("Error copying 64x64 icon for component %s: %s\n"), component_id_text, my_error->message);
              g_clear_error (&my_error);
            }

          if (!copy_icon (component_id_text, icons_dir, repo, size2_mtree, "128x128", &my_error))
             {
               g_print (_("Error copying 128x128 icon for component %s: %s\n"), component_id_text, my_error->message);
               g_clear_error (&my_error);
             }


          /* We might match other prefixes, so keep on going */
          component = component->next_sibling;
        }
    }

  return TRUE;
}

/* This is similar to ostree_repo_list_refs(), but returns only valid flatpak
 * refs, as FlatpakDecomposed. */
static GHashTable *
flatpak_repo_list_flatpak_refs (OstreeRepo   *repo,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(GHashTable) refspecs = NULL;
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter iter;
  gpointer key, value;

  if (!ostree_repo_list_refs_ext (repo, NULL, &refspecs,
                                  OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES | OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_MIRRORS,
                                  cancellable, error))
    return NULL;

  refs = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, g_free);

  g_hash_table_iter_init (&iter, refspecs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *refstr = key;
      const char *checksum = value;
      FlatpakDecomposed *ref = NULL;

      ref = flatpak_decomposed_new_from_ref_take ((char *)refstr, NULL);
      if (ref)
        {
          g_hash_table_iter_steal (&iter);
          g_hash_table_insert (refs, ref, (char *)checksum);
        }
    }

  return g_steal_pointer (&refs);
}

static gboolean
_flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                  const char  **gpg_key_ids,
                                  const char   *gpg_homedir,
                                  FlatpakDecomposed **all_refs_keys,
                                  guint         n_keys,
                                  GHashTable   *all_commits,
                                  const char   *arch,
                                  const char   *subset,
                                  guint64       timestamp,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autoptr(FlatpakXml) appstream_root = NULL;
  g_autoptr(GBytes) xml_data = NULL;
  g_autoptr(GBytes) xml_gz_data = NULL;
  g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  g_autoptr(OstreeMutableTree) icons_mtree = NULL;
  g_autoptr(OstreeMutableTree) icons_flatpak_mtree = NULL;
  g_autoptr(OstreeMutableTree) size1_mtree = NULL;
  g_autoptr(OstreeMutableTree) size2_mtree = NULL;
  const char *compat_arch;
  compat_arch = flatpak_get_compat_arch (arch);
  const char *branch_names[] = { "appstream", "appstream2" };
  const char *collection_id;

  if (subset != NULL && *subset != 0)
    g_info ("Generating appstream for %s, subset %s", arch, subset);
  else
    g_info ("Generating appstream for %s", arch);

  collection_id = ostree_repo_get_collection_id (repo);

  if (!flatpak_mtree_ensure_dir_metadata (repo, mtree, cancellable, error))
    return FALSE;

  if (!flatpak_mtree_create_dir (repo, mtree, "icons", &icons_mtree, error))
    return FALSE;

  if (!flatpak_mtree_create_dir (repo, icons_mtree, "64x64", &size1_mtree, error))
    return FALSE;

  if (!flatpak_mtree_create_dir (repo, icons_mtree, "128x128", &size2_mtree, error))
    return FALSE;

  /* For compatibility with libappstream we create a $origin ("flatpak") subdirectory with symlinks
   * to the size directories thus matching the standard merged appstream layout if we assume the
   * appstream has origin=flatpak, which flatpak-builder creates.
   *
   * See https://github.com/ximion/appstream/pull/224 for details.
   */
  if (!flatpak_mtree_create_dir (repo, icons_mtree, "flatpak", &icons_flatpak_mtree, error))
    return FALSE;
  if (!flatpak_mtree_create_symlink (repo, icons_flatpak_mtree, "64x64", "../64x64", error))
    return FALSE;
  if (!flatpak_mtree_create_symlink (repo, icons_flatpak_mtree, "128x128", "../128x128", error))
    return FALSE;

  appstream_root = flatpak_appstream_xml_new ();

  for (int i = 0; i < n_keys; i++)
    {
      FlatpakDecomposed *ref = all_refs_keys[i];
      GVariant *commit_v = NULL;
      VarMetadataRef commit_metadata;
      g_autoptr(GError) my_error = NULL;
      g_autofree char *id = NULL;

      if (!flatpak_decomposed_is_arch (ref, arch))
        {
          g_autoptr(FlatpakDecomposed) main_ref = NULL;

          /* Include refs that don't match the main arch (e.g. x86_64), if they match
             the compat arch (e.g. i386) and the main arch version is not in the repo */
          if (compat_arch != NULL && flatpak_decomposed_is_arch (ref, compat_arch))
            main_ref = flatpak_decomposed_new_from_decomposed (ref, 0, NULL, compat_arch, NULL, NULL);

          if (main_ref == NULL ||
              g_hash_table_lookup (all_commits, main_ref))
            continue;
        }

      commit_v = g_hash_table_lookup (all_commits, ref);
      g_assert (commit_v != NULL);

      commit_metadata = var_commit_get_metadata (var_commit_from_gvariant (commit_v));
      if (var_metadata_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, NULL, NULL) ||
          var_metadata_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, NULL, NULL))
        {
          g_info (_("%s is end-of-life, ignoring for appstream"), flatpak_decomposed_get_ref (ref));
          continue;
        }

      if (*subset != 0)
        {
          VarVariantRef xa_subsets_v;
          gboolean in_subset = FALSE;

          if (var_metadata_lookup (commit_metadata, "xa.subsets", NULL, &xa_subsets_v))
            {
              VarArrayofstringRef xa_subsets = var_arrayofstring_from_variant (xa_subsets_v);
              gsize len = var_arrayofstring_get_length (xa_subsets);

              for (gsize j = 0; j < len; j++)
                {
                  const char *xa_subset = var_arrayofstring_get_at (xa_subsets, j);
                  if (strcmp (subset, xa_subset) == 0)
                    {
                      in_subset = TRUE;
                      break;
                    }
                }
            }

          if (!in_subset)
            continue;
        }

      id = flatpak_decomposed_dup_id (ref);
      if (!extract_appstream (repo, appstream_root,
                              ref, id, size1_mtree, size2_mtree,
                              cancellable, &my_error))
        {
          if (flatpak_decomposed_is_app (ref))
            g_print (_("No appstream data for %s: %s\n"), flatpak_decomposed_get_ref (ref), my_error->message);
          continue;
        }
    }

  if (!flatpak_appstream_xml_root_to_data (appstream_root, &xml_data, &xml_gz_data, error))
    return FALSE;

  for (int i = 0; i < G_N_ELEMENTS (branch_names); i++)
    {
      gboolean skip_commit = FALSE;
      const char *branch_prefix = branch_names[i];
      g_autoptr(GFile) root = NULL;
      g_autofree char *branch = NULL;
      g_autofree char *parent = NULL;
      g_autofree char *commit_checksum = NULL;

      if (*subset != 0 && i == 0)
        continue; /* No old-style branch for subsets */

      if (*subset != 0)
        branch = g_strdup_printf ("%s/%s-%s", branch_prefix, subset, arch);
      else
        branch = g_strdup_printf ("%s/%s", branch_prefix, arch);

      if (!flatpak_repo_resolve_rev (repo, collection_id, NULL, branch, TRUE,
                                     &parent, cancellable, error))
        return FALSE;

      if (i == 0)
        {
          if (!flatpak_mtree_add_file_from_bytes (repo, xml_gz_data, mtree, "appstream.xml.gz", cancellable, error))
            return FALSE;
        }
      else
        {
          if (!ostree_mutable_tree_remove (mtree, "appstream.xml.gz", TRUE, error))
            return FALSE;

          if (!flatpak_mtree_add_file_from_bytes (repo, xml_data, mtree, "appstream.xml", cancellable, error))
            return FALSE;
        }

      if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
        return FALSE;

      /* No need to commit if nothing changed */
      if (parent)
        {
          g_autoptr(GFile) parent_root = NULL;

          if (!ostree_repo_read_commit (repo, parent, &parent_root, NULL, cancellable, error))
            return FALSE;

          if (g_file_equal (root, parent_root))
            {
              skip_commit = TRUE;
              g_info ("Not updating %s, no change", branch);
            }
        }

      if (!skip_commit)
        {
          g_autoptr(GVariantDict) metadata_dict = NULL;
          g_autoptr(GVariant) metadata = NULL;

          /* Add bindings to the metadata. Do this even if P2P support is not
           * enabled, as it might be enable for other flatpak builds. */
          metadata_dict = g_variant_dict_new (NULL);
          g_variant_dict_insert (metadata_dict, "ostree.collection-binding",
                                 "s", (collection_id != NULL) ? collection_id : "");
          g_variant_dict_insert_value (metadata_dict, "ostree.ref-binding",
                                       g_variant_new_strv ((const gchar * const *) &branch, 1));
          metadata = g_variant_ref_sink (g_variant_dict_end (metadata_dict));

          if (timestamp > 0)
            {
              if (!ostree_repo_write_commit_with_time (repo, parent, "Update", NULL, metadata,
                                                       OSTREE_REPO_FILE (root),
                                                       timestamp,
                                                       &commit_checksum,
                                                       cancellable, error))
                return FALSE;
            }
          else
            {
              if (!ostree_repo_write_commit (repo, parent, "Update", NULL, metadata,
                                             OSTREE_REPO_FILE (root),
                                             &commit_checksum, cancellable, error))
                return FALSE;
            }

          if (gpg_key_ids)
            {
              for (int j = 0; gpg_key_ids[j] != NULL; j++)
                {
                  const char *keyid = gpg_key_ids[j];

                  if (!ostree_repo_sign_commit (repo,
                                                commit_checksum,
                                                keyid,
                                                gpg_homedir,
                                                cancellable,
                                                error))
                    return FALSE;
                }
            }

          g_info ("Creating appstream branch %s", branch);
          if (collection_id != NULL)
            {
              const OstreeCollectionRef collection_ref = { (char *) collection_id, branch };
              ostree_repo_transaction_set_collection_ref (repo, &collection_ref, commit_checksum);
            }
          else
            {
              ostree_repo_transaction_set_ref (repo, NULL, branch, commit_checksum);
            }
        }
    }

  return TRUE;
}

gboolean
flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                 const char  **gpg_key_ids,
                                 const char   *gpg_homedir,
                                 guint64       timestamp,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) all_commits = NULL;
  g_autofree FlatpakDecomposed **all_refs_keys = NULL;
  guint n_keys;
  g_autoptr(GPtrArray) arches = NULL;  /* (element-type utf8 utf8) */
  g_autoptr(GPtrArray) subsets = NULL;  /* (element-type utf8 utf8) */
  g_autoptr(FlatpakRepoTransaction) transaction = NULL;
  OstreeRepoTransactionStats stats;

  arches = g_ptr_array_new_with_free_func (g_free);
  subsets = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (subsets, g_strdup (""));

  all_refs = flatpak_repo_list_flatpak_refs (repo, cancellable, error);
  if (all_refs == NULL)
    return FALSE;

  all_commits = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, (GDestroyNotify)g_variant_unref);

  GLNX_HASH_TABLE_FOREACH_KV (all_refs, FlatpakDecomposed *, ref, const char *, commit)
    {
      VarMetadataRef commit_metadata;
      VarVariantRef xa_subsets_v;
      const char *reverse_compat_arch;
      char *new_arch = NULL;
      g_autoptr(GVariant) commit_v = NULL;

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit, &commit_v, NULL))
        {
          g_warning ("Couldn't load commit %s (ref %s)", commit, flatpak_decomposed_get_ref (ref));
          continue;
        }

      g_hash_table_insert (all_commits, flatpak_decomposed_ref (ref), g_variant_ref (commit_v));

      /* Compute list of subsets */
      commit_metadata = var_commit_get_metadata (var_commit_from_gvariant (commit_v));
      if (var_metadata_lookup (commit_metadata, "xa.subsets", NULL, &xa_subsets_v))
        {
          VarArrayofstringRef xa_subsets = var_arrayofstring_from_variant (xa_subsets_v);
          gsize len = var_arrayofstring_get_length (xa_subsets);
          for (gsize j = 0; j < len; j++)
            {
              const char *subset = var_arrayofstring_get_at (xa_subsets, j);

              if (!flatpak_g_ptr_array_contains_string (subsets, subset))
                g_ptr_array_add (subsets, g_strdup (subset));
            }
        }

      /* Compute list of arches */
      if (!flatpak_decomposed_is_arches (ref, arches->len, (const char **) arches->pdata))
        {
          new_arch = flatpak_decomposed_dup_arch (ref);
          g_ptr_array_add (arches, new_arch);

          /* If repo contains e.g. i386, also generated x86-64 appdata */
          reverse_compat_arch = flatpak_get_compat_arch_reverse (new_arch);
          if (reverse_compat_arch != NULL &&
              !flatpak_g_ptr_array_contains_string (arches, reverse_compat_arch))
            g_ptr_array_add (arches, g_strdup (reverse_compat_arch));
        }
    }

  g_ptr_array_sort (subsets, flatpak_strcmp0_ptr);
  g_ptr_array_sort (arches, flatpak_strcmp0_ptr);

  all_refs_keys = (FlatpakDecomposed **) g_hash_table_get_keys_as_array (all_refs, &n_keys);

  /* Sort refs so that appdata order is stable for e.g. deltas */
  g_qsort_with_data (all_refs_keys, n_keys, sizeof (FlatpakDecomposed *), (GCompareDataFunc) flatpak_decomposed_strcmp_p, NULL);

  transaction = flatpak_repo_transaction_start (repo, cancellable, error);
  if (transaction == NULL)
    return FALSE;

  for (int l = 0; l < subsets->len; l++)
    {
      const char *subset = g_ptr_array_index (subsets, l);

      for (int k = 0; k < arches->len; k++)
        {
          const char *arch = g_ptr_array_index (arches, k);

          if (!_flatpak_repo_generate_appstream (repo,
                                                 gpg_key_ids,
                                                 gpg_homedir,
                                                 all_refs_keys,
                                                 n_keys,
                                                 all_commits,
                                                 arch,
                                                 subset,
                                                 timestamp,
                                                 cancellable,
                                                 error))
            return FALSE;
        }
    }

  if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
    return FALSE;

  return TRUE;
}

#define OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "(uayttay)"
#define OSTREE_STATIC_DELTA_FALLBACK_FORMAT "(yaytt)"
#define OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT "(a{sv}tayay" OSTREE_COMMIT_GVARIANT_STRING "aya" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT ")"

static inline guint64
maybe_swap_endian_u64 (gboolean swap,
                       guint64  v)
{
  if (!swap)
    return v;
  return GUINT64_SWAP_LE_BE (v);
}

static guint64
flatpak_bundle_get_installed_size (GVariant *bundle, gboolean byte_swap)
{
  guint64 total_usize = 0;
  g_autoptr(GVariant) meta_entries = NULL;
  guint i, n_parts;

  g_variant_get_child (bundle, 6, "@a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT, &meta_entries);
  n_parts = g_variant_n_children (meta_entries);

  for (i = 0; i < n_parts; i++)
    {
      guint32 version;
      guint64 size, usize;
      g_autoptr(GVariant) objects = NULL;

      g_variant_get_child (meta_entries, i, "(u@aytt@ay)",
                           &version, NULL, &size, &usize, &objects);

      total_usize += maybe_swap_endian_u64 (byte_swap, usize);
    }

  return total_usize;
}

GVariant *
flatpak_bundle_load (GFile              *file,
                     char              **commit,
                     FlatpakDecomposed **ref,
                     char              **origin,
                     char              **runtime_repo,
                     char              **app_metadata,
                     guint64            *installed_size,
                     GBytes            **gpg_keys,
                     char              **collection_id,
                     GError             **error)
{
  g_autoptr(GVariant) delta = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GBytes) copy = NULL;
  g_autoptr(GVariant) to_csum_v = NULL;

  guint8 endianness_char;
  gboolean byte_swap = FALSE;

  GMappedFile *mfile = g_mapped_file_new (flatpak_file_get_path_cached (file), FALSE, error);

  if (mfile == NULL)
    return NULL;

  bytes = g_mapped_file_get_bytes (mfile);
  g_mapped_file_unref (mfile);

  delta = g_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT), bytes, FALSE);
  g_variant_ref_sink (delta);

  to_csum_v = g_variant_get_child_value (delta, 3);
  if (!ostree_validate_structureof_csum_v (to_csum_v, error))
    return NULL;

  metadata = g_variant_get_child_value (delta, 0);

  if (g_variant_lookup (metadata, "ostree.endianness", "y", &endianness_char))
    {
      int file_byte_order = G_BYTE_ORDER;
      switch (endianness_char)
        {
        case 'l':
          file_byte_order = G_LITTLE_ENDIAN;
          break;

        case 'B':
          file_byte_order = G_BIG_ENDIAN;
          break;

        default:
          break;
        }
      byte_swap = (G_BYTE_ORDER != file_byte_order);
    }

  if (commit)
    *commit = ostree_checksum_from_bytes_v (to_csum_v);

  if (installed_size)
    *installed_size = flatpak_bundle_get_installed_size (delta, byte_swap);

  if (ref != NULL)
    {
      FlatpakDecomposed *the_ref = NULL;
      g_autofree char *ref_str = NULL;

      if (!g_variant_lookup (metadata, "ref", "s", &ref_str))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid bundle, no ref in metadata"));
          return NULL;
        }

      the_ref = flatpak_decomposed_new_from_ref (ref_str, error);
      if (the_ref == NULL)
        return NULL;

      g_clear_pointer (ref, flatpak_decomposed_unref);
      *ref = the_ref;
    }

  if (origin != NULL)
    {
      if (!g_variant_lookup (metadata, "origin", "s", origin))
        *origin = NULL;
    }

  if (runtime_repo != NULL)
    {
      if (!g_variant_lookup (metadata, "runtime-repo", "s", runtime_repo))
        *runtime_repo = NULL;
    }

  if (collection_id != NULL)
    {
      if (!g_variant_lookup (metadata, "collection-id", "s", collection_id))
        {
          *collection_id = NULL;
        }
      else if (**collection_id == '\0')
        {
          g_free (*collection_id);
          *collection_id = NULL;
        }
    }

  if (app_metadata != NULL)
    {
      if (!g_variant_lookup (metadata, "metadata", "s", app_metadata))
        *app_metadata = NULL;
    }

  if (gpg_keys != NULL)
    {
      g_autoptr(GVariant) gpg_value = g_variant_lookup_value (metadata, "gpg-keys",
                                                              G_VARIANT_TYPE ("ay"));
      if (gpg_value)
        {
          gsize n_elements;
          const char *data = g_variant_get_fixed_array (gpg_value, &n_elements, 1);
          *gpg_keys = g_bytes_new (data, n_elements);
        }
      else
        {
          *gpg_keys = NULL;
        }
    }

  /* Make a copy of the data so we can return it after freeing the file */
  copy = g_bytes_new (g_variant_get_data (metadata),
                      g_variant_get_size (metadata));
  return g_variant_ref_sink (g_variant_new_from_bytes (g_variant_get_type (metadata),
                                                       copy,
                                                       FALSE));
}

gboolean
flatpak_pull_from_bundle (OstreeRepo   *repo,
                          GFile        *file,
                          const char   *remote,
                          const char   *ref,
                          gboolean      require_gpg_signature,
                          GCancellable *cancellable,
                          GError      **error)
{
  gsize metadata_size = 0;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *to_checksum = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(OstreeGpgVerifyResult) gpg_result = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GVariant) metadata = NULL;
  gboolean metadata_valid;
  g_autofree char *remote_collection_id = NULL;
  g_autofree char *collection_id = NULL;

  metadata = flatpak_bundle_load (file, &to_checksum, NULL, NULL, NULL, &metadata_contents, NULL, NULL, &collection_id, error);
  if (metadata == NULL)
    return FALSE;

  if (metadata_contents != NULL)
    metadata_size = strlen (metadata_contents);

  if (!ostree_repo_get_remote_option (repo, remote, "collection-id", NULL,
                                      &remote_collection_id, NULL))
    remote_collection_id = NULL;

  if (remote_collection_id != NULL && collection_id != NULL &&
      strcmp (remote_collection_id, collection_id) != 0)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ‘%s’ of bundle doesn’t match collection ‘%s’ of remote"),
                               collection_id, remote_collection_id);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  /* Don’t need to set the collection ID here, since the remote binds this ref to the collection. */
  ostree_repo_transaction_set_ref (repo, remote, ref, to_checksum);

  if (!ostree_repo_static_delta_execute_offline (repo,
                                                 file,
                                                 FALSE,
                                                 cancellable,
                                                 error))
    return FALSE;

  gpg_result = ostree_repo_verify_commit_ext (repo, to_checksum,
                                              NULL, NULL, cancellable, &my_error);
  if (gpg_result == NULL)
    {
      /* no gpg signature, we ignore this *if* there is no gpg key
       * specified in the bundle or by the user */
      if (g_error_matches (my_error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE) &&
          !require_gpg_signature)
        {
          g_clear_error (&my_error);
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }
  else
    {
      /* If there is no valid gpg signature we fail, unless there is no gpg
         key specified (on the command line or in the file) because then we
         trust the source bundle. */
      if (ostree_gpg_verify_result_count_valid (gpg_result) == 0  &&
          require_gpg_signature)
        return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("GPG signatures found, but none are in trusted keyring"));
    }

  if (!ostree_repo_read_commit (repo, to_checksum, &root, NULL, NULL, error))
    return FALSE;

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  /* We ensure that the actual installed metadata matches the one in the
     header, because you may have made decisions on whether to install it or not
     based on that data. */
  metadata_file = g_file_resolve_relative_path (root, "metadata");
  in = (GInputStream *) g_file_read (metadata_file, cancellable, NULL);
  if (in != NULL)
    {
      g_autoptr(GMemoryOutputStream) data_stream = (GMemoryOutputStream *) g_memory_output_stream_new_resizable ();

      if (g_output_stream_splice (G_OUTPUT_STREAM (data_stream), in,
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                  cancellable, error) < 0)
        return FALSE;

      metadata_valid =
        metadata_contents != NULL &&
        metadata_size == g_memory_output_stream_get_data_size (data_stream) &&
        memcmp (metadata_contents, g_memory_output_stream_get_data (data_stream), metadata_size) == 0;
    }
  else
    {
      metadata_valid = (metadata_contents == NULL);
    }

  if (!metadata_valid)
    {
      /* Immediately remove this broken commit */
      ostree_repo_set_ref_immediate (repo, remote, ref, NULL, cancellable, error);
      return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Metadata in header and app are inconsistent"));
    }

  return TRUE;
}
