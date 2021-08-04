/*
 * Copyright © 2018 Red Hat, Inc
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

#include "flatpak-utils-http-private.h"
#include "flatpak-oci-registry-private.h"

#include <glib/gi18n-lib.h>

#include <gio/gunixoutputstream.h>
#include <libsoup/soup.h>
#include "libglnx/libglnx.h"

#include <sys/types.h>
#include <sys/xattr.h>

/* copied from libostree */
#define DEFAULT_N_NETWORK_RETRIES 5

G_DEFINE_QUARK (flatpak_http_error, flatpak_http_error)

typedef struct
{
  char  *uri;
  char  *etag;
  gint64 last_modified;
  gint64 expires;
} CacheHttpData;

typedef struct
{
  GMainContext          *context;
  gboolean               done;
  GError                *error;
  gboolean               store_compressed;

  GOutputStream         *out; /*or */
  GString               *content; /* or */
  GLnxTmpfile           *out_tmpfile;
  int                    out_tmpfile_parent_dfd;

  guint64                downloaded_bytes;
  char                   buffer[16 * 1024];
  FlatpakLoadUriProgress progress;
  GCancellable          *cancellable;
  gpointer               user_data;
  guint64                last_progress_time;
  CacheHttpData         *cache_data;
  char                 **content_type_out;
} LoadUriData;

#define CACHE_HTTP_XATTR "user.flatpak.http"
#define CACHE_HTTP_SUFFIX ".flatpak.http"
#define CACHE_HTTP_TYPE "(sstt)"

static void
clear_cache_http_data (CacheHttpData *data,
                       gboolean       clear_uri)
{
  if (clear_uri)
    g_clear_pointer (&data->uri, g_free);
  g_clear_pointer (&data->etag, g_free);
  data->last_modified = 0;
  data->expires = 0;
}

static void
free_cache_http_data (CacheHttpData *data)
{
  clear_cache_http_data (data, TRUE);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CacheHttpData, free_cache_http_data)

static GBytes *
serialize_cache_http_data (CacheHttpData * data)
{
  g_autoptr(GVariant) cache_variant = NULL;

  cache_variant = g_variant_ref_sink (g_variant_new (CACHE_HTTP_TYPE,
                                                     data->uri,
                                                     data->etag ? data->etag : "",
                                                     data->last_modified,
                                                     data->expires));
  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      g_autoptr(GVariant) tmp_variant = cache_variant;
      cache_variant = g_variant_byteswap (tmp_variant);
    }

  return g_variant_get_data_as_bytes (cache_variant);
}

static void
deserialize_cache_http_data (CacheHttpData *data,
                             GBytes        *bytes)
{
  g_autoptr(GVariant) cache_variant = NULL;

  cache_variant = g_variant_ref_sink (g_variant_new_from_bytes (G_VARIANT_TYPE (CACHE_HTTP_TYPE),
                                                                bytes,
                                                                FALSE));
  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      g_autoptr(GVariant) tmp_variant = cache_variant;
      cache_variant = g_variant_byteswap (tmp_variant);
    }

  g_variant_get (cache_variant,
                 CACHE_HTTP_TYPE,
                 &data->uri,
                 &data->etag,
                 &data->last_modified,
                 &data->expires);
}

static CacheHttpData *
load_cache_http_data (int           dfd,
                      char         *name,
                      gboolean     *no_xattr,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(CacheHttpData) data = NULL;
  g_autoptr(GBytes) cache_bytes = glnx_lgetxattrat (dfd, name,
                                                    CACHE_HTTP_XATTR,
                                                    error);
  if (cache_bytes == NULL)
    {
      if (errno == ENOTSUP)
        {
          g_autofree char *cache_file = NULL;
          glnx_autofd int fd = -1;

          g_clear_error (error);
          *no_xattr = TRUE;

          cache_file = g_strconcat (name, CACHE_HTTP_SUFFIX, NULL);

          if (!glnx_openat_rdonly (dfd, cache_file, FALSE,
                                   &fd, error))
            return FALSE;

          cache_bytes = glnx_fd_readall_bytes (fd, cancellable, error);
          if (!cache_bytes)
            return NULL;
        }
      else if (errno == ENOENT || errno == ENODATA)
        {
          g_clear_error (error);
          return g_new0 (CacheHttpData, 1);
        }
      else
        {
          return NULL;
        }
    }


  data = g_new0 (CacheHttpData, 1);
  deserialize_cache_http_data (data, cache_bytes);
  return g_steal_pointer (&data);
}

static void
set_cache_http_data_from_headers (CacheHttpData *data,
                                  SoupMessage   *msg)
{
  const char *etag = soup_message_headers_get_one (msg->response_headers, "ETag");
  const char *last_modified = soup_message_headers_get_one (msg->response_headers, "Last-Modified");
  const char *cache_control = soup_message_headers_get_list (msg->response_headers, "Cache-Control");
  const char *expires = soup_message_headers_get_list (msg->response_headers, "Expires");
  gboolean expires_computed = FALSE;

  /* The original HTTP 1/1 specification only required sending the ETag header in a 304
   * response, and implied that a cache might need to save the old Cache-Control
   * values. The updated RFC 7232 from 2014 requires sending Cache-Control, ETags, and
   * Expire if they would have been sent in the original 200 response, and recommends
   * sending Last-Modified for requests without an etag. Since sending these headers was
   * apparently normal previously, for simplicity we assume the RFC 7232 behavior and start
   * from scratch for a 304 response.
   */
  clear_cache_http_data (data, FALSE);

  if (etag && *etag)
    {
      data->etag = g_strdup (etag);
    }
  else if (last_modified && *last_modified)
    {
      SoupDate *date = soup_date_new_from_string (last_modified);
      if (date)
        {
          data->last_modified = soup_date_to_time_t (date);
          soup_date_free (date);
        }
    }

  if (cache_control && *cache_control)
    {
      g_autoptr(GHashTable) params = soup_header_parse_param_list (cache_control);
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, params);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          if (g_strcmp0 (key, "max-age") == 0)
            {
              char *end;

              char *max_age = value;
              int max_age_sec = g_ascii_strtoll (max_age,  &end, 10);
              if (*max_age != '\0' && *end == '\0')
                {
                  GTimeVal now;
                  g_get_current_time (&now);
                  data->expires = now.tv_sec + max_age_sec;
                  expires_computed = TRUE;
                }
            }
          else if (g_strcmp0 (key, "no-cache") == 0)
            {
              data->expires = 0;
              expires_computed = TRUE;
            }
        }
    }

  if (!expires_computed && expires && *expires)
    {
      SoupDate *date = soup_date_new_from_string (expires);
      if (date)
        {
          data->expires = soup_date_to_time_t (date);
          soup_date_free (date);
          expires_computed = TRUE;
        }
    }

  if (!expires_computed)
    {
      /* If nothing implies an expires time, use 30 minutes. Browsers use
       * 0.1 * (Date - Last-Modified), but it's clearly appropriate here, and
       * better if server's send a value.
       */
      GTimeVal now;
      g_get_current_time (&now);
      data->expires = now.tv_sec + 1800;
    }
}

static gboolean
save_cache_http_data_xattr (int      fd,
                            GBytes  *bytes,
                            GError **error)
{
  if (TEMP_FAILURE_RETRY (fsetxattr (fd, (char *) CACHE_HTTP_XATTR,
                                     g_bytes_get_data (bytes, NULL),
                                     g_bytes_get_size (bytes),
                                     0)) < 0)
    return glnx_throw_errno_prefix (error, "fsetxattr");

  return TRUE;
}

static gboolean
save_cache_http_data_fallback (int      fd,
                               GBytes  *bytes,
                               GError **error)
{
  if (glnx_loop_write (fd,
                       g_bytes_get_data (bytes, NULL),
                       g_bytes_get_size (bytes)) < 0)
    return glnx_throw_errno_prefix (error, "write");

  return TRUE;
}

static gboolean
save_cache_http_data_to_file (int           dfd,
                              char         *name,
                              GBytes       *bytes,
                              gboolean      no_xattr,
                              GCancellable *cancellable,
                              GError      **error)
{
  glnx_autofd int fd = -1;
  g_autofree char *fallback_name = NULL;

  if (!no_xattr)
    {
      if (!glnx_openat_rdonly (dfd, name, FALSE,
                               &fd, error))
        return FALSE;

      if (save_cache_http_data_xattr (fd, bytes, error))
        return TRUE;

      if (errno == ENOTSUP)
        g_clear_error (error);
      else
        return FALSE;
    }

  fallback_name = g_strconcat (name, CACHE_HTTP_SUFFIX, NULL);
  if (!glnx_file_replace_contents_at (dfd, fallback_name,
                                      g_bytes_get_data (bytes, NULL),
                                      g_bytes_get_size (bytes),
                                      0,
                                      cancellable,
                                      error))
    return FALSE;

  return TRUE;
}

static void
stream_closed (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoadUriData *data = user_data;
  GInputStream *stream = G_INPUT_STREAM (source);
  g_autoptr(GError) error = NULL;

  if (!g_input_stream_close_finish (stream, res, &error))
    g_warning (_("Error closing http stream: %s"), error->message);

  if (data->out_tmpfile)
    {
      if (!g_output_stream_close (data->out, data->cancellable, &error))
        {
          if (data->error == NULL)
            g_propagate_error (&data->error, g_steal_pointer (&error));
        }

      g_clear_pointer (&data->out, g_object_unref);
    }

  data->done = TRUE;
  g_main_context_wakeup (data->context);
}

static void
load_uri_read_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoadUriData *data = user_data;
  GInputStream *stream = G_INPUT_STREAM (source);
  gssize nread;

  nread = g_input_stream_read_finish (stream, res, &data->error);
  if (nread == -1 || nread == 0)
    {
      if (data->progress)
        data->progress (data->downloaded_bytes, data->user_data);
      g_input_stream_close_async (stream,
                                  G_PRIORITY_DEFAULT, NULL,
                                  stream_closed, data);

      return;
    }

  if (data->out != NULL)
    {
      gsize n_written;

      if (!g_output_stream_write_all (data->out, data->buffer, nread, &n_written,
                                      NULL, &data->error))
        {
          data->downloaded_bytes += n_written;
          g_input_stream_close_async (stream,
                                      G_PRIORITY_DEFAULT, NULL,
                                      stream_closed, data);
          return;
        }

      data->downloaded_bytes += n_written;
    }
  else
    {
      data->downloaded_bytes += nread;
      g_string_append_len (data->content, data->buffer, nread);
    }

  if (g_get_monotonic_time () - data->last_progress_time > 1 * G_USEC_PER_SEC)
    {
      if (data->progress)
        data->progress (data->downloaded_bytes, data->user_data);
      data->last_progress_time = g_get_monotonic_time ();
    }

  g_input_stream_read_async (stream, data->buffer, sizeof (data->buffer),
                             G_PRIORITY_DEFAULT, data->cancellable,
                             load_uri_read_cb, data);
}

static void
load_uri_callback (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  SoupRequestHTTP *request = SOUP_REQUEST_HTTP (source_object);
  g_autoptr(GInputStream) in = NULL;
  LoadUriData *data = user_data;

  in = soup_request_send_finish (SOUP_REQUEST (request), res, &data->error);
  if (in == NULL)
    {
      /* data->error has been set */
      g_main_context_wakeup (data->context);
      return;
    }

  g_autoptr(SoupMessage) msg = soup_request_http_get_message ((SoupRequestHTTP *) request);
  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      int code;
      GQuark domain = G_IO_ERROR;

      switch (msg->status_code)
        {
        case 304:
          if (data->cache_data)
            set_cache_http_data_from_headers (data->cache_data, msg);

          domain = FLATPAK_HTTP_ERROR;
          code = FLATPAK_HTTP_ERROR_NOT_CHANGED;
          break;

        case 401:
          domain = FLATPAK_HTTP_ERROR;
          code = FLATPAK_HTTP_ERROR_UNAUTHORIZED;
          break;

        case 403:
        case 404:
        case 410:
          code = G_IO_ERROR_NOT_FOUND;
          break;

        case 408:
          code = G_IO_ERROR_TIMED_OUT;
          break;

        case SOUP_STATUS_CANCELLED:
          code = G_IO_ERROR_CANCELLED;
          break;

        case SOUP_STATUS_CANT_RESOLVE:
        case SOUP_STATUS_CANT_CONNECT:
          code = G_IO_ERROR_HOST_NOT_FOUND;
          break;

        case SOUP_STATUS_INTERNAL_SERVER_ERROR:
          /* The server did return something, but it was useless to us, so that’s basically equivalent to not returning */
          code = G_IO_ERROR_HOST_UNREACHABLE;
          break;

        case SOUP_STATUS_IO_ERROR:
#if !GLIB_CHECK_VERSION(2, 44, 0)
          code = G_IO_ERROR_BROKEN_PIPE;
#else
          code = G_IO_ERROR_CONNECTION_CLOSED;
#endif
          break;

        default:
          code = G_IO_ERROR_FAILED;
        }

      data->error = g_error_new (domain, code,
                                 "Server returned status %u: %s",
                                 msg->status_code,
                                 soup_status_get_phrase (msg->status_code));
      g_main_context_wakeup (data->context);
      return;
    }

  if (data->cache_data)
    set_cache_http_data_from_headers (data->cache_data, msg);

  if (data->content_type_out)
    *data->content_type_out = g_strdup (soup_message_headers_get_content_type (msg->response_headers, NULL));

  if (data->out_tmpfile)
    {
      g_autoptr(GOutputStream) out = NULL;

      if (!glnx_open_tmpfile_linkable_at (data->out_tmpfile_parent_dfd, ".",
                                          O_WRONLY, data->out_tmpfile,
                                          &data->error))
        return;

      g_assert (data->out == NULL);

      out = g_unix_output_stream_new (data->out_tmpfile->fd, FALSE);
      if (data->store_compressed &&
          g_strcmp0 (soup_message_headers_get_one (msg->response_headers, "Content-Encoding"), "gzip") != 0)
        {
          g_autoptr(GZlibCompressor) compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
          data->out = g_converter_output_stream_new (out, G_CONVERTER (compressor));
        }
      else
        {
          data->out = g_steal_pointer (&out);
        }
    }

  g_input_stream_read_async (in, data->buffer, sizeof (data->buffer),
                             G_PRIORITY_DEFAULT, data->cancellable,
                             load_uri_read_cb, data);
}

SoupSession *
flatpak_create_soup_session (const char *user_agent)
{
  SoupSession *soup_session;
  const char *http_proxy;

  soup_session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT, user_agent,
                                                SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                SOUP_SESSION_TIMEOUT, 60,
                                                SOUP_SESSION_IDLE_TIMEOUT, 60,
                                                NULL);
  soup_session_remove_feature_by_type (soup_session, SOUP_TYPE_CONTENT_DECODER);
  http_proxy = g_getenv ("http_proxy");
  if (http_proxy)
    {
      g_autoptr(SoupURI) proxy_uri = soup_uri_new (http_proxy);
      if (!proxy_uri)
        g_warning (_("Invalid proxy URI '%s'"), http_proxy);
      else
        g_object_set (soup_session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
    }

  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    soup_session_add_feature (soup_session, (SoupSessionFeature *) soup_logger_new (SOUP_LOGGER_LOG_BODY, 500));

  return soup_session;
}

/* Check whether a particular operation should be retried. This is entirely
 * based on how it failed (if at all) last time, and whether the operation has
 * some retries left. The retry count is set when the operation is first
 * created, and must be decremented by the caller. (@n_retries_remaining == 0)
 * will always return %FALSE from this function.
 *
 * This code is copied from libostree's _ostree_fetcher_should_retry_request()
 */
static gboolean
flatpak_http_should_retry_request (const GError *error,
                                   guint         n_retries_remaining)
{
  if (error == NULL || n_retries_remaining == 0)
    return FALSE;

  /* Return TRUE for transient errors. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT) ||
#if !GLIB_CHECK_VERSION(2, 44, 0)
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE) ||
#else
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
#endif
      g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND) ||
      g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE))
    {
      g_debug ("Should retry request (remaining: %u retries), due to transient error: %s",
               n_retries_remaining, error->message);
      return TRUE;
    }

  return FALSE;
}

static GBytes *
flatpak_load_http_uri_once (SoupSession           *soup_session,
                            const char            *uri,
                            FlatpakHTTPFlags       flags,
                            const char            *token,
                            FlatpakLoadUriProgress progress,
                            gpointer               user_data,
                            char                 **out_content_type,
                            GCancellable          *cancellable,
                            GError               **error)
{
  GBytes *bytes = NULL;
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(SoupRequestHTTP) request = NULL;
  g_autoptr(GString) content = g_string_new ("");
  LoadUriData data = { NULL };
  SoupMessage *m;

  g_debug ("Loading %s using libsoup", uri);

  context = g_main_context_ref_thread_default ();

  data.context = context;
  data.content = content;
  data.progress = progress;
  data.cancellable = cancellable;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();
  data.content_type_out = out_content_type;

  request = soup_session_request_http (soup_session, "GET",
                                       uri, error);
  if (request == NULL)
    return NULL;

  m = soup_request_http_get_message (request);

  if (flags & FLATPAK_HTTP_FLAGS_ACCEPT_OCI)
    soup_message_headers_replace (m->request_headers, "Accept",
                                  FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST ", " FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2 ", " FLATPAK_OCI_MEDIA_TYPE_IMAGE_INDEX);


  if (token)
    {
      g_autofree char *bearer_token = g_strdup_printf ("Bearer %s", token);
      soup_message_headers_replace (m->request_headers, "Authorization", bearer_token);
    }

  soup_request_send_async (SOUP_REQUEST (request),
                           cancellable,
                           load_uri_callback, &data);

  while (data.error == NULL && !data.done)
    g_main_context_iteration (data.context, TRUE);

  if (data.error)
    {
      g_propagate_error (error, data.error);
      return NULL;
    }

  bytes = g_string_free_to_bytes (g_steal_pointer (&content));
  g_debug ("Received %" G_GUINT64_FORMAT " bytes", data.downloaded_bytes);

  return bytes;
}

GBytes *
flatpak_load_uri (SoupSession           *soup_session,
                  const char            *uri,
                  FlatpakHTTPFlags       flags,
                  const char            *token,
                  FlatpakLoadUriProgress progress,
                  gpointer               user_data,
                  char                 **out_content_type,
                  GCancellable          *cancellable,
                  GError               **error)
{
  g_autoptr(GError) local_error = NULL;
  guint n_retries_remaining = DEFAULT_N_NETWORK_RETRIES;
  g_autoptr(GMainContextPopDefault) main_context = NULL;

  main_context = flatpak_main_context_new_default ();

  /* Ensure we handle file: uris always */
  if (g_ascii_strncasecmp (uri, "file:", 5) == 0)
    {
      g_autoptr(GFile) file = g_file_new_for_uri (uri);
      gchar *contents;
      gsize len;

      if (!g_file_load_contents (file, cancellable, &contents, &len, NULL, error))
        return NULL;

      return g_bytes_new_take (g_steal_pointer (&contents), len);
    }

  do
    {
      g_autoptr(GBytes) bytes = NULL;

      if (n_retries_remaining < DEFAULT_N_NETWORK_RETRIES)
        {
          g_clear_error (&local_error);

          if (progress)
            progress (0, user_data); /* Reset the progress */
        }

      bytes = flatpak_load_http_uri_once (soup_session, uri, flags,
                                          token, progress, user_data, out_content_type,
                                          cancellable, &local_error);

      if (local_error == NULL)
        return g_steal_pointer (&bytes);
    }
  while (flatpak_http_should_retry_request (local_error, n_retries_remaining--));

  g_assert (local_error != NULL);
  g_propagate_error (error, g_steal_pointer (&local_error));
  return NULL;
}

static gboolean
flatpak_download_http_uri_once (SoupSession           *soup_session,
                                const char            *uri,
                                FlatpakHTTPFlags       flags,
                                GOutputStream         *out,
                                const char            *token,
                                FlatpakLoadUriProgress progress,
                                gpointer               user_data,
                                guint64               *out_bytes_written,
                                GCancellable          *cancellable,
                                GError               **error)
{
  g_autoptr(SoupRequestHTTP) request = NULL;
  g_autoptr(GMainContext) context = NULL;
  LoadUriData data = { NULL };
  SoupMessage *m;

  g_debug ("Loading %s using libsoup", uri);

  context = g_main_context_ref_thread_default ();

  data.context = context;
  data.out = out;
  data.progress = progress;
  data.cancellable = cancellable;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();

  request = soup_session_request_http (soup_session, "GET",
                                       uri, error);
  if (request == NULL)
    return FALSE;

  m = soup_request_http_get_message (request);
  if (flags & FLATPAK_HTTP_FLAGS_ACCEPT_OCI)
    soup_message_headers_replace (m->request_headers, "Accept",
                                  FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST ", " FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2);

  if (token)
    {
      g_autofree char *bearer_token = g_strdup_printf ("Bearer %s", token);
      soup_message_headers_replace (m->request_headers, "Authorization", bearer_token);
    }

  soup_request_send_async (SOUP_REQUEST (request),
                           cancellable,
                           load_uri_callback, &data);

  while (data.error == NULL && !data.done)
    g_main_context_iteration (data.context, TRUE);

  if (out_bytes_written)
    *out_bytes_written = data.downloaded_bytes;

  if (data.error)
    {
      g_propagate_error (error, data.error);
      return FALSE;
    }

  g_debug ("Received %" G_GUINT64_FORMAT " bytes", data.downloaded_bytes);

  return TRUE;
}

gboolean
flatpak_download_http_uri (SoupSession           *soup_session,
                           const char            *uri,
                           FlatpakHTTPFlags       flags,
                           GOutputStream         *out,
                           const char            *token,
                           FlatpakLoadUriProgress progress,
                           gpointer               user_data,
                           GCancellable          *cancellable,
                           GError               **error)
{
  g_autoptr(GError) local_error = NULL;
  guint n_retries_remaining = DEFAULT_N_NETWORK_RETRIES;
  g_autoptr(GMainContextPopDefault) main_context = NULL;

  main_context = flatpak_main_context_new_default ();

  do
    {
      guint64 bytes_written = 0;

      if (n_retries_remaining < DEFAULT_N_NETWORK_RETRIES)
        {
          g_clear_error (&local_error);

          if (progress)
            progress (0, user_data); /* Reset the progress */
        }

      if (flatpak_download_http_uri_once (soup_session, uri, flags,
                                          out, token,
                                          progress, user_data,
                                          &bytes_written,
                                          cancellable, &local_error))
        {
          g_assert (local_error == NULL);
          return TRUE;
        }

      /* If the output stream has already been written to we can't retry.
       * TODO: use a range request to resume the download */
      if (bytes_written > 0)
        break;
    }
  while (flatpak_http_should_retry_request (local_error, n_retries_remaining--));

  g_assert (local_error != NULL);
  g_propagate_error (error, g_steal_pointer (&local_error));
  return FALSE;
}

static gboolean
sync_and_rename_tmpfile (GLnxTmpfile *tmpfile,
                         const char  *dest_name,
                         GError     **error)
{
  /* Filesystem paranoia: If we end up with the new metadata but not
   * the new data, then because the cache headers are in the metadata,
   * we'll never re-download.  (If we just want to avoid losing both
   * the old and new data, skipping fdatasync when the destination is
   * missing works, but it won't here.)
   *
   * This will cause a bunch of fdatasyncs when downloading the icons for
   * a large appstream the first time, would mostly be a problem with a
   * very fast internet connection and a slow spinning drive.
   * Possible solution: update in new directory without fdatasync
   * (copying in any existing cached icons to revalidate), syncfs(), then
   * atomic symlink.
   */
  if (fdatasync (tmpfile->fd) != 0)
    return glnx_throw_errno_prefix (error, "fdatasync");

  if (fchmod (tmpfile->fd, 0644) != 0)
    return glnx_throw_errno_prefix (error, "fchmod");

  if (!glnx_link_tmpfile_at (tmpfile,
                             GLNX_LINK_TMPFILE_REPLACE,
                             tmpfile->src_dfd, dest_name, error))
    return FALSE;

  return TRUE;
}

static gboolean
flatpak_cache_http_uri_once (SoupSession           *soup_session,
                             const char            *uri,
                             FlatpakHTTPFlags       flags,
                             int                    dest_dfd,
                             const char            *dest_subpath,
                             FlatpakLoadUriProgress progress,
                             gpointer               user_data,
                             GCancellable          *cancellable,
                             GError               **error)
{
  g_autoptr(SoupRequestHTTP) request = NULL;
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(CacheHttpData) cache_data = NULL;
  g_autofree char *parent_path = g_path_get_dirname (dest_subpath);
  g_autofree char *name = g_path_get_basename (dest_subpath);
  glnx_autofd int dfd = -1;
  gboolean no_xattr = FALSE;
  LoadUriData data = { NULL };
  g_auto(GLnxTmpfile) out_tmpfile = { 0 };
  g_auto(GLnxTmpfile) cache_tmpfile = { 0 };
  g_autoptr(GBytes) cache_bytes = NULL;
  SoupMessage *m;

  if (!glnx_opendirat (dest_dfd, parent_path, TRUE, &dfd, error))
    return FALSE;

  cache_data = load_cache_http_data (dfd, name, &no_xattr,
                                     cancellable, error);
  if (!cache_data)
    return FALSE;

  if (g_strcmp0 (cache_data->uri, uri) != 0)
    clear_cache_http_data (cache_data, TRUE);

  if (cache_data->uri)
    {
      GTimeVal now;

      g_get_current_time (&now);
      if (cache_data->expires > now.tv_sec)
        {
          if (error)
            *error = g_error_new (FLATPAK_HTTP_ERROR,
                                  FLATPAK_HTTP_ERROR_NOT_CHANGED,
                                  "Reusing cached value");
          return FALSE;
        }
    }

  if (cache_data->uri == NULL)
    cache_data->uri = g_strdup (uri);

  /* Must revalidate */

  g_debug ("Loading %s using libsoup", uri);

  context = g_main_context_ref_thread_default ();

  data.context = context;
  data.cache_data = cache_data;
  data.out_tmpfile = &out_tmpfile;
  data.out_tmpfile_parent_dfd = dfd;
  data.progress = progress;
  data.cancellable = cancellable;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();

  request = soup_session_request_http (soup_session, "GET",
                                       uri, error);
  if (request == NULL)
    return FALSE;

  m = soup_request_http_get_message (request);

  if (cache_data->etag && cache_data->etag[0])
    soup_message_headers_replace (m->request_headers, "If-None-Match", cache_data->etag);
  else if (cache_data->last_modified != 0)
    {
      SoupDate *date = soup_date_new_from_time_t (cache_data->last_modified);
      g_autofree char *date_str = soup_date_to_string (date, SOUP_DATE_HTTP);
      soup_message_headers_replace (m->request_headers, "If-Modified-Since", date_str);
      soup_date_free (date);
    }

  if (flags & FLATPAK_HTTP_FLAGS_ACCEPT_OCI)
    soup_message_headers_replace (m->request_headers, "Accept",
                                  FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST ", " FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2);

  if (flags & FLATPAK_HTTP_FLAGS_STORE_COMPRESSED)
    {
      soup_message_headers_replace (m->request_headers, "Accept-Encoding",
                                    "gzip");
      data.store_compressed = TRUE;
    }

  soup_request_send_async (SOUP_REQUEST (request),
                           cancellable,
                           load_uri_callback, &data);

  while (data.error == NULL && !data.done)
    g_main_context_iteration (data.context, TRUE);

  if (data.error)
    {
      if (data.error->domain == FLATPAK_HTTP_ERROR &&
          data.error->code == FLATPAK_HTTP_ERROR_NOT_CHANGED)
        {
          GError *tmp_error = NULL;

          cache_bytes = serialize_cache_http_data (cache_data);

          if (!save_cache_http_data_to_file (dfd, name, cache_bytes, no_xattr,
                                             cancellable, &tmp_error))
            {
              g_clear_error (&data.error);
              g_propagate_error (error, tmp_error);

              return FALSE;
            }
        }

      g_propagate_error (error, data.error);
      return FALSE;
    }

  cache_bytes = serialize_cache_http_data (cache_data);
  if (!no_xattr)
    {
      if (!save_cache_http_data_xattr (out_tmpfile.fd, cache_bytes, error))
        {
          if (errno != ENOTSUP)
            return FALSE;

          g_clear_error (error);
          no_xattr = TRUE;
        }
    }

  if (no_xattr)
    {
      if (!glnx_open_tmpfile_linkable_at (dfd, ".", O_WRONLY, &cache_tmpfile, error))
        return FALSE;

      if (!save_cache_http_data_fallback (cache_tmpfile.fd, cache_bytes, error))
        return FALSE;
    }

  if (!sync_and_rename_tmpfile (&out_tmpfile, name, error))
    return FALSE;

  if (no_xattr)
    {
      g_autofree char *fallback_name = g_strconcat (name, CACHE_HTTP_SUFFIX, NULL);

      if (!sync_and_rename_tmpfile (&cache_tmpfile, fallback_name, error))
        return FALSE;
    }

  g_debug ("Received %" G_GUINT64_FORMAT " bytes", data.downloaded_bytes);

  return TRUE;
}

gboolean
flatpak_cache_http_uri (SoupSession           *soup_session,
                        const char            *uri,
                        FlatpakHTTPFlags       flags,
                        int                    dest_dfd,
                        const char            *dest_subpath,
                        FlatpakLoadUriProgress progress,
                        gpointer               user_data,
                        GCancellable          *cancellable,
                        GError               **error)
{
  g_autoptr(GError) local_error = NULL;
  guint n_retries_remaining = DEFAULT_N_NETWORK_RETRIES;
  g_autoptr(GMainContextPopDefault) main_context = NULL;

  main_context = flatpak_main_context_new_default ();

  do
    {
      if (n_retries_remaining < DEFAULT_N_NETWORK_RETRIES)
        {
          g_clear_error (&local_error);

          if (progress)
            progress (0, user_data); /* Reset the progress */
        }

      if (flatpak_cache_http_uri_once (soup_session, uri, flags,
                                       dest_dfd, dest_subpath,
                                       progress, user_data,
                                       cancellable, &local_error))
        {
          g_assert (local_error == NULL);
          return TRUE;
        }
    }
  while (flatpak_http_should_retry_request (local_error, n_retries_remaining--));

  g_assert (local_error != NULL);
  g_propagate_error (error, g_steal_pointer (&local_error));
  return FALSE;
}
