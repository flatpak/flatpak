/*
 * Copyright © 2016 Red Hat, Inc
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
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#include "libglnx.h"

#include <gpgme.h>
#include <libsoup/soup.h>
#include "flatpak-oci-registry-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-dir-private.h"

#define MAX_JSON_SIZE (1024 * 1024)

typedef struct archive FlatpakAutoArchiveWrite;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakAutoArchiveWrite, archive_write_free)

static void flatpak_oci_registry_initable_iface_init (GInitableIface *iface);

/* A FlatpakOciRegistry represents either:
 *
 *  A local directory with a layout corresponding to the OCI image specification -
 *    we usually use this to store a single image, but it could be used for multiple
 *    images.
 *  A remote docker registry.
 *
 * This code used to support OCI image layouts on remote HTTP servers, but that's not
 * really a thing anybody does. It would be inefficient for storing large numbers of
 * images, since all versions need to be listed in index.json.
 */
struct FlatpakOciRegistry
{
  GObject  parent;

  gboolean for_write;
  gboolean valid;
  gboolean is_docker;
  char    *uri;
  int      tmp_dfd;
  char    *token;

  /* Local repos */
  int dfd;

  /* Remote repos */
  SoupSession *soup_session;
  SoupURI     *base_uri;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakOciRegistryClass;

enum {
  PROP_0,

  PROP_URI,
  PROP_FOR_WRITE,
  PROP_TMP_DFD,
};

G_DEFINE_TYPE_WITH_CODE (FlatpakOciRegistry, flatpak_oci_registry, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                flatpak_oci_registry_initable_iface_init))

static void
flatpak_oci_registry_finalize (GObject *object)
{
  FlatpakOciRegistry *self = FLATPAK_OCI_REGISTRY (object);

  if (self->dfd != -1)
    close (self->dfd);

  g_clear_object (&self->soup_session);
  g_clear_pointer (&self->base_uri, soup_uri_free);
  g_free (self->uri);
  g_free (self->token);

  G_OBJECT_CLASS (flatpak_oci_registry_parent_class)->finalize (object);
}

static void
flatpak_oci_registry_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  FlatpakOciRegistry *self = FLATPAK_OCI_REGISTRY (object);
  const char *uri;

  switch (prop_id)
    {
    case PROP_URI:
      /* Ensure the base uri ends with a / so relative urls work */
      uri = g_value_get_string (value);
      if (g_str_has_suffix (uri, "/"))
        self->uri = g_strdup (uri);
      else
        self->uri = g_strconcat (uri, "/", NULL);
      break;

    case PROP_FOR_WRITE:
      self->for_write = g_value_get_boolean (value);
      break;

    case PROP_TMP_DFD:
      self->tmp_dfd = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_oci_registry_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  FlatpakOciRegistry *self = FLATPAK_OCI_REGISTRY (object);

  switch (prop_id)
    {
    case PROP_URI:
      g_value_set_string (value, self->uri);
      break;

    case PROP_FOR_WRITE:
      g_value_set_boolean (value, self->for_write);
      break;

    case PROP_TMP_DFD:
      g_value_set_int (value, self->tmp_dfd);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_oci_registry_class_init (FlatpakOciRegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_oci_registry_finalize;
  object_class->get_property = flatpak_oci_registry_get_property;
  object_class->set_property = flatpak_oci_registry_set_property;

  g_object_class_install_property (object_class,
                                   PROP_URI,
                                   g_param_spec_string ("uri",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_TMP_DFD,
                                   g_param_spec_int ("tmp-dfd",
                                                     "",
                                                     "",
                                                     -1, G_MAXINT, -1,
                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_FOR_WRITE,
                                   g_param_spec_boolean ("for-write",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
flatpak_oci_registry_init (FlatpakOciRegistry *self)
{
  self->dfd = -1;
  self->tmp_dfd = -1;
}

const char *
flatpak_oci_registry_get_uri (FlatpakOciRegistry *self)
{
  return self->uri;
}

void
flatpak_oci_registry_set_token (FlatpakOciRegistry *self,
                                const char *token)
{
  g_free (self->token);
  self->token = g_strdup (token);
}

FlatpakOciRegistry *
flatpak_oci_registry_new (const char   *uri,
                          gboolean      for_write,
                          int           tmp_dfd,
                          GCancellable *cancellable,
                          GError      **error)
{
  FlatpakOciRegistry *oci_registry;

  oci_registry = g_initable_new (FLATPAK_TYPE_OCI_REGISTRY,
                                 cancellable, error,
                                 "uri", uri,
                                 "for-write", for_write,
                                 "tmp-dfd", tmp_dfd,
                                 NULL);

  return oci_registry;
}

static int
local_open_file (int           dfd,
                 const char   *subpath,
                 struct stat  *st_buf,
                 GCancellable *cancellable,
                 GError      **error)
{
  glnx_autofd int fd = -1;
  struct stat tmp_st_buf;

  do
    fd = openat (dfd, subpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  while (G_UNLIKELY (fd == -1 && errno == EINTR));
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      return -1;
    }

  if (st_buf == NULL)
    st_buf = &tmp_st_buf;

  if (fstat (fd, st_buf) != 0)
    {
      glnx_set_error_from_errno (error);
      return -1;
    }

  if (!S_ISREG (st_buf->st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Non-regular file in OCI registry at %s", subpath);
      return -1;
    }

  return glnx_steal_fd (&fd);
}

static GBytes *
local_load_file (int           dfd,
                 const char   *subpath,
                 GCancellable *cancellable,
                 GError      **error)
{
  glnx_autofd int fd = -1;
  struct stat st_buf;
  GBytes *bytes;

  fd = local_open_file (dfd, subpath, &st_buf, cancellable, error);
  if (fd == -1)
    return NULL;

  bytes = glnx_fd_readall_bytes (fd, cancellable, error);
  if (bytes == NULL)
    return NULL;

  return bytes;
}

static GBytes *
remote_load_file (SoupSession  *soup_session,
                  SoupURI      *base,
                  const char   *subpath,
                  const char   *token,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(SoupURI) uri = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autofree char *uri_s = NULL;

  uri = soup_uri_new_with_base (base, subpath);
  if (uri == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid relative url %s", subpath);
      return NULL;
    }

  uri_s = soup_uri_to_string (uri, FALSE);
  bytes = flatpak_load_uri (soup_session,
                            uri_s, FLATPAK_HTTP_FLAGS_ACCEPT_OCI,
                            token,
                            NULL, NULL,
                            cancellable, error);
  if (bytes == NULL)
    return NULL;

  return g_steal_pointer (&bytes);
}

static GBytes *
flatpak_oci_registry_load_file (FlatpakOciRegistry *self,
                                const char         *subpath,
                                GCancellable       *cancellable,
                                GError            **error)
{
  if (self->dfd != -1)
    return local_load_file (self->dfd, subpath, cancellable, error);
  else
    return remote_load_file (self->soup_session, self->base_uri, subpath, self->token, cancellable, error);
}

static JsonNode *
parse_json (GBytes *bytes, GCancellable *cancellable, GError **error)
{
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   error))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid json, no root object");
      return NULL;
    }

  return json_node_copy (root);
}

static gboolean
verify_oci_version (GBytes *oci_layout_bytes, gboolean *not_json, GCancellable *cancellable, GError **error)
{
  const char *version;
  g_autoptr(JsonNode) node = NULL;
  JsonObject *oci_layout;

  node = parse_json (oci_layout_bytes, cancellable, error);
  if (node == NULL)
    {
      *not_json = TRUE;
      return FALSE;
    }

  *not_json = FALSE;
  oci_layout = json_node_get_object (node);

  version = json_object_get_string_member (oci_layout, "imageLayoutVersion");
  if (version == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unsupported oci repo: oci-layout version missing");
      return FALSE;
    }

  if (strcmp (version, "1.0.0") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsupported existing oci-layout version %s (only 1.0.0 supported)", version);
      return FALSE;
    }

  return TRUE;
}

static gboolean
flatpak_oci_registry_ensure_local (FlatpakOciRegistry *self,
                                   gboolean            for_write,
                                   GCancellable       *cancellable,
                                   GError            **error)
{
  g_autoptr(GFile) dir = g_file_new_for_uri (self->uri);
  glnx_autofd int local_dfd = -1;
  int dfd;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GBytes) oci_layout_bytes = NULL;
  gboolean not_json;

  if (self->dfd != -1)
    dfd = self->dfd;
  else
    {
      if (!glnx_opendirat (AT_FDCWD, flatpak_file_get_path_cached (dir),
                           TRUE, &local_dfd, &local_error))
        {
          if (for_write && g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&local_error);

              if (!glnx_shutil_mkdir_p_at (AT_FDCWD, flatpak_file_get_path_cached (dir), 0755, cancellable, error))
                return FALSE;

              if (!glnx_opendirat (AT_FDCWD, flatpak_file_get_path_cached (dir),
                                   TRUE, &local_dfd, error))
                return FALSE;
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      dfd = local_dfd;
    }

  if (for_write)
    {
      if (!glnx_shutil_mkdir_p_at (dfd, "blobs/sha256", 0755, cancellable, error))
        return FALSE;
    }

  oci_layout_bytes = local_load_file (dfd, "oci-layout", cancellable, &local_error);
  if (oci_layout_bytes == NULL)
    {
      if (for_write && g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          const char *new_layout_data = "{\"imageLayoutVersion\": \"1.0.0\"}";

          g_clear_error (&local_error);

          if (!glnx_file_replace_contents_at (dfd, "oci-layout",
                                              (const guchar *) new_layout_data,
                                              strlen (new_layout_data),
                                              0,
                                              cancellable, error))
            return FALSE;
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }
  else if (!verify_oci_version (oci_layout_bytes, &not_json, cancellable, error))
    return FALSE;

  if (self->dfd == -1 && local_dfd != -1)
    self->dfd = glnx_steal_fd (&local_dfd);

  return TRUE;
}

static gboolean
flatpak_oci_registry_ensure_remote (FlatpakOciRegistry *self,
                                    gboolean            for_write,
                                    GCancellable       *cancellable,
                                    GError            **error)
{
  g_autoptr(SoupURI) baseuri = NULL;

  if (for_write)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Writes are not supported for remote OCI registries");
      return FALSE;
    }

  self->soup_session = flatpak_create_soup_session (PACKAGE_STRING);
  baseuri = soup_uri_new (self->uri);
  if (baseuri == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid url %s", self->uri);
      return FALSE;
    }

  self->is_docker = TRUE;
  self->base_uri = g_steal_pointer (&baseuri);

  return TRUE;
}

static gboolean
flatpak_oci_registry_initable_init (GInitable    *initable,
                                    GCancellable *cancellable,
                                    GError      **error)
{
  FlatpakOciRegistry *self = FLATPAK_OCI_REGISTRY (initable);
  gboolean res;

  if (self->tmp_dfd == -1 &&
      !glnx_opendirat (AT_FDCWD, "/tmp", TRUE, &self->tmp_dfd, error))
    return FALSE;

  if (g_str_has_prefix (self->uri, "file:/"))
    res = flatpak_oci_registry_ensure_local (self, self->for_write, cancellable, error);
  else
    res = flatpak_oci_registry_ensure_remote (self, self->for_write, cancellable, error);

  if (!res)
    return FALSE;

  self->valid = TRUE;

  return TRUE;
}

static void
flatpak_oci_registry_initable_iface_init (GInitableIface *iface)
{
  iface->init = flatpak_oci_registry_initable_init;
}

FlatpakOciIndex *
flatpak_oci_registry_load_index (FlatpakOciRegistry *self,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) local_error = NULL;

  g_assert (self->valid);

  bytes = flatpak_oci_registry_load_file (self, "index.json", cancellable, &local_error);
  if (bytes == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  return (FlatpakOciIndex *) flatpak_json_from_bytes (bytes, FLATPAK_TYPE_OCI_INDEX, error);
}

gboolean
flatpak_oci_registry_save_index (FlatpakOciRegistry *self,
                                 FlatpakOciIndex    *index,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  g_autoptr(GBytes) bytes = NULL;

  g_assert (self->valid);

  bytes = flatpak_json_to_bytes (FLATPAK_JSON (index));

  if (!glnx_file_replace_contents_at (self->dfd, "index.json",
                                      g_bytes_get_data (bytes, NULL),
                                      g_bytes_get_size (bytes),
                                      0, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
write_update_checksum (GOutputStream *out,
                       gconstpointer  data,
                       gsize          len,
                       gsize         *out_bytes_written,
                       GChecksum     *checksum,
                       GCancellable  *cancellable,
                       GError       **error)
{
  if (out)
    {
      if (!g_output_stream_write_all (out, data, len, out_bytes_written,
                                      cancellable, error))
        return FALSE;
    }
  else if (out_bytes_written)
    {
      *out_bytes_written = len;
    }

  if (checksum)
    g_checksum_update (checksum, data, len);

  return TRUE;
}

static gboolean
splice_update_checksum (GOutputStream *out,
                        GInputStream  *in,
                        GChecksum     *checksum,
                        GCancellable  *cancellable,
                        GError       **error)
{
  g_return_val_if_fail (out != NULL || checksum != NULL, FALSE);

  if (checksum != NULL)
    {
      gsize bytes_read, bytes_written;
      char buf[4096];
      do
        {
          if (!g_input_stream_read_all (in, buf, sizeof (buf), &bytes_read, cancellable, error))
            return FALSE;
          if (!write_update_checksum (out, buf, bytes_read, &bytes_written, checksum,
                                      cancellable, error))
            return FALSE;
        }
      while (bytes_read > 0);
    }
  else if (out != NULL)
    {
      if (g_output_stream_splice (out, in, 0, cancellable, error) < 0)
        return FALSE;
    }

  return TRUE;
}

static char *
get_digest_subpath (FlatpakOciRegistry *self,
                    const char         *repository,
                    gboolean            is_manifest,
                    const char         *digest,
                    GError            **error)
{
  g_autoptr(GString) s = g_string_new ("");

  if (!g_str_has_prefix (digest, "sha256:"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsupported digest type %s", digest);
      return NULL;
    }

  if (self->is_docker)
    g_string_append (s, "v2/");

  if (repository)
    {
      g_string_append (s, repository);
      g_string_append (s, "/");
    }

  if (self->is_docker)
    {
      if (is_manifest)
        g_string_append (s, "manifests/");
      else
        g_string_append (s, "blobs/");
      g_string_append (s, digest);
    }
  else
    {
      g_string_append (s, "blobs/sha256/");
      g_string_append (s, digest + strlen ("sha256:"));
    }

  return g_string_free (g_steal_pointer (&s), FALSE);
}

static char *
checksum_fd (int fd, GCancellable *cancellable, GError **error)
{
  g_autoptr(GChecksum) checksum = NULL;
  g_autoptr(GInputStream) in = g_unix_input_stream_new (fd, FALSE);

  checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (!splice_update_checksum (NULL, in, checksum, cancellable, error))
    return NULL;

  return g_strdup (g_checksum_get_string (checksum));
}

int
flatpak_oci_registry_download_blob (FlatpakOciRegistry    *self,
                                    const char            *repository,
                                    gboolean               manifest,
                                    const char            *digest,
                                    FlatpakLoadUriProgress progress_cb,
                                    gpointer               user_data,
                                    GCancellable          *cancellable,
                                    GError               **error)
{
  g_autofree char *subpath = NULL;
  glnx_autofd int fd = -1;

  g_assert (self->valid);

  subpath = get_digest_subpath (self, repository, manifest, digest, error);
  if (subpath == NULL)
    return -1;

  if (self->dfd != -1)
    {
      /* Local case, trust checksum */
      fd = local_open_file (self->dfd, subpath, NULL, cancellable, error);
      if (fd == -1)
        return -1;
    }
  else
    {
      g_autoptr(SoupURI) uri = NULL;
      g_autofree char *uri_s = NULL;
      g_autofree char *checksum = NULL;
      g_autofree char *tmpfile_name = g_strdup_printf ("oci-layer-XXXXXX");
      g_autoptr(GOutputStream) out_stream = NULL;

      /* remote case, download and verify */

      uri = soup_uri_new_with_base (self->base_uri, subpath);
      if (uri == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Invalid relative url %s", subpath);
          return -1;
        }

      uri_s = soup_uri_to_string (uri, FALSE);

      if (!flatpak_open_in_tmpdir_at (self->tmp_dfd, 0600, tmpfile_name,
                                      &out_stream, cancellable, error))
        return -1;

      fd = local_open_file (self->tmp_dfd, tmpfile_name, NULL, cancellable, error);
      (void) unlinkat (self->tmp_dfd, tmpfile_name, 0);

      if (fd == -1)
        return -1;

      if (!flatpak_download_http_uri (self->soup_session, uri_s,
                                      FLATPAK_HTTP_FLAGS_ACCEPT_OCI,
                                      out_stream,
                                      self->token,
                                      progress_cb, user_data,
                                      cancellable, error))
        return -1;

      if (!g_output_stream_close (out_stream, cancellable, error))
        return -1;

      checksum = checksum_fd (fd, cancellable, error);
      if (checksum == NULL)
        return -1;

      if (strcmp (checksum, digest + strlen ("sha256:")) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Checksum digest did not match (%s != %s)", digest, checksum);
          return -1;
        }

      lseek (fd, 0, SEEK_SET);
    }

  return glnx_steal_fd (&fd);
}

gboolean
flatpak_oci_registry_mirror_blob (FlatpakOciRegistry    *self,
                                  FlatpakOciRegistry    *source_registry,
                                  const char            *repository,
                                  gboolean               manifest,
                                  const char            *digest,
                                  FlatpakLoadUriProgress progress_cb,
                                  gpointer               user_data,
                                  GCancellable          *cancellable,
                                  GError               **error)
{
  g_autofree char *src_subpath = NULL;
  g_autofree char *dst_subpath = NULL;
  g_auto(GLnxTmpfile) tmpf = { 0 };
  g_autoptr(GOutputStream) out_stream = NULL;
  struct stat stbuf;
  g_autofree char *checksum = NULL;

  g_assert (self->valid);

  if (!self->for_write)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Write not supported to registry");
      return FALSE;
    }

  src_subpath = get_digest_subpath (source_registry, repository, manifest, digest, error);
  if (src_subpath == NULL)
    return FALSE;

  dst_subpath = get_digest_subpath (self, NULL, manifest, digest, error);
  if (dst_subpath == NULL)
    return FALSE;

  /* Check if its already available */
  if (fstatat (self->dfd, dst_subpath, &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    return TRUE;

  if (!glnx_open_tmpfile_linkable_at (self->dfd, "blobs/sha256",
                                      O_RDWR | O_CLOEXEC | O_NOCTTY,
                                      &tmpf, error))
    return FALSE;

  if (source_registry->dfd != -1)
    {
      glnx_autofd int src_fd = -1;

      src_fd = local_open_file (source_registry->dfd, src_subpath, NULL, cancellable, error);
      if (src_fd == -1)
        return FALSE;

      if (glnx_regfile_copy_bytes (src_fd, tmpf.fd, (off_t) -1) < 0)
        return glnx_throw_errno_prefix (error, "copyfile");
    }
  else
    {
      g_autoptr(SoupURI) uri = NULL;
      g_autofree char *uri_s = NULL;

      uri = soup_uri_new_with_base (source_registry->base_uri, src_subpath);
      if (uri == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Invalid relative url %s", src_subpath);
          return FALSE;
        }

      out_stream = g_unix_output_stream_new (tmpf.fd, FALSE);

      uri_s = soup_uri_to_string (uri, FALSE);
      if (!flatpak_download_http_uri (source_registry->soup_session, uri_s,
                                      FLATPAK_HTTP_FLAGS_ACCEPT_OCI, out_stream,
                                      self->token,
                                      progress_cb, user_data,
                                      cancellable, error))
        return FALSE;

      if (!g_output_stream_close (out_stream, cancellable, error))
        return FALSE;
    }

  lseek (tmpf.fd, 0, SEEK_SET);

  checksum = checksum_fd (tmpf.fd, cancellable, error);
  if (checksum == NULL)
    return FALSE;

  if (strcmp (checksum, digest + strlen ("sha256:")) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Checksum digest did not match (%s != %s)", digest, checksum);
      return FALSE;
    }

  if (!glnx_link_tmpfile_at (&tmpf,
                             GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST,
                             self->dfd, dst_subpath,
                             error))
    return FALSE;

  return TRUE;
}

static const char *
object_get_string_member_with_default (JsonNode *json,
                                       const char *member_name,
                                       const char *default_value)
{
  JsonNode *node;

  if (json == NULL || !JSON_NODE_HOLDS_OBJECT(json))
    return default_value;

  node = json_object_get_member (json_node_get_object (json), member_name);

  if (node == NULL || JSON_NODE_HOLDS_NULL (node) || JSON_NODE_TYPE (node) != JSON_NODE_VALUE)
    return default_value;

  return json_node_get_string (node);
}


static char *
get_token_for_www_auth (FlatpakOciRegistry *self,
                        const char    *repository,
                        const char    *www_authenticate,
                        const char    *auth,
                        GCancellable  *cancellable,
                        GError        **error)
{
  g_autoptr(GInputStream) auth_stream = NULL;
  g_autoptr(SoupMessage) auth_msg = NULL;
  g_autoptr(GHashTable) params = NULL;
  g_autoptr(GHashTable) args = NULL;
  const char *realm, *service, *scope, *token;
  g_autofree char *default_scope = NULL;
  g_autoptr(SoupURI) auth_uri = NULL;
  g_autoptr(GBytes) body = NULL;
  g_autoptr(JsonNode) json = NULL;

  if (g_ascii_strncasecmp (www_authenticate, "Bearer ", strlen ("Bearer ")) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Only Bearer authentication supported");
      return NULL;
    }

  params = soup_header_parse_param_list (www_authenticate + strlen ("Bearer "));

  realm = g_hash_table_lookup (params, "realm");
  if (realm == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Only realm in authentication request");
      return NULL;
    }

  auth_uri = soup_uri_new (realm);
  if (auth_uri == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid realm in authentication request");
      return NULL;
    }

  args = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
  service = g_hash_table_lookup (params, "service");
  if (service)
    g_hash_table_insert (args, "service", (char *)service);

  scope = g_hash_table_lookup (params, "scope");
  if (scope == NULL)
    scope = default_scope = g_strdup_printf("repository:%s:pull", repository);
  g_hash_table_insert (args, "scope", (char *)scope);

  soup_uri_set_query_from_form (auth_uri, args);

  auth_msg = soup_message_new_from_uri ("GET", auth_uri);

  if (auth)
    {
      g_autofree char *basic_auth = g_strdup_printf ("Basic %s", auth);
      soup_message_headers_replace (auth_msg->request_headers, "Authorization", basic_auth);
    }

  auth_stream = soup_session_send (self->soup_session, auth_msg, NULL, error);
  if (auth_stream == NULL)
    return NULL;

  body = flatpak_read_stream (auth_stream, TRUE, error);
  if (body == NULL)
    return NULL;

  json = json_from_string ((char *)g_bytes_get_data (body, NULL), error);
  if (json == NULL)
    return NULL;

  token = object_get_string_member_with_default (json, "token", NULL);
  if (token == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid authentication request response");
      return NULL;
    }

  return g_strdup (token);
}

char *
flatpak_oci_registry_get_token (FlatpakOciRegistry *self,
                                const char         *repository,
                                const char         *digest,
                                const char         *basic_auth,
                                GCancellable       *cancellable,
                                GError            **error)
{
  g_autofree char *subpath = NULL;
  g_autoptr(SoupURI) uri = NULL;
  g_autoptr(GInputStream) stream = NULL;
  g_autoptr(SoupMessage) msg = NULL;
  g_autofree char *www_authenticate = NULL;
  g_autofree char *token = NULL;

  g_assert (self->valid);

  subpath = get_digest_subpath (self, repository, TRUE, digest, error);
  if (subpath == NULL)
    return NULL;

  if (self->dfd != -1)
    return g_strdup (""); // No tokens for local repos

  uri = soup_uri_new_with_base (self->base_uri, subpath);
  if (uri == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid relative url %s", subpath);
      return NULL;
    }

  msg = soup_message_new_from_uri ("HEAD", uri);

  soup_message_headers_replace (msg->request_headers, "Accept",
                                FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST ", " FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2);

  stream = soup_session_send (self->soup_session, msg, NULL, error);
  if (stream == NULL)
    return NULL;

  if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      return g_strdup ("");
    }
  else if (msg->status_code != SOUP_STATUS_UNAUTHORIZED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unexpected response status %d from repo", msg->status_code);
      return NULL;
    }

  /* Need www-authenticated header */
  www_authenticate = g_strdup (soup_message_headers_get_one (msg->response_headers, "WWW-Authenticate"));
  if (www_authenticate == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Now WWW-Authenticate header from repo");
      return NULL;
    }

  token = get_token_for_www_auth (self, repository, www_authenticate, basic_auth, cancellable, error);
  if (token == NULL)
    return NULL;

  return g_steal_pointer (&token);
}

GBytes *
flatpak_oci_registry_load_blob (FlatpakOciRegistry *self,
                                const char         *repository,
                                gboolean            manifest,
                                const char         *digest,
                                GCancellable       *cancellable,
                                GError            **error)
{
  g_autofree char *subpath = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autofree char *json_checksum = NULL;

  g_assert (self->valid);

  subpath = get_digest_subpath (self, repository, manifest, digest, error);
  if (subpath == NULL)
    return NULL;

  bytes = flatpak_oci_registry_load_file (self, subpath, cancellable, error);
  if (bytes == NULL)
    return NULL;

  json_checksum = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, bytes);

  if (strcmp (json_checksum, digest + strlen ("sha256:")) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Checksum for digest %s is wrong (was %s)", digest, json_checksum);
      return NULL;
    }

  return g_steal_pointer (&bytes);
}

char *
flatpak_oci_registry_store_blob (FlatpakOciRegistry *self,
                                 GBytes             *data,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  g_autofree char *sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, data);
  g_autofree char *subpath = NULL;

  g_assert (self->valid);

  subpath = g_strdup_printf ("blobs/sha256/%s", sha256);
  if (!glnx_file_replace_contents_at (self->dfd, subpath,
                                      g_bytes_get_data (data, NULL),
                                      g_bytes_get_size (data),
                                      0, cancellable, error))
    return FALSE;

  return g_strdup_printf ("sha256:%s", sha256);
}

FlatpakOciDescriptor *
flatpak_oci_registry_store_json (FlatpakOciRegistry *self,
                                 FlatpakJson        *json,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  g_autoptr(GBytes) bytes = flatpak_json_to_bytes (json);
  g_autofree char *digest = NULL;

  digest = flatpak_oci_registry_store_blob (self, bytes, cancellable, error);
  if (digest == NULL)
    return NULL;

  return flatpak_oci_descriptor_new (FLATPAK_JSON_CLASS (FLATPAK_JSON_GET_CLASS (json))->mediatype, digest, g_bytes_get_size (bytes));
}

FlatpakOciVersioned *
flatpak_oci_registry_load_versioned (FlatpakOciRegistry *self,
                                     const char         *repository,
                                     const char         *digest,
                                     gsize              *out_size,
                                     GCancellable       *cancellable,
                                     GError            **error)
{
  g_autoptr(GBytes) bytes = NULL;

  g_assert (self->valid);

  bytes = flatpak_oci_registry_load_blob (self, repository, TRUE, digest, cancellable, error);
  if (bytes == NULL)
    return NULL;

  if (out_size)
    *out_size = g_bytes_get_size (bytes);
  return flatpak_oci_versioned_from_json (bytes, error);
}

FlatpakOciImage *
flatpak_oci_registry_load_image_config (FlatpakOciRegistry *self,
                                        const char         *repository,
                                        const char         *digest,
                                        gsize              *out_size,
                                        GCancellable       *cancellable,
                                        GError            **error)
{
  g_autoptr(GBytes) bytes = NULL;

  g_assert (self->valid);

  bytes = flatpak_oci_registry_load_blob (self, repository, FALSE, digest, cancellable, error);
  if (bytes == NULL)
    return NULL;

  if (out_size)
    *out_size = g_bytes_get_size (bytes);
  return flatpak_oci_image_from_json (bytes, error);
}

struct FlatpakOciLayerWriter
{
  GObject             parent;

  FlatpakOciRegistry *registry;

  GChecksum          *uncompressed_checksum;
  GChecksum          *compressed_checksum;
  struct archive     *archive;
  GZlibCompressor    *compressor;
  guint64             uncompressed_size;
  guint64             compressed_size;
  GLnxTmpfile         tmpf;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakOciLayerWriterClass;

G_DEFINE_TYPE (FlatpakOciLayerWriter, flatpak_oci_layer_writer, G_TYPE_OBJECT)

static gboolean
propagate_libarchive_error (GError        **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
  return FALSE;
}

static void
flatpak_oci_layer_writer_reset (FlatpakOciLayerWriter *self)
{
  glnx_tmpfile_clear (&self->tmpf);

  g_checksum_reset (self->uncompressed_checksum);
  g_checksum_reset (self->compressed_checksum);

  if (self->archive)
    {
      archive_write_free (self->archive);
      self->archive = NULL;
    }

  g_clear_object (&self->compressor);
}


static void
flatpak_oci_layer_writer_finalize (GObject *object)
{
  FlatpakOciLayerWriter *self = FLATPAK_OCI_LAYER_WRITER (object);

  flatpak_oci_layer_writer_reset (self);

  g_checksum_free (self->compressed_checksum);
  g_checksum_free (self->uncompressed_checksum);
  glnx_tmpfile_clear (&self->tmpf);

  g_clear_object (&self->registry);

  G_OBJECT_CLASS (flatpak_oci_layer_writer_parent_class)->finalize (object);
}

static void
flatpak_oci_layer_writer_class_init (FlatpakOciLayerWriterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_oci_layer_writer_finalize;
}

static void
flatpak_oci_layer_writer_init (FlatpakOciLayerWriter *self)
{
  self->uncompressed_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  self->compressed_checksum = g_checksum_new (G_CHECKSUM_SHA256);
}

static int
flatpak_oci_layer_writer_open_cb (struct archive *archive,
                                  void           *client_data)
{
  return ARCHIVE_OK;
}

static gssize
flatpak_oci_layer_writer_compress (FlatpakOciLayerWriter *self,
                                   const void            *buffer,
                                   size_t                 length,
                                   gboolean               at_end)
{
  guchar compressed_buffer[8192];
  GConverterResult res;
  gsize total_bytes_read, bytes_read, bytes_written, to_write_len;
  guchar *to_write;
  g_autoptr(GError) local_error = NULL;
  GConverterFlags flags = 0;
  bytes_read = 0;

  total_bytes_read = 0;

  if (at_end)
    flags |= G_CONVERTER_INPUT_AT_END;

  do
    {
      res = g_converter_convert (G_CONVERTER (self->compressor),
                                 buffer, length,
                                 compressed_buffer, sizeof (compressed_buffer),
                                 flags, &bytes_read, &bytes_written,
                                 &local_error);
      if (res == G_CONVERTER_ERROR)
        {
          archive_set_error (self->archive, EIO, "%s", local_error->message);
          return -1;
        }

      g_checksum_update (self->uncompressed_checksum, buffer, bytes_read);
      g_checksum_update (self->compressed_checksum, compressed_buffer, bytes_written);
      self->uncompressed_size += bytes_read;
      self->compressed_size += bytes_written;

      to_write_len = bytes_written;
      to_write = compressed_buffer;
      while (to_write_len > 0)
        {
          ssize_t res = write (self->tmpf.fd, to_write, to_write_len);
          if (res <= 0)
            {
              if (errno == EINTR)
                continue;
              archive_set_error (self->archive, errno, "Write error");
              return -1;
            }

          to_write_len -= res;
          to_write += res;
        }

      total_bytes_read += bytes_read;
    }
  while ((length > 0 && bytes_read == 0) || /* Repeat if we consumed nothing */
         (at_end && res != G_CONVERTER_FINISHED)); /* Or until finished if at_end */

  return total_bytes_read;
}

static ssize_t
flatpak_oci_layer_writer_write_cb (struct archive *archive,
                                   void           *client_data,
                                   const void     *buffer,
                                   size_t          length)
{
  FlatpakOciLayerWriter *self = FLATPAK_OCI_LAYER_WRITER (client_data);

  return flatpak_oci_layer_writer_compress (self, buffer, length, FALSE);
}

static int
flatpak_oci_layer_writer_close_cb (struct archive *archive,
                                   void           *client_data)
{
  FlatpakOciLayerWriter *self = FLATPAK_OCI_LAYER_WRITER (client_data);
  gssize res;
  char buffer[1] = {0};

  res = flatpak_oci_layer_writer_compress (self, &buffer, 0, TRUE);
  if (res < 0)
    return ARCHIVE_FATAL;

  return ARCHIVE_OK;
}

FlatpakOciLayerWriter *
flatpak_oci_registry_write_layer (FlatpakOciRegistry *self,
                                  GCancellable       *cancellable,
                                  GError            **error)
{
  g_autoptr(FlatpakOciLayerWriter) oci_layer_writer = NULL;
  g_autoptr(FlatpakAutoArchiveWrite) a = NULL;
  g_auto(GLnxTmpfile) tmpf = { 0 };

  g_assert (self->valid);

  if (!self->for_write)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Write not supported to registry");
      return NULL;
    }

  oci_layer_writer = g_object_new (FLATPAK_TYPE_OCI_LAYER_WRITER, NULL);
  oci_layer_writer->registry = g_object_ref (self);

  if (!glnx_open_tmpfile_linkable_at (self->dfd,
                                      "blobs/sha256",
                                      O_WRONLY,
                                      &tmpf,
                                      error))
    return NULL;

  if (fchmod (tmpf.fd, 0644) != 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  a = archive_write_new ();
  if (archive_write_set_format_gnutar (a) != ARCHIVE_OK ||
      archive_write_add_filter_none (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      return NULL;
    }

  if (archive_write_open (a, oci_layer_writer,
                          flatpak_oci_layer_writer_open_cb,
                          flatpak_oci_layer_writer_write_cb,
                          flatpak_oci_layer_writer_close_cb) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      return NULL;
    }

  flatpak_oci_layer_writer_reset (oci_layer_writer);

  oci_layer_writer->archive = g_steal_pointer (&a);
  /* Transfer ownership of the tmpfile */
  oci_layer_writer->tmpf = tmpf;
  tmpf.initialized = 0;
  oci_layer_writer->compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);

  return g_steal_pointer (&oci_layer_writer);
}

gboolean
flatpak_oci_layer_writer_close (FlatpakOciLayerWriter *self,
                                char                 **uncompressed_digest_out,
                                FlatpakOciDescriptor **res_out,
                                GCancellable          *cancellable,
                                GError               **error)
{
  g_autofree char *path = NULL;

  if (archive_write_close (self->archive) != ARCHIVE_OK)
    return propagate_libarchive_error (error, self->archive);

  path = g_strdup_printf ("blobs/sha256/%s",
                          g_checksum_get_string (self->compressed_checksum));

  if (!glnx_link_tmpfile_at (&self->tmpf,
                             GLNX_LINK_TMPFILE_REPLACE,
                             self->registry->dfd,
                             path,
                             error))
    return FALSE;

  if (uncompressed_digest_out != NULL)
    *uncompressed_digest_out = g_strdup_printf ("sha256:%s", g_checksum_get_string (self->uncompressed_checksum));
  if (res_out != NULL)
    {
      g_autofree char *digest = g_strdup_printf ("sha256:%s", g_checksum_get_string (self->compressed_checksum));

      *res_out = flatpak_oci_descriptor_new (FLATPAK_OCI_MEDIA_TYPE_IMAGE_LAYER, digest, self->compressed_size);
    }

  return TRUE;
}

struct archive *
flatpak_oci_layer_writer_get_archive (FlatpakOciLayerWriter *self)
{
  return self->archive;
}

typedef struct
{
  int        fd;
  GChecksum *checksum;
  char       buffer[16 * 1024];
} FlatpakArchiveReadWithChecksum;

static int
checksum_open_cb (struct archive *a, void *user_data)
{
  return ARCHIVE_OK;
}


static ssize_t
checksum_read_cb (struct archive *a, void *user_data, const void **buff)
{
  FlatpakArchiveReadWithChecksum *data = user_data;
  ssize_t bytes_read;

  *buff = &data->buffer;
  do
    bytes_read = read (data->fd, &data->buffer, sizeof (data->buffer));
  while (G_UNLIKELY (bytes_read == -1 && errno == EINTR));

  if (bytes_read < 0)
    {
      archive_set_error (a, errno, "Read error on fd %d", data->fd);
      return -1;
    }

  g_checksum_update (data->checksum, (guchar *) data->buffer, bytes_read);

  return bytes_read;
}

static int64_t
checksum_skip_cb (struct archive *a, void *user_data, int64_t request)
{
  FlatpakArchiveReadWithChecksum *data = user_data;
  int64_t old_offset, new_offset;

  if (((old_offset = lseek (data->fd, 0, SEEK_CUR)) >= 0) &&
      ((new_offset = lseek (data->fd, request, SEEK_CUR)) >= 0))
    return new_offset - old_offset;

  archive_set_error (a, errno, "Error seeking");
  return -1;
}

static int
checksum_close_cb (struct archive *a, void *user_data)
{
  FlatpakArchiveReadWithChecksum *data = user_data;

  g_free (data);

  return ARCHIVE_OK;
}

gboolean
flatpak_archive_read_open_fd_with_checksum (struct archive *a,
                                            int             fd,
                                            GChecksum      *checksum,
                                            GError        **error)
{
  FlatpakArchiveReadWithChecksum *data = g_new0 (FlatpakArchiveReadWithChecksum, 1);

  data->fd = fd;
  data->checksum = checksum;

  if (archive_read_open2 (a, data,
                          checksum_open_cb,
                          checksum_read_cb,
                          checksum_skip_cb,
                          checksum_close_cb) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  return TRUE;
}

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
                              _("Unable to lookup key ID %s: %d)"),
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

static const char *
get_image_metadata (FlatpakOciIndexImage *img, const char *key)
{
  if (img->labels != NULL)
    {
      const char *ref = g_hash_table_lookup (img->labels, key);
      if (ref)
        return ref;
    }
  return NULL;
}


static const char *
get_image_ref (FlatpakOciIndexImage *img)
{
  return get_image_metadata (img, "org.flatpak.ref");
}

typedef struct
{
  char                 *repository;
  FlatpakOciIndexImage *image;
} ImageInfo;

static gint
compare_image_by_ref (ImageInfo *a,
                      ImageInfo *b)
{
  const char *a_ref = get_image_ref (a->image);
  const char *b_ref = get_image_ref (b->image);

  return g_strcmp0 (a_ref, b_ref);
}

gboolean
flatpak_oci_index_ensure_cached (SoupSession  *soup_session,
                                 const char   *uri,
                                 GFile        *index,
                                 char        **index_uri_out,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autofree char *index_path = g_file_get_path (index);
  g_autoptr(SoupURI) base_uri = NULL;
  g_autoptr(SoupURI) query_uri = NULL;
  g_autofree char *query_uri_s = NULL;
  g_autoptr(GString) path = NULL;
  g_autofree char *tag = NULL;
  const char *oci_arch = NULL;
  gboolean success = FALSE;
  g_autoptr(GError) local_error = NULL;

  if (!g_str_has_prefix (uri, "oci+http:") && !g_str_has_prefix (uri, "oci+https:"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "OCI Index URI %s does not start with oci+http(s)://", uri);
      return FALSE;
    }

  base_uri = soup_uri_new (uri + 4);
  if (base_uri == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot parse index url %s", uri);
      return FALSE;
    }

  path = g_string_new (soup_uri_get_path (base_uri));

  /* Append /index/static or /static to the path.
   */
  if (!g_str_has_suffix (path->str, "/"))
    g_string_append_c (path, '/');

  if (!g_str_has_suffix (path->str, "/index/"))
    g_string_append (path, "index/");

  g_string_append (path, "static");

  soup_uri_set_path (base_uri, path->str);

  /* The fragment of the URI defines a tag to look for; if absent
   * or empty, we use 'latest'
   */
  tag = g_strdup (soup_uri_get_fragment (base_uri));
  if (tag == NULL || tag[0] == '\0')
    {
      g_clear_pointer (&tag, g_free);
      tag = g_strdup ("latest");
    }

  soup_uri_set_fragment (base_uri, NULL);

  query_uri = soup_uri_copy (base_uri);

  oci_arch = flatpak_arch_to_oci_arch (flatpak_get_arch ());

  soup_uri_set_query_from_fields (query_uri,
                                  "label:org.flatpak.ref:exists", "1",
                                  "architecture", oci_arch,
                                  "os", "linux",
                                  "tag", tag,
                                  NULL);
  query_uri_s = soup_uri_to_string (query_uri, FALSE);

  success = flatpak_cache_http_uri (soup_session,
                                    query_uri_s,
                                    FLATPAK_HTTP_FLAGS_STORE_COMPRESSED,
                                    AT_FDCWD, index_path,
                                    NULL, NULL,
                                    cancellable, &local_error);

  if (success ||
      g_error_matches (local_error, FLATPAK_HTTP_ERROR, FLATPAK_HTTP_ERROR_NOT_CHANGED))
    {
      if (index_uri_out)
        *index_uri_out = soup_uri_to_string (base_uri, FALSE);
    }
  else
    {
      if (index_uri_out)
        *index_uri_out = NULL;
    }

  if (!success)
    g_propagate_error (error, g_steal_pointer (&local_error));

  return success;
}

static FlatpakOciIndexResponse *
load_oci_index (GFile        *index,
                GCancellable *cancellable,
                GError      **error)
{
  g_autoptr(GFileInputStream) in = NULL;
  g_autoptr(GZlibDecompressor) decompressor = NULL;
  g_autoptr(GInputStream) converter = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(FlatpakJson) json = NULL;

  in = g_file_read (index, cancellable, error);
  if (in == NULL)
    return FALSE;

  decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
  converter = g_converter_input_stream_new (G_INPUT_STREAM (in), G_CONVERTER (decompressor));

  json = flatpak_json_from_stream (G_INPUT_STREAM (converter), FLATPAK_TYPE_OCI_INDEX_RESPONSE,
                                   cancellable, error);
  if (json == NULL)
    return NULL;

  if (!g_input_stream_close (G_INPUT_STREAM (in), cancellable, error))
    g_warning ("Error closing http stream: %s", local_error->message);

  return (FlatpakOciIndexResponse *) g_steal_pointer (&json);
}

static GVariant *
maybe_variant_from_base64 (const char *base64)
{
  guchar *bin;
  gsize bin_len;

  if (base64 == NULL)
    return NULL;

  bin = g_base64_decode (base64, &bin_len);
  return g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("v"),
                                                      bin, bin_len, FALSE,
                                                      g_free, bin));
}

GVariant *
flatpak_oci_index_make_summary (GFile        *index,
                                const char   *index_uri,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(FlatpakOciIndexResponse) response = NULL;
  g_autoptr(SoupURI) registry_uri = NULL;
  g_autofree char *registry_uri_s = NULL;
  int i;
  g_autoptr(GArray) images = g_array_new (FALSE, TRUE, sizeof (ImageInfo));
  g_autoptr(GVariantBuilder) refs_builder = NULL;
  g_autoptr(GVariantBuilder) additional_metadata_builder = NULL;
  g_autoptr(GVariantBuilder) ref_sparse_data_builder = NULL;
  g_autoptr(GVariantBuilder) summary_builder = NULL;
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariantBuilder) ref_data_builder = NULL;
  g_autoptr(SoupURI) soup_uri = NULL;

  response = load_oci_index (index, cancellable, error);
  if (!response)
    return NULL;

  soup_uri = soup_uri_new (index_uri);
  registry_uri = soup_uri_new_with_base (soup_uri, response->registry);
  registry_uri_s = soup_uri_to_string (registry_uri, FALSE);

  for (i = 0; response->results != NULL && response->results[i] != NULL; i++)
    {
      FlatpakOciIndexRepository *r = response->results[i];
      int j;
      ImageInfo info = { r->name };

      for (j = 0; r->images != NULL && r->images[j] != NULL; j++)
        {
          info.image = r->images[j];
          g_array_append_val (images, info);
        }

      for (j = 0; r->lists != NULL && r->lists[j] != NULL; j++)
        {
          FlatpakOciIndexImageList *list =  r->lists[j];
          int k;

          for (k = 0; list->images != NULL && list->images[k] != NULL; k++)
            {
              info.image = list->images[k];
              g_array_append_val (images, info);
            }
        }
    }

  refs_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(s(taya{sv}))"));
  ref_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{s(tts)}"));
  additional_metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  ref_sparse_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sa{sv}}"));

  /* The summary has to be sorted by ref */
  g_array_sort (images, (GCompareFunc) compare_image_by_ref);

  for (i = 0; i < images->len; i++)
    {
      ImageInfo *info = &g_array_index (images, ImageInfo, i);
      FlatpakOciIndexImage *image = info->image;
      const char *ref = get_image_ref (image);
      const char *fake_commit;
      guint64 installed_size = 0;
      guint64 download_size = 0;
      const char *installed_size_str;
      const char *download_size_str;
      const char *token_type_base64;
      const char *endoflife_base64;
      const char *endoflife_rebase_base64 = NULL;
      const char *metadata_contents = NULL;
      g_autoptr(GVariantBuilder) ref_metadata_builder = NULL;
      g_autoptr(GVariant) token_type_v = NULL;
      g_autoptr(GVariant) endoflife_v = NULL;
      g_autoptr(GVariant) endoflife_rebase_v = NULL;

      if (ref == NULL)
        continue;

      metadata_contents = get_image_metadata (image, "org.flatpak.metadata");
      if (metadata_contents == NULL && !g_str_has_prefix (ref, "appstream/"))
        continue; /* Not a flatpak, skip */

      if (!g_str_has_prefix (image->digest, "sha256:"))
        {
          g_debug ("Ignoring digest type %s", image->digest);
          continue;
        }

      fake_commit = image->digest + strlen ("sha256:");

      installed_size_str = get_image_metadata (image, "org.flatpak.installed-size");
      if (installed_size_str)
        installed_size = g_ascii_strtoull (installed_size_str, NULL, 10);

      download_size_str = get_image_metadata (image, "org.flatpak.download-size");
      if (download_size_str)
        download_size = g_ascii_strtoull (download_size_str, NULL, 10);

      ref_metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

      g_variant_builder_add (ref_metadata_builder, "{sv}", "xa.oci-repository",
                             g_variant_new_string (info->repository));

      g_variant_builder_add_value (refs_builder,
                                   g_variant_new ("(s(t@ay@a{sv}))", ref,
                                                  (guint64) 0,
                                                  ostree_checksum_to_bytes_v (fake_commit),
                                                  g_variant_builder_end (ref_metadata_builder)));
      g_variant_builder_add (ref_data_builder, "{s(tts)}",
                             ref,
                             GUINT64_TO_BE (installed_size),
                             GUINT64_TO_BE (download_size),
                             metadata_contents ? metadata_contents : "");

      token_type_base64 = get_image_metadata (image, "org.flatpak.commit-metadata.xa.token-type");
      token_type_v = maybe_variant_from_base64 (token_type_base64);
      endoflife_base64 = get_image_metadata (image, "org.flatpak.commit-metadata.ostree.endoflife");
      endoflife_v = maybe_variant_from_base64 (endoflife_base64);
      endoflife_rebase_base64 = get_image_metadata (image, "org.flatpak.commit-metadata.ostree.endoflife-rebase");
      endoflife_rebase_v = maybe_variant_from_base64 (endoflife_rebase_base64);

      if (token_type_v != NULL ||
          endoflife_v != NULL ||
          endoflife_rebase_v != NULL)
        {
          g_autoptr(GVariantBuilder) sparse_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

          if (token_type_v != NULL)
            g_variant_builder_add (sparse_builder, "{s@v}", FLATPAK_SPARSE_CACHE_KEY_TOKEN_TYPE, token_type_v);
          if (endoflife_v != NULL)
            g_variant_builder_add (sparse_builder, "{s@v}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE, endoflife_v);
          if (endoflife_rebase_v != NULL)
            g_variant_builder_add (sparse_builder, "{s@v}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE, endoflife_rebase_v);

          g_variant_builder_add (ref_sparse_data_builder, "{s@a{sv}}",
                                 ref, g_variant_builder_end (sparse_builder));
        }
    }

  g_variant_builder_add (additional_metadata_builder, "{sv}", "xa.cache",
                         g_variant_new_variant (g_variant_builder_end (ref_data_builder)));
  g_variant_builder_add (additional_metadata_builder, "{sv}", "xa.sparse-cache",
                         g_variant_builder_end (ref_sparse_data_builder));
  g_variant_builder_add (additional_metadata_builder, "{sv}", "xa.oci-registry-uri",
                         g_variant_new_string (registry_uri_s));

  summary_builder = g_variant_builder_new (OSTREE_SUMMARY_GVARIANT_FORMAT);

  g_variant_builder_add_value (summary_builder, g_variant_builder_end (refs_builder));
  g_variant_builder_add_value (summary_builder, g_variant_builder_end (additional_metadata_builder));

  summary = g_variant_ref_sink (g_variant_builder_end (summary_builder));

  return g_steal_pointer (&summary);
}

static gboolean
add_icon_image (SoupSession  *soup_session,
                const char   *index_uri,
                int           icons_dfd,
                GHashTable   *used_icons,
                const char   *subdir,
                const char   *id,
                const char   *icon_data,
                GCancellable *cancellable,
                GError      **error)
{
  g_autofree char *icon_name = g_strconcat (id, ".png", NULL);
  g_autofree char *icon_path = g_build_filename (subdir, icon_name, NULL);

  /* Create the destination directory */

  if (!glnx_shutil_mkdir_p_at (icons_dfd, subdir, 0755, cancellable, error))
    return FALSE;

  if (g_str_has_prefix (icon_data, "data:"))
    {
      if (g_str_has_prefix (icon_data, "data:image/png;base64,"))
        {
          const char *base64_data = icon_data + strlen ("data:image/png;base64,");
          gsize decoded_size;
          g_autofree guint8 *decoded = g_base64_decode (base64_data, &decoded_size);

          if (!glnx_file_replace_contents_at (icons_dfd, icon_path,
                                              decoded, decoded_size,
                                              0 /* flags */, cancellable, error))
            return FALSE;

          g_hash_table_replace (used_icons, g_steal_pointer (&icon_path), GUINT_TO_POINTER (1));

          return TRUE;
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Data URI for icon has an unsupported type");
          return FALSE;
        }
    }
  else
    {
      g_autoptr(SoupURI) base_uri = soup_uri_new (index_uri);
      g_autoptr(SoupURI) icon_uri = soup_uri_new_with_base (base_uri, icon_data);
      g_autofree char *icon_uri_s = soup_uri_to_string (icon_uri, FALSE);
      g_autoptr(GError) local_error = NULL;

      if (!flatpak_cache_http_uri (soup_session, icon_uri_s,
                                   0 /* flags */,
                                   icons_dfd, icon_path,
                                   NULL, NULL,
                                   cancellable, &local_error) &&
          !g_error_matches (local_error, FLATPAK_HTTP_ERROR, FLATPAK_HTTP_ERROR_NOT_CHANGED))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      g_hash_table_replace (used_icons, g_steal_pointer (&icon_path), GUINT_TO_POINTER (1));

      return TRUE;
    }
}

static void
add_image_to_appstream (SoupSession               *soup_session,
                        const char                *index_uri,
                        FlatpakXml                *appstream_root,
                        int                        icons_dfd,
                        GHashTable                *used_icons,
                        FlatpakOciIndexRepository *repository,
                        FlatpakOciIndexImage      *image,
                        GCancellable              *cancellable)
{
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakXml) xml_root = NULL;
  g_auto(GStrv) ref_parts = NULL;
  const char *ref;
  const char *id = NULL;
  FlatpakXml *source_components;
  FlatpakXml *dest_components;
  FlatpakXml *component;
  FlatpakXml *prev_component;
  const char *appdata;
  int i;

  static struct
  {
    const char *label;
    const char *subdir;
  } icon_sizes[] = {
    { "org.freedesktop.appstream.icon-64", "64x64" },
    { "org.freedesktop.appstream.icon-128", "128x128" },
  };

  ref = get_image_ref (image);
  if (!ref)
    return;

  ref_parts = g_strsplit (ref, "/", -1);
  if (g_strv_length (ref_parts) != 4 || strcmp (ref_parts[0], "app") != 0)
    return;

  id = ref_parts[1];

  appdata = get_image_metadata (image, "org.freedesktop.appstream.appdata");
  if (!appdata)
    return;

  in = g_memory_input_stream_new_from_data (appdata, -1, NULL);

  xml_root = flatpak_xml_parse (in, FALSE, cancellable, &error);
  if (xml_root == NULL)
    {
      g_print ("%s: Failed to parse appdata annotation: %s\n",
               repository->name,
               error->message);
      return;
    }

  if (xml_root->first_child == NULL ||
      xml_root->first_child->next_sibling != NULL ||
      g_strcmp0 (xml_root->first_child->element_name, "components") != 0)
    {
      return;
    }

  source_components = xml_root->first_child;
  dest_components = appstream_root->first_child;

  component = source_components->first_child;
  prev_component = NULL;
  while (component != NULL)
    {
      FlatpakXml *next = component->next_sibling;

      if (g_strcmp0 (component->element_name, "component") == 0)
        {
          flatpak_xml_add (dest_components,
                           flatpak_xml_unlink (component, prev_component));
        }
      else
        {
          prev_component = component;
        }

      component = next;
    }

  for (i = 0; i < G_N_ELEMENTS (icon_sizes); i++)
    {
      const char *icon_data = get_image_metadata (image, icon_sizes[i].label);
      if (icon_data)
        {
          if (!add_icon_image (soup_session,
                               index_uri,
                               icons_dfd,
                               used_icons,
                               icon_sizes[i].subdir, id, icon_data,
                               cancellable, &error))
            {
              g_print ("%s: Failed to add %s icon: %s\n",
                       repository->name,
                       icon_sizes[i].subdir,
                       error->message);
              g_clear_error (&error);
            }
        }
    }
}

static gboolean
clean_unused_icons_recurse (int           icons_dfd,
                            const char   *dirpath,
                            GHashTable   *used_icons,
                            gboolean     *any_found_parent,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_auto(GLnxDirFdIterator) iter = { 0, };
  gboolean any_found = FALSE;

  if (!glnx_dirfd_iterator_init_at (icons_dfd,
                                    dirpath ? dirpath : ".",
                                    FALSE, &iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *out_dent;
      g_autofree char *subpath = NULL;

      if (!glnx_dirfd_iterator_next_dent (&iter, &out_dent, cancellable, error))
        return FALSE;

      if (out_dent == NULL)
        break;

      if (dirpath)
        subpath = g_build_filename (dirpath, out_dent->d_name, NULL);
      else
        subpath = g_strdup (out_dent->d_name);

      if (out_dent->d_type == DT_DIR)
        clean_unused_icons_recurse (icons_dfd, subpath, used_icons, &any_found, cancellable, error);
      else if (g_hash_table_lookup (used_icons, subpath) == NULL)
        {
          if (!glnx_unlinkat (icons_dfd, subpath, 0, error))
            return FALSE;
        }
      else
        any_found = TRUE;
    }

  if (any_found)
    {
      if (any_found_parent)
        *any_found_parent = TRUE;
    }
  else
    {
      if (dirpath) /* Don't remove the toplevel icons/ directory */
        if (!glnx_unlinkat (icons_dfd, dirpath, AT_REMOVEDIR, error))
          return FALSE;
    }

  return TRUE;
}

static gboolean
clean_unused_icons (int           icons_dfd,
                    GHashTable   *used_icons,
                    GCancellable *cancellable,
                    GError      **error)
{
  return clean_unused_icons_recurse (icons_dfd, NULL, used_icons, NULL, cancellable, error);
}

GBytes *
flatpak_oci_index_make_appstream (SoupSession  *soup_session,
                                  GFile        *index,
                                  const char   *index_uri,
                                  const char   *arch,
                                  int           icons_dfd,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autoptr(FlatpakOciIndexResponse) response = NULL;
  g_autoptr(FlatpakXml) appstream_root = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GHashTable) used_icons = NULL;
  int i;

  const char *oci_arch = flatpak_arch_to_oci_arch (arch);

  response = load_oci_index (index, cancellable, error);
  if (!response)
    return NULL;

  used_icons = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      g_free, NULL);

  appstream_root = flatpak_appstream_xml_new ();

  for (i = 0; response->results != NULL && response->results[i] != NULL; i++)
    {
      FlatpakOciIndexRepository *r = response->results[i];
      int j;

      for (j = 0; r->images != NULL && r->images[j] != NULL; j++)
        {
          FlatpakOciIndexImage *image = r->images[j];
          if (g_strcmp0 (image->architecture, oci_arch) == 0)
            add_image_to_appstream (soup_session,
                                    index_uri,
                                    appstream_root, icons_dfd, used_icons,
                                    r, image,
                                    cancellable);
        }

      for (j = 0; r->lists != NULL && r->lists[j] != NULL; j++)
        {
          FlatpakOciIndexImageList *list =  r->lists[j];
          int k;

          for (k = 0; list->images != NULL && list->images[k] != NULL; k++)
            {
              FlatpakOciIndexImage *image = list->images[k];
              if (g_strcmp0 (image->architecture, oci_arch) == 0)
                add_image_to_appstream (soup_session,
                                        index_uri,
                                        appstream_root, icons_dfd, used_icons,
                                        r, image,
                                        cancellable);
            }
        }
    }

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  if (!flatpak_appstream_xml_root_to_data (appstream_root,
                                           &bytes, NULL, error))
    return NULL;

  if (!clean_unused_icons (icons_dfd, used_icons, cancellable, error))
    return FALSE;

  return g_steal_pointer (&bytes);
}
