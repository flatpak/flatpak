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

#include <glib/gi18n.h>

#include "flatpak-builtins-utils.h"
#include "flatpak-utils.h"


gboolean
looks_like_branch (const char *branch)
{
  const char *dot;

  /* In particular, / is not a valid branch char, so
     this lets us distinguish full or partial refs as
     non-branches. */
  if (!flatpak_is_valid_branch (branch, NULL))
    return FALSE;

  /* Dots are allowed in branches, but not really used much, while
     app ids require at least two, so thats a good check to
     distinguish the two */
  dot = strchr (branch, '.');
  if (dot != NULL)
    {
      if (strchr (dot + 1, '.') != NULL)
        return FALSE;
    }

  return TRUE;
}

SoupSession *
get_soup_session (void)
{
  static SoupSession *soup_session = NULL;

  if (soup_session == NULL)
    {
      const char *http_proxy;

      soup_session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT, "flatpak-builder ",
                                                    SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                    SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                    SOUP_SESSION_TIMEOUT, 60,
                                                    SOUP_SESSION_IDLE_TIMEOUT, 60,
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
    }

  return soup_session;
}

GBytes *
download_uri (const char     *url,
              GError        **error)
{
  SoupSession *session;
  g_autoptr(SoupRequest) req = NULL;
  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GOutputStream) out = NULL;

  session = get_soup_session ();

  req = soup_session_request (session, url, error);
  if (req == NULL)
    return NULL;

  input = soup_request_send (req, NULL, error);
  if (input == NULL)
    return NULL;

  out = g_memory_output_stream_new_resizable ();
  if (!g_output_stream_splice (out,
                               input,
                               G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                               NULL,
                               error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
}
