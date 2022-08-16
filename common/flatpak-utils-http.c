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

#include "config.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include "flatpak-utils-http-private.h"
#include "flatpak-uri-private.h"
#include "flatpak-oci-registry-private.h"

#include <gio/gunixoutputstream.h>
#include "libglnx.h"

#include <sys/types.h>
#include <sys/xattr.h>

#if defined(HAVE_CURL)

#include <curl/curl.h>

/* These macros came from 7.43.0, but we want to check
 * for versions a bit earlier than that (to work on CentOS 7),
 * so define them here if we're using an older version.
 */
#ifndef CURL_VERSION_BITS
#define CURL_VERSION_BITS(x,y,z) ((x)<<16|(y)<<8|z)
#endif
#ifndef CURL_AT_LEAST_VERSION
#define CURL_AT_LEAST_VERSION(x,y,z) (LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(x, y, z))
#endif

#elif defined(HAVE_SOUP)

#include <libsoup/soup.h>

#if !defined(SOUP_AUTOCLEANUPS_H) && !defined(__SOUP_AUTOCLEANUPS_H__)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupSession, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupRequest, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupRequestHTTP, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupURI, soup_uri_free)
#endif

#else

# error "No HTTP backend enabled"

#endif


#define FLATPAK_HTTP_TIMEOUT_SECS 60

/* copied from libostree */
#define DEFAULT_N_NETWORK_RETRIES 5

G_DEFINE_QUARK (flatpak_http_error, flatpak_http_error)

/* Information about the cache status of a file.
   Encoded in an xattr on the cached file, or a file on the side if xattrs don't work.
*/
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

  /* Input args */

  FlatpakHTTPFlags       flags;
  const char            *auth;
  const char            *token;
  FlatpakLoadUriProgress progress;
  GCancellable          *cancellable;
  gpointer               user_data;
  CacheHttpData         *cache_data;

  /* Output from the request, set even on http server errors */

  guint64               downloaded_bytes;
  int                   status;
  char                  *hdr_content_type;
  char                  *hdr_www_authenticate;
  char                  *hdr_etag;
  char                  *hdr_last_modified;
  char                  *hdr_cache_control;
  char                  *hdr_expires;
  char                  *hdr_content_encoding;

  /* Data destination */

  GOutputStream         *out; /*or */
  GString               *content; /* or */
  GLnxTmpfile           *out_tmpfile;
  int                    out_tmpfile_parent_dfd;

  /* Used during operation */

  char                   buffer[16 * 1024];
  guint64                last_progress_time;
  gboolean               store_compressed;

} LoadUriData;

static void
clear_load_uri_data_headers (LoadUriData *data)
{
  g_clear_pointer (&data->hdr_content_type, g_free);
  g_clear_pointer (&data->hdr_www_authenticate, g_free);
  g_clear_pointer (&data->hdr_etag, g_free);
  g_clear_pointer (&data->hdr_last_modified, g_free);
  g_clear_pointer (&data->hdr_last_modified, g_free);
  g_clear_pointer (&data->hdr_cache_control, g_free);
  g_clear_pointer (&data->hdr_expires, g_free);
  g_clear_pointer (&data->hdr_content_encoding, g_free);
}

/* Reset between requests retries */
static void
reset_load_uri_data (LoadUriData *data)
{
  g_clear_error (&data->error);
  data->status = 0;
  data->downloaded_bytes = 0;
  if (data->content)
    g_string_set_size (data->content, 0);

  clear_load_uri_data_headers (data);

  if (data->out_tmpfile)
    {
      glnx_tmpfile_clear (data->out_tmpfile);
      g_clear_pointer (&data->out, g_object_unref);
    }

  /* Reset the progress */
  if (data->progress)
    data->progress (0, data->user_data);
}

/* Free allocated data at end of full repeated download */
static void
clear_load_uri_data (LoadUriData *data)
{
  if (data->content)
    {
      g_string_free (data->content, TRUE);
      data->content = NULL;
    }

  g_clear_error (&data->error);

  clear_load_uri_data_headers (data);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(LoadUriData, clear_load_uri_data)

static gboolean
check_http_status (guint status_code,
                   GError **error)
{
  GQuark domain;
  int code;

  if (status_code >= 200 && status_code < 300)
    return TRUE;

  switch (status_code)
    {
    case 304: /* Not Modified */
      domain = FLATPAK_HTTP_ERROR;
      code = FLATPAK_HTTP_ERROR_NOT_CHANGED;
      break;

    case 401: /* Unauthorized */
      domain = FLATPAK_HTTP_ERROR;
      code = FLATPAK_HTTP_ERROR_UNAUTHORIZED;
      break;

    case 403: /* Forbidden */
    case 404: /* Not found */
    case 410: /* Gone */
      domain = G_IO_ERROR;
      code = G_IO_ERROR_NOT_FOUND;
      break;

    case 408: /* Request Timeout */
      domain = G_IO_ERROR;
      code = G_IO_ERROR_TIMED_OUT;
      break;

    case 500: /* Internal Server Error */
      /* The server did return something, but it was useless to us, so that’s basically equivalent to not returning */
      domain = G_IO_ERROR;
      code = G_IO_ERROR_HOST_UNREACHABLE;
      break;

    default:
      domain = G_IO_ERROR;
      code = G_IO_ERROR_FAILED;
    }

  g_set_error (error, domain, code,
               "Server returned status %u",
               status_code);
  return FALSE;
}

#if defined(HAVE_CURL)

/************************************************************************
 *                        Curl implementation                           *
 ************************************************************************/

typedef struct curl_slist auto_curl_slist;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (auto_curl_slist, curl_slist_free_all)

struct FlatpakHttpSession {
  CURL *curl;
  GMutex lock;
};

static void
check_header(char **value_out,
             const char *header,
             char *buffer,
             size_t realsize)
{
  size_t hlen = strlen (header);

  if (realsize < hlen + 1)
    return;

  if (!g_ascii_strncasecmp(buffer, header, hlen) == 0 ||
      buffer[hlen] != ':')
    return;

  buffer += hlen + 1;
  realsize -= hlen + 1;

  while (realsize > 0 && g_ascii_isspace (*buffer))
    {
      buffer++;
      realsize--;
    }

  while (realsize > 0 && g_ascii_isspace (buffer[realsize-1]))
    realsize--;

  g_free (*value_out); /* Use the last header */
  *value_out = g_strndup (buffer, realsize);
}

static size_t
_header_cb (char *buffer,
            size_t size,
            size_t nitems,
            void *userdata)
{
  size_t realsize = size * nitems;
  LoadUriData *data = (LoadUriData *)userdata;

  check_header(&data->hdr_content_type, "content-type", buffer, realsize);
  check_header(&data->hdr_www_authenticate, "WWW-Authenticate", buffer, realsize);

  check_header(&data->hdr_etag, "ETag", buffer, realsize);
  check_header(&data->hdr_last_modified, "Last-Modified", buffer, realsize);
  check_header(&data->hdr_cache_control, "Cache-Control", buffer, realsize);
  check_header(&data->hdr_expires, "Expires", buffer, realsize);
  check_header(&data->hdr_content_encoding, "Content-Encoding", buffer, realsize);

  return realsize;
}

static size_t
_write_cb (void *content_data,
           size_t size,
           size_t nmemb,
           void *userp)
{
  size_t realsize = size * nmemb;
  LoadUriData *data = (LoadUriData *)userp;
  gsize n_written = 0;

  /* If first write to tmpfile, initiate if needed */
  if (data->content == NULL && data->out == NULL &&
      data->out_tmpfile != NULL)
    {
      g_autoptr(GOutputStream) out = NULL;
      g_autoptr(GError) tmp_error = NULL;

      if (!glnx_open_tmpfile_linkable_at (data->out_tmpfile_parent_dfd, ".",
                                          O_WRONLY, data->out_tmpfile,
                                          &tmp_error))
        {
          g_warning ("Failed to open http tmpfile: %s\n", tmp_error->message);
          return 0; /* This short read will make curl report an error */
        }

      out = g_unix_output_stream_new (data->out_tmpfile->fd, FALSE);
      if (data->store_compressed &&
          g_strcmp0 (data->hdr_content_encoding, "gzip") != 0)
        {
          g_autoptr(GZlibCompressor) compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
          data->out = g_converter_output_stream_new (out, G_CONVERTER (compressor));
        }
      else
        {
          data->out = g_steal_pointer (&out);
        }
    }

  if (data->content)
    {
      g_string_append_len (data->content, content_data, realsize);
      n_written = realsize;
    }
  else if (data->out)
    {
      /* We ignore the error here, but reporting a short read will make curl report the error */
      g_output_stream_write_all (data->out, content_data, realsize,
                                 &n_written, NULL, NULL);
    }

  data->downloaded_bytes += realsize;

  if (g_get_monotonic_time () - data->last_progress_time > 1 * G_USEC_PER_SEC)
    {
      if (data->progress)
        data->progress (data->downloaded_bytes, data->user_data);
      data->last_progress_time = g_get_monotonic_time ();
    }

  return realsize;
}

FlatpakHttpSession *
flatpak_create_http_session (const char *user_agent)
{
  FlatpakHttpSession *session = g_new0 (FlatpakHttpSession, 1);
  CURLcode rc;
  CURL *curl;

  session->curl = curl = curl_easy_init();
  g_assert (session->curl != NULL);

  g_mutex_init (&session->lock);

  curl_easy_setopt (curl, CURLOPT_USERAGENT, user_agent);
  rc = curl_easy_setopt (curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
  g_assert_cmpint (rc, ==, CURLM_OK);

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  /* Note: curl automatically respects the http_proxy env var */

  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    curl_easy_setopt (curl, CURLOPT_VERBOSE, 1L);

  /* Picked the current version in F25 as of 20170127, since
   * there are numerous HTTP/2 fixes since the original version in
   * libcurl 7.43.0.
   */
#if CURL_AT_LEAST_VERSION(7, 51, 0)
  rc = curl_easy_setopt (curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
  g_assert_cmpint (rc, ==, CURLM_OK);
#endif
  /* https://github.com/curl/curl/blob/curl-7_53_0/docs/examples/http2-download.c */
#if (CURLPIPE_MULTIPLEX > 0)
  /* wait for pipe connection to confirm */
  rc = curl_easy_setopt (curl, CURLOPT_PIPEWAIT, 1L);
  g_assert_cmpint (rc, ==, CURLM_OK);
#endif

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, _header_cb);

  curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, (long)FLATPAK_HTTP_TIMEOUT_SECS);

  return session;
}

void
flatpak_http_session_free (FlatpakHttpSession* session)
{
  g_mutex_lock (&session->lock);
  curl_easy_cleanup (session->curl);
  g_mutex_unlock (&session->lock);
  g_mutex_clear (&session->lock);
  g_free (session);
}

static void
set_error_from_curl (GError **error,
                     const char *uri,
                     CURLcode res)
{
  GQuark domain = G_IO_ERROR;
  int code;

  switch (res)
    {
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
      code = G_IO_ERROR_HOST_NOT_FOUND;
      break;
    case CURLE_OPERATION_TIMEDOUT:
      code = G_IO_ERROR_TIMED_OUT;
      break;
    default:
      code =  G_IO_ERROR_FAILED;
    }

  g_set_error (error, domain, code,
               "While fetching %s: [%u] %s", uri, res,
               curl_easy_strerror (res));
}

static gboolean
flatpak_download_http_uri_once (FlatpakHttpSession    *session,
                                LoadUriData           *data,
                                const char            *uri,
                                GError               **error)
{
  CURLcode res;
  g_autofree char *auth_header = NULL;
  g_autofree char *cache_header = NULL;
  g_autoptr(auto_curl_slist) header_list = NULL;
  g_autoptr(GMutexLocker) curl_lock = g_mutex_locker_new (&session->lock);
  long response;
  CURL *curl = session->curl;

  g_debug ("Loading %s using curl", uri);

  curl_easy_setopt (curl, CURLOPT_URL, uri);
  curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *)data);
  curl_easy_setopt (curl, CURLOPT_HEADERDATA, (void *)data);

  if (data->flags & FLATPAK_HTTP_FLAGS_HEAD)
    curl_easy_setopt (curl, CURLOPT_NOBODY, 1L);
  else
    curl_easy_setopt (curl, CURLOPT_HTTPGET, 1L);

  if (data->flags & FLATPAK_HTTP_FLAGS_ACCEPT_OCI)
    header_list = curl_slist_append (header_list,
                                     "Accept: " FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST ", " FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2 ", " FLATPAK_OCI_MEDIA_TYPE_IMAGE_INDEX);

  if (data->auth)
    auth_header = g_strdup_printf ("Authorization: Basic %s", data->auth);
  else if (data->token)
    auth_header = g_strdup_printf ("Authorization: Bearer %s", data->token);
  if (auth_header)
    header_list = curl_slist_append (header_list, auth_header);

  if (data->cache_data)
    {
      CacheHttpData *cache_data = data->cache_data;

      if (cache_data->etag && cache_data->etag[0])
        cache_header = g_strdup_printf ("If-None-Match: %s", cache_data->etag);
      else if (cache_data->last_modified != 0)
        {
          g_autoptr(GDateTime) date = g_date_time_new_from_unix_utc (cache_data->last_modified);
          g_autofree char *date_str = flatpak_format_http_date (date);
          cache_header = g_strdup_printf ("If-Modified-Since: %s", date_str);
        }
      if (cache_header)
        header_list = curl_slist_append (header_list, cache_header);
    }

  curl_easy_setopt (curl, CURLOPT_HTTPHEADER, header_list);

  if (data->flags & FLATPAK_HTTP_FLAGS_STORE_COMPRESSED)
    {
      curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
      curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
      data->store_compressed = TRUE;
    }
  else
    {
      /* enable all supported built-in compressions */
      curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
      curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 1L);
      data->store_compressed = FALSE;
    }

  res = curl_easy_perform (session->curl);

  curl_easy_setopt (session->curl, CURLOPT_HTTPHEADER, NULL); /* Don't point to freed list */

  if (res != CURLE_OK)
    {
      set_error_from_curl (error, uri, res);

      /* Make sure we clear the tmpfile stream we possible created during the request */
      if (data->out_tmpfile && data->out)
        g_clear_pointer (&data->out, g_object_unref);

      return FALSE;
    }

  if (data->out_tmpfile && data->out)
    {
      /* Flush the writes */
      if (!g_output_stream_close (data->out, data->cancellable, error))
        return FALSE;

      g_clear_pointer (&data->out, g_object_unref);
    }

  if (data->progress)
    data->progress (data->downloaded_bytes, data->user_data);

  curl_easy_getinfo (session->curl, CURLINFO_RESPONSE_CODE, &response);

  data->status = response;

  if ((data->flags & FLATPAK_HTTP_FLAGS_NOCHECK_STATUS) == 0 &&
      !check_http_status (data->status, error))
    return FALSE;

  g_debug ("Received %" G_GUINT64_FORMAT " bytes", data->downloaded_bytes);

  /* This is not really needed, but the auto-pointer confuses some compilers in the CI */
  g_clear_pointer (&curl_lock, g_mutex_locker_free);

  return TRUE;
}

#endif  /* HAVE_CURL */

#if defined(HAVE_SOUP)

/************************************************************************
 *                        Soup implementation                           *
 ***********************************************************************/

static gboolean
check_soup_transfer_error (SoupMessage *msg, GError **error)
{
  GQuark domain = G_IO_ERROR;
  int code;

  if (!SOUP_STATUS_IS_TRANSPORT_ERROR (msg->status_code))
    return TRUE;

  switch (msg->status_code)
    {
    case SOUP_STATUS_CANCELLED:
      code = G_IO_ERROR_CANCELLED;
      break;

    case SOUP_STATUS_CANT_RESOLVE:
    case SOUP_STATUS_CANT_CONNECT:
      code = G_IO_ERROR_HOST_NOT_FOUND;
      break;

    case SOUP_STATUS_IO_ERROR:
      code = G_IO_ERROR_CONNECTION_CLOSED;
      break;

    default:
      code = G_IO_ERROR_FAILED;
    }

  g_set_error (error, domain, code,
               "Error connecting to server: %s",
               soup_status_get_phrase (msg->status_code));
  return FALSE;
}

/* The soup input stream was closed */
static void
stream_closed (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoadUriData *data = user_data;
  GInputStream *stream = G_INPUT_STREAM (source);
  g_autoptr(GError) error = NULL;

  if (!g_input_stream_close_finish (stream, res, &error))
    g_warning ("Error closing http stream: %s", error->message);

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

/* Got some data from the soup input stream */
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
      g_assert (data->content != NULL);
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

/* The http header part of the request is ready */
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
      g_main_context_wakeup (data->context);
      return;
    }

  g_autoptr(SoupMessage) msg = soup_request_http_get_message ((SoupRequestHTTP *) request);

  if (!check_soup_transfer_error (msg, &data->error))
    {
      g_main_context_wakeup (data->context);
      return;
    }

  /* We correctly made a connection, although it may be a http failure like 404.
     The status and headers are valid on return, even of a http failure though. */

  data->status = msg->status_code;
  data->hdr_content_type = g_strdup (soup_message_headers_get_content_type (msg->response_headers, NULL));
  data->hdr_www_authenticate = g_strdup (soup_message_headers_get_one (msg->response_headers, "WWW-Authenticate"));
  data->hdr_etag = g_strdup (soup_message_headers_get_one (msg->response_headers, "ETag"));
  data->hdr_last_modified = g_strdup (soup_message_headers_get_one (msg->response_headers, "Last-Modified"));
  data->hdr_cache_control = g_strdup (soup_message_headers_get_list (msg->response_headers, "Cache-Control"));
  data->hdr_expires = g_strdup (soup_message_headers_get_list (msg->response_headers, "Expires"));

  if ((data->flags & FLATPAK_HTTP_FLAGS_NOCHECK_STATUS) == 0 &&
      !check_http_status (data->status, &data->error))
    {
      g_main_context_wakeup (data->context);
      return;
    }

  /* All is good, write the body to the destination */

  if (data->out_tmpfile)
    {
      g_autoptr(GOutputStream) out = NULL;

      if (!glnx_open_tmpfile_linkable_at (data->out_tmpfile_parent_dfd, ".",
                                          O_WRONLY, data->out_tmpfile,
                                          &data->error))
        {
          g_main_context_wakeup (data->context);
          return;
        }

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

static SoupSession *
flatpak_create_soup_session (const char *user_agent)
{
  SoupSession *soup_session;
  const char *http_proxy;

  soup_session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT, user_agent,
                                                SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                SOUP_SESSION_TIMEOUT, FLATPAK_HTTP_TIMEOUT_SECS,
                                                SOUP_SESSION_IDLE_TIMEOUT, FLATPAK_HTTP_TIMEOUT_SECS,
                                                NULL);
  http_proxy = g_getenv ("http_proxy");
  if (http_proxy)
    {
      g_autoptr(SoupURI) proxy_uri = soup_uri_new (http_proxy);
      if (!proxy_uri)
        g_warning ("Invalid proxy URI '%s'", http_proxy);
      else
        g_object_set (soup_session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
    }

  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    soup_session_add_feature (soup_session, (SoupSessionFeature *) soup_logger_new (SOUP_LOGGER_LOG_BODY, 500));

  return soup_session;
}

FlatpakHttpSession *
flatpak_create_http_session (const char *user_agent)
{
  return (FlatpakHttpSession *)flatpak_create_soup_session (user_agent);
}

void
flatpak_http_session_free (FlatpakHttpSession* http_session)
{
  SoupSession *soup_session = (SoupSession *)http_session;

  g_object_unref (soup_session);
}

static gboolean
flatpak_download_http_uri_once (FlatpakHttpSession    *http_session,
                                LoadUriData           *data,
                                const char            *uri,
                                GError               **error)
{
  SoupSession *soup_session = (SoupSession *)http_session;
  g_autoptr(SoupRequestHTTP) request = NULL;
  SoupMessage *m;

  g_debug ("Loading %s using libsoup", uri);

  request = soup_session_request_http (soup_session,
                                       (data->flags & FLATPAK_HTTP_FLAGS_HEAD) != 0 ? "HEAD" : "GET",
                                       uri, error);
  if (request == NULL)
    return FALSE;

  m = soup_request_http_get_message (request);

  if (data->flags & FLATPAK_HTTP_FLAGS_ACCEPT_OCI)
    soup_message_headers_replace (m->request_headers, "Accept",
                                  FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST ", " FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2 ", " FLATPAK_OCI_MEDIA_TYPE_IMAGE_INDEX);

  if (data->auth)
    {
      g_autofree char *basic_auth = g_strdup_printf ("Basic %s", data->auth);
      soup_message_headers_replace (m->request_headers, "Authorization", basic_auth);
    }

  if (data->token)
    {
      g_autofree char *bearer_token = g_strdup_printf ("Bearer %s", data->token);
      soup_message_headers_replace (m->request_headers, "Authorization", bearer_token);
    }

  if (data->cache_data)
    {
      CacheHttpData *cache_data = data->cache_data;

      if (cache_data->etag && cache_data->etag[0])
        soup_message_headers_replace (m->request_headers, "If-None-Match", cache_data->etag);
      else if (cache_data->last_modified != 0)
        {
          g_autoptr(GDateTime) date = g_date_time_new_from_unix_utc (cache_data->last_modified);
          g_autofree char *date_str = flatpak_format_http_date (date);
          soup_message_headers_replace (m->request_headers, "If-Modified-Since", date_str);
        }
    }

  if (data->flags & FLATPAK_HTTP_FLAGS_STORE_COMPRESSED)
    {
      soup_session_remove_feature_by_type (soup_session, SOUP_TYPE_CONTENT_DECODER);
      soup_message_headers_replace (m->request_headers, "Accept-Encoding", "gzip");
      data->store_compressed = TRUE;
    }
  else if (!soup_session_has_feature (soup_session, SOUP_TYPE_CONTENT_DECODER))
    {
      soup_session_add_feature_by_type (soup_session, SOUP_TYPE_CONTENT_DECODER);
      data->store_compressed = FALSE;
    }

  soup_request_send_async (SOUP_REQUEST (request),
                           data->cancellable,
                           load_uri_callback, data);

  while (data->error == NULL && !data->done)
    g_main_context_iteration (data->context, TRUE);

  if (data->error)
    {
      g_propagate_error (error, g_steal_pointer (&data->error));
      return FALSE;
    }

  g_debug ("Received %" G_GUINT64_FORMAT " bytes", data->downloaded_bytes);

  return TRUE;
}

#endif /* HAVE_SOUP */

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
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
      g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND) ||
      g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE))
    {
      g_debug ("Should retry request (remaining: %u retries), due to transient error: %s",
               n_retries_remaining, error->message);
      return TRUE;
    }

  return FALSE;
}

GBytes *
flatpak_load_uri_full (FlatpakHttpSession    *http_session,
                       const char            *uri,
                       FlatpakHTTPFlags       flags,
                       const char            *auth,
                       const char            *token,
                       FlatpakLoadUriProgress progress,
                       gpointer               user_data,
                       int                   *out_status,
                       char                 **out_content_type,
                       char                 **out_www_authenticate,
                       GCancellable          *cancellable,
                       GError               **error)
{
  g_auto(LoadUriData) data = { NULL };
  g_autoptr(GError) local_error = NULL;
  guint n_retries_remaining = DEFAULT_N_NETWORK_RETRIES;
  g_autoptr(GMainContextPopDefault) main_context = NULL;
  gboolean success = FALSE;

  /* Ensure we handle file: uris the same independent of backend */
  if (g_ascii_strncasecmp (uri, "file:", 5) == 0)
    {
      g_autoptr(GFile) file = g_file_new_for_uri (uri);
      gchar *contents;
      gsize len;

      if (!g_file_load_contents (file, cancellable, &contents, &len, NULL, error))
        return NULL;

      return g_bytes_new_take (g_steal_pointer (&contents), len);
    }

  main_context = flatpak_main_context_new_default ();

  data.context = main_context;
  data.progress = progress;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();
  data.cancellable = cancellable;
  data.flags = flags;
  data.auth = auth;
  data.token = token;

  data.content = g_string_new ("");

  do
    {
      if (n_retries_remaining < DEFAULT_N_NETWORK_RETRIES)
        {
          g_clear_error (&local_error);
          reset_load_uri_data (&data);
        }

      success = flatpak_download_http_uri_once (http_session, &data, uri, &local_error);

      if (success)
        break;

      g_assert (local_error != NULL);
    }
  while (flatpak_http_should_retry_request (local_error, n_retries_remaining--));

  if (success)
    {
      if (out_content_type)
        *out_content_type = g_steal_pointer (&data.hdr_content_type);

      if (out_www_authenticate)
        *out_www_authenticate = g_steal_pointer (&data.hdr_www_authenticate);

      if (out_status)
        *out_status = data.status;

      return g_string_free_to_bytes (g_steal_pointer (&data.content));
    }

  g_assert (local_error != NULL);
  g_propagate_error (error, g_steal_pointer (&local_error));
  return NULL;
}


GBytes *
flatpak_load_uri (FlatpakHttpSession    *http_session,
                  const char            *uri,
                  FlatpakHTTPFlags       flags,
                  const char            *token,
                  FlatpakLoadUriProgress progress,
                  gpointer               user_data,
                  char                 **out_content_type,
                  GCancellable          *cancellable,
                  GError               **error)
{
  return flatpak_load_uri_full (http_session, uri, flags, NULL, token,
                                progress, user_data, NULL, out_content_type, NULL,
                                cancellable, error);
}

gboolean
flatpak_download_http_uri (FlatpakHttpSession    *http_session,
                           const char            *uri,
                           FlatpakHTTPFlags       flags,
                           GOutputStream         *out,
                           const char            *token,
                           FlatpakLoadUriProgress progress,
                           gpointer               user_data,
                           GCancellable          *cancellable,
                           GError               **error)
{
  g_auto(LoadUriData) data = { NULL };
  g_autoptr(GError) local_error = NULL;
  guint n_retries_remaining = DEFAULT_N_NETWORK_RETRIES;
  g_autoptr(GMainContextPopDefault) main_context = NULL;
  gboolean success = FALSE;

  main_context = flatpak_main_context_new_default ();

  data.context = main_context;
  data.progress = progress;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();
  data.cancellable = cancellable;
  data.flags = flags;
  data.token = token;

  data.out = out;

  do
    {
      if (n_retries_remaining < DEFAULT_N_NETWORK_RETRIES)
        {
          g_clear_error (&local_error);
          reset_load_uri_data (&data);
        }

      success =  flatpak_download_http_uri_once (http_session, &data, uri, &local_error);

      if (success)
        break;

      g_assert (local_error != NULL);

      /* If the output stream has already been written to we can't retry.
       * TODO: use a range request to resume the download */
      if (data.downloaded_bytes > 0)
        break;
    }
  while (flatpak_http_should_retry_request (local_error, n_retries_remaining--));

  if (success)
    return TRUE;

  g_assert (local_error != NULL);
  g_propagate_error (error, g_steal_pointer (&local_error));
  return FALSE;
}

/************************************************************************
 *                        Cached http support                           *
 ***********************************************************************/

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

static void
set_cache_http_data_from_headers (CacheHttpData *cache_data,
                                  LoadUriData *data)
{
  const char *etag = data->hdr_etag;
  const char *last_modified = data->hdr_last_modified;
  const char *cache_control = data->hdr_cache_control;
  const char *expires = data->hdr_expires;
  gboolean expires_computed = FALSE;

  /* The original HTTP 1/1 specification only required sending the ETag header in a 304
   * response, and implied that a cache might need to save the old Cache-Control
   * values. The updated RFC 7232 from 2014 requires sending Cache-Control, ETags, and
   * Expire if they would have been sent in the original 200 response, and recommends
   * sending Last-Modified for requests without an etag. Since sending these headers was
   * apparently normal previously, for simplicity we assume the RFC 7232 behavior and start
   * from scratch for a 304 response.
   */
  clear_cache_http_data (cache_data, FALSE);

  if (etag && *etag)
    {
      cache_data->etag = g_strdup (etag);
    }
  else if (last_modified && *last_modified)
    {
      g_autoptr(GDateTime) date = flatpak_parse_http_time (last_modified);
      if (date)
        cache_data->last_modified = g_date_time_to_unix (date);
    }

  if (cache_control && *cache_control)
    {
      g_autoptr(GHashTable) params = flatpak_parse_http_header_param_list (cache_control);
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
                  cache_data->expires = now.tv_sec + max_age_sec;
                  expires_computed = TRUE;
                }
            }
          else if (g_strcmp0 (key, "no-cache") == 0)
            {
              cache_data->expires = 0;
              expires_computed = TRUE;
            }
        }
    }

  if (!expires_computed && expires && *expires)
    {
      g_autoptr(GDateTime) date = flatpak_parse_http_time (expires);
      if (date)
        {
          cache_data->expires = g_date_time_to_unix (date);
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
      cache_data->expires = now.tv_sec + 1800;
    }
}

gboolean
flatpak_cache_http_uri (FlatpakHttpSession    *http_session,
                        const char            *uri,
                        FlatpakHTTPFlags       flags,
                        int                    dest_dfd,
                        const char            *dest_subpath,
                        FlatpakLoadUriProgress progress,
                        gpointer               user_data,
                        GCancellable          *cancellable,
                        GError               **error)
{
  g_auto(LoadUriData) data = { NULL };
  g_autoptr(GError) local_error = NULL;
  guint n_retries_remaining = DEFAULT_N_NETWORK_RETRIES;
  g_autoptr(CacheHttpData) cache_data = NULL;
  g_autoptr(GMainContextPopDefault) main_context = NULL;
  g_autofree char *parent_path = g_path_get_dirname (dest_subpath);
  g_autofree char *name = g_path_get_basename (dest_subpath);
  g_auto(GLnxTmpfile) out_tmpfile = { 0 };
  g_auto(GLnxTmpfile) cache_tmpfile = { 0 };
  g_autoptr(GBytes) cache_bytes = NULL;
  gboolean no_xattr = FALSE;
  glnx_autofd int cache_dfd = -1;
  gboolean success;

  if (!glnx_opendirat (dest_dfd, parent_path, TRUE, &cache_dfd, error))
    return FALSE;

  cache_data = load_cache_http_data (cache_dfd, name, &no_xattr,
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
          g_set_error (error, FLATPAK_HTTP_ERROR,
                       FLATPAK_HTTP_ERROR_NOT_CHANGED,
                       "Reusing cached value");
          return FALSE;
        }
    }

  if (cache_data->uri == NULL)
    cache_data->uri = g_strdup (uri);

  /* Missing from cache, or expired so must revalidate via etag/last-modified headers */

  main_context = flatpak_main_context_new_default ();

  data.context = main_context;
  data.progress = progress;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();
  data.cancellable = cancellable;
  data.flags = flags;

  data.cache_data = cache_data;

  data.out_tmpfile = &out_tmpfile;
  data.out_tmpfile_parent_dfd = cache_dfd;

  do
    {
      if (n_retries_remaining < DEFAULT_N_NETWORK_RETRIES)
        {
          g_clear_error (&local_error);
          reset_load_uri_data (&data);
        }

      success = flatpak_download_http_uri_once (http_session, &data, uri, &local_error);

      if (success)
        break;

      g_assert (local_error != NULL);
    }
  while (flatpak_http_should_retry_request (local_error, n_retries_remaining--));

  /* Update the cache data on success or cache-valid */
  if (success || g_error_matches (local_error, FLATPAK_HTTP_ERROR, FLATPAK_HTTP_ERROR_NOT_CHANGED))
    {
      set_cache_http_data_from_headers (cache_data, &data);
      cache_bytes = serialize_cache_http_data (cache_data);
    }

  if (local_error)
    {
      if (cache_bytes)
        {
          GError *tmp_error = NULL;

          if (!save_cache_http_data_to_file (cache_dfd, name, cache_bytes, no_xattr,
                                             cancellable, &tmp_error))
            {
              g_clear_error (&local_error);
              g_propagate_error (error, tmp_error);

              return FALSE;
            }
        }

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

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
      if (!glnx_open_tmpfile_linkable_at (cache_dfd, ".", O_WRONLY, &cache_tmpfile, error))
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

  return TRUE;
}
