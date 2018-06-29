/*
 * Copyright © 2014 Red Hat, Inc
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

#include <libsoup/soup.h>

typedef struct
{
  GMainLoop             *loop;
  GError                *error;
  GOutputStream         *out;
  guint64                downloaded_bytes;
  GString               *content;
  char                   buffer[16 * 1024];
  FlatpakLoadUriProgress progress;
  GCancellable          *cancellable;
  gpointer               user_data;
  guint64                last_progress_time;
  char                  *etag;
} LoadUriData;

static void
stream_closed (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoadUriData *data = user_data;
  GInputStream *stream = G_INPUT_STREAM (source);

  g_autoptr(GError) error = NULL;

  if (!g_input_stream_close_finish (stream, res, &error))
    g_warning ("Error closing http stream: %s", error->message);

  g_main_loop_quit (data->loop);
}

static void
load_uri_read_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoadUriData *data = user_data;
  GInputStream *stream = G_INPUT_STREAM (source);
  gsize nread;

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
      g_main_loop_quit (data->loop);
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
          domain = FLATPAK_OCI_ERROR;
          code = FLATPAK_OCI_ERROR_NOT_CHANGED;
          break;

        case 404:
        case 410:
          code = G_IO_ERROR_NOT_FOUND;
          break;

        default:
          code = G_IO_ERROR_FAILED;
        }

      data->error = g_error_new (domain, code,
                                 "Server returned status %u: %s",
                                 msg->status_code,
                                 soup_status_get_phrase (msg->status_code));
      g_main_loop_quit (data->loop);
      return;
    }

  data->etag = g_strdup (soup_message_headers_get_one (msg->response_headers, "ETag"));

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
        g_warning ("Invalid proxy URI '%s'", http_proxy);
      else
        g_object_set (soup_session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
    }

  return soup_session;
}

GBytes *
flatpak_load_http_uri (SoupSession           *soup_session,
                       const char            *uri,
                       FlatpakHTTPFlags       flags,
                       const char            *etag,
                       char                 **out_etag,
                       FlatpakLoadUriProgress progress,
                       gpointer               user_data,
                       GCancellable          *cancellable,
                       GError               **error)
{
  GBytes *bytes = NULL;

  g_autoptr(GMainContext) context = NULL;
  g_autoptr(SoupRequestHTTP) request = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GString) content = g_string_new ("");
  LoadUriData data = { NULL };
  SoupMessage *m;

  g_debug ("Loading %s using libsoup", uri);

  context = g_main_context_ref_thread_default ();

  loop = g_main_loop_new (context, TRUE);
  data.loop = loop;
  data.content = content;
  data.progress = progress;
  data.cancellable = cancellable;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();

  request = soup_session_request_http (soup_session, "GET",
                                       uri, error);
  if (request == NULL)
    return NULL;

  m = soup_request_http_get_message (request);
  if (etag)
    soup_message_headers_replace (m->request_headers, "If-None-Match", etag);

  if (flags & FLATPAK_HTTP_FLAGS_ACCEPT_OCI)
    soup_message_headers_replace (m->request_headers, "Accept",
                                  "application/vnd.oci.image.manifest.v1+json");

  soup_request_send_async (SOUP_REQUEST (request),
                           cancellable,
                           load_uri_callback, &data);

  g_main_loop_run (loop);

  if (data.error)
    {
      g_propagate_error (error, data.error);
      g_free (data.etag);
      return NULL;
    }

  bytes = g_string_free_to_bytes (g_steal_pointer (&content));
  g_debug ("Received %" G_GUINT64_FORMAT " bytes", data.downloaded_bytes);

  if (out_etag)
    *out_etag = g_steal_pointer (&data.etag);

  g_free (data.etag);

  return bytes;
}

gboolean
flatpak_download_http_uri (SoupSession           *soup_session,
                           const char            *uri,
                           FlatpakHTTPFlags       flags,
                           GOutputStream         *out,
                           FlatpakLoadUriProgress progress,
                           gpointer               user_data,
                           GCancellable          *cancellable,
                           GError               **error)
{
  g_autoptr(SoupRequestHTTP) request = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GMainContext) context = NULL;
  LoadUriData data = { NULL };
  SoupMessage *m;

  g_debug ("Loading %s using libsoup", uri);

  context = g_main_context_ref_thread_default ();

  loop = g_main_loop_new (context, TRUE);
  data.loop = loop;
  data.out = out;
  data.progress = progress;
  data.cancellable = cancellable;
  data.user_data = user_data;
  data.last_progress_time = g_get_monotonic_time ();

  request = soup_session_request_http (soup_session, "GET",
                                       uri, error);
  if (request == NULL)
    return FALSE;

  if (flags & FLATPAK_HTTP_FLAGS_ACCEPT_OCI)
    soup_message_headers_replace (m->request_headers, "Accept",
                                  "application/vnd.oci.image.manifest.v1+json");

  soup_request_send_async (SOUP_REQUEST (request),
                           cancellable,
                           load_uri_callback, &data);

  g_main_loop_run (loop);

  if (data.error)
    {
      g_propagate_error (error, data.error);
      return FALSE;
    }

  g_debug ("Received %" G_GUINT64_FORMAT " bytes", data.downloaded_bytes);

  return TRUE;
}
