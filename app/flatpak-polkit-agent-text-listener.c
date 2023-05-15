/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <pwd.h>

#include <termios.h>
#include <unistd.h>

#include "flatpak-polkit-agent-text-listener.h"

#include "flatpak-tty-utils-private.h"
#include "flatpak-utils-private.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

struct _FlatpakPolkitAgentTextListener
{
  PolkitAgentListener parent_instance;

  GSimpleAsyncResult *simple;
  PolkitAgentSession *active_session;
  gulong cancel_id;
  GCancellable *cancellable;

  FILE *tty;
};

typedef struct
{
  PolkitAgentListenerClass parent_class;
} FlatpakPolkitAgentTextListenerClass;

static void flatpak_polkit_agent_text_listener_initiate_authentication (PolkitAgentListener  *_listener,
                                                                const gchar          *action_id,
                                                                const gchar          *message,
                                                                const gchar          *icon_name,
                                                                PolkitDetails        *details,
                                                                const gchar          *cookie,
                                                                GList                *identities,
                                                                GCancellable         *cancellable,
                                                                GAsyncReadyCallback   callback,
                                                                gpointer              user_data);

static gboolean flatpak_polkit_agent_text_listener_initiate_authentication_finish (PolkitAgentListener  *_listener,
                                                                           GAsyncResult         *res,
                                                                           GError              **error);

static void initable_iface_init (GInitableIface *initable_iface);

G_DEFINE_TYPE_WITH_CODE (FlatpakPolkitAgentTextListener, flatpak_polkit_agent_text_listener, POLKIT_AGENT_TYPE_LISTENER,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init));

static void
flatpak_polkit_agent_text_listener_init (FlatpakPolkitAgentTextListener *listener)
{
}

static void
flatpak_polkit_agent_text_listener_finalize (GObject *object)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (object);

  if (listener->tty != NULL)
    fclose (listener->tty);

  if (listener->active_session != NULL)
    g_object_unref (listener->active_session);

  if (G_OBJECT_CLASS (flatpak_polkit_agent_text_listener_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (flatpak_polkit_agent_text_listener_parent_class)->finalize (object);
}

static void
flatpak_polkit_agent_text_listener_class_init (FlatpakPolkitAgentTextListenerClass *klass)
{
  GObjectClass *gobject_class;
  PolkitAgentListenerClass *listener_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = flatpak_polkit_agent_text_listener_finalize;

  listener_class = POLKIT_AGENT_LISTENER_CLASS (klass);
  listener_class->initiate_authentication = flatpak_polkit_agent_text_listener_initiate_authentication;
  listener_class->initiate_authentication_finish = flatpak_polkit_agent_text_listener_initiate_authentication_finish;
}

PolkitAgentListener *
flatpak_polkit_agent_text_listener_new (GCancellable  *cancellable,
                                        GError       **error)
{
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  return POLKIT_AGENT_LISTENER (g_initable_new (FLATPAK_POLKIT_AGENT_TYPE_TEXT_LISTENER,
                                                cancellable,
                                                error,
                                                NULL));
}

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (initable);
  gboolean ret;
  const gchar *tty_name;

  ret = FALSE;

  tty_name = ctermid (NULL);
  if (tty_name == NULL)
    {
      g_set_error (error, POLKIT_ERROR, POLKIT_ERROR_FAILED,
                   "Cannot determine pathname for current controlling terminal for the process: %s",
                   strerror (errno));
      goto out;
    }

  listener->tty = fopen (tty_name, "r+");
  if (listener->tty == NULL)
    {
      g_set_error (error, POLKIT_ERROR, POLKIT_ERROR_FAILED,
                   "Error opening current controlling terminal for the process (`%s'): %s",
                   tty_name, strerror (errno));
      goto out;
    }

  ret = TRUE;

 out:
  return ret;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

static void
on_completed (PolkitAgentSession *session,
              gboolean            gained_authorization,
              gpointer            user_data)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (user_data);

  if (flatpak_fancy_output ())
    fprintf (listener->tty, FLATPAK_ANSI_RED);
  if (gained_authorization)
    fprintf (listener->tty, "==== AUTHENTICATION COMPLETE ====\n");
  else
    fprintf (listener->tty, "==== AUTHENTICATION FAILED ====\n");
  if (flatpak_fancy_output ())
    {
      sleep (1); /* show the message for a bit */
      fprintf (listener->tty, FLATPAK_ANSI_COLOR_RESET FLATPAK_ANSI_ALT_SCREEN_OFF);
    }
  fflush (listener->tty);

  g_simple_async_result_complete_in_idle (listener->simple);

  g_object_unref (listener->simple);
  g_object_unref (listener->active_session);
  g_cancellable_disconnect (listener->cancellable, listener->cancel_id);
  g_object_unref (listener->cancellable);

  listener->simple = NULL;
  listener->active_session = NULL;
  listener->cancel_id = 0;
}

static void
on_request (PolkitAgentSession *session,
            const gchar        *request,
            gboolean            echo_on,
            gpointer            user_data)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (user_data);
  struct termios ts, ots;
  GString *str;

  fprintf (listener->tty, "%s", request);
  fflush (listener->tty);

  setbuf (listener->tty, NULL);

  tcgetattr (fileno (listener->tty), &ts);
  ots = ts;
  ts.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
  tcsetattr (fileno (listener->tty), TCSAFLUSH, &ts);

  str = g_string_new (NULL);
  while (TRUE)
    {
      gint c;
      c = getc (listener->tty);
      if (c == '\n')
        {
          /* ok, done */
          break;
        }
      else if (c == EOF)
        {
          tcsetattr (fileno (listener->tty), TCSAFLUSH, &ots);
          g_error ("Got unexpected EOF while reading from controlling terminal.");
          abort ();
          break;
        }
      else
        {
          g_string_append_c (str, c);
        }
    }
  tcsetattr (fileno (listener->tty), TCSAFLUSH, &ots);
  putc ('\n', listener->tty);

  polkit_agent_session_response (session, str->str);
  memset (str->str, '\0', str->len);
  g_string_free (str, TRUE);
}

static void
on_show_error (PolkitAgentSession *session,
               const gchar        *text,
               gpointer            user_data)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (user_data);
  fprintf (listener->tty, "Error: %s\n", text);
  fflush (listener->tty);
}

static void
on_show_info (PolkitAgentSession *session,
              const gchar        *text,
              gpointer            user_data)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (user_data);
  fprintf (listener->tty, "Info: %s\n", text);
  fflush (listener->tty);
}

static void
on_cancelled (GCancellable *cancellable,
              gpointer      user_data)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (user_data);
  fprintf (listener->tty, "Cancelled\n");
  fflush (listener->tty);
  polkit_agent_session_cancel (listener->active_session);
}

static gchar *
identity_to_human_readable_string (PolkitIdentity *identity)
{
  gchar *ret;

  g_return_val_if_fail (POLKIT_IS_IDENTITY (identity), NULL);

  ret = NULL;
  if (POLKIT_IS_UNIX_USER (identity))
    {
      struct passwd pw;
      struct passwd *ppw;
      char buf[2048];
      int res;

      res = getpwuid_r (polkit_unix_user_get_uid (POLKIT_UNIX_USER (identity)),
                        &pw,
                        buf,
                        sizeof buf,
                        &ppw);
      if (res != 0)
        {
          g_warning ("Error calling getpwuid_r: %s", strerror (res));
        }
      else
        {
          if (ppw->pw_gecos == NULL || strlen (ppw->pw_gecos) == 0 || strcmp (ppw->pw_gecos, ppw->pw_name) == 0)
            {
              ret = g_strdup_printf ("%s", ppw->pw_name);
            }
          else
            {
              ret = g_strdup_printf ("%s (%s)", ppw->pw_gecos, ppw->pw_name);
            }
        }
    }
  if (ret == NULL)
    ret = polkit_identity_to_string (identity);
  return ret;
}

static PolkitIdentity *
choose_identity (FlatpakPolkitAgentTextListener *listener,
                 GList                   *identities)
{
  GList *l;
  guint n;
  guint num_identities;
  GString *str;
  PolkitIdentity *ret;
  guint num;
  gchar *endp;

  ret = NULL;

  fprintf (listener->tty, "Multiple identities can be used for authentication:\n");
  for (l = identities, n = 0; l != NULL; l = l->next, n++)
    {
      PolkitIdentity *identity = POLKIT_IDENTITY (l->data);
      gchar *s;
      s = identity_to_human_readable_string (identity);
      fprintf (listener->tty, " %d.  %s\n", n + 1, s);
      g_free (s);
    }
  num_identities = n;
  fprintf (listener->tty, "Choose identity to authenticate as (1-%d): ", num_identities);
  fflush (listener->tty);

  str = g_string_new (NULL);
  while (TRUE)
    {
      gint c;
      c = getc (listener->tty);
      if (c == '\n')
        {
          /* ok, done */
          break;
        }
      else if (c == EOF)
        {
          g_error ("Got unexpected EOF while reading from controlling terminal.");
          abort ();
          break;
        }
      else
        {
          g_string_append_c (str, c);
        }
    }

  num = strtol (str->str, &endp, 10);
  if (str->len == 0 || *endp != '\0' || (num < 1 || num > num_identities))
    {
      fprintf (listener->tty, "Invalid response `%s'.\n", str->str);
      goto out;
    }

  ret = g_list_nth_data (identities, num-1);

 out:
  g_string_free (str, TRUE);
  return ret;
}


static void
flatpak_polkit_agent_text_listener_initiate_authentication (PolkitAgentListener  *_listener,
                                                            const gchar          *action_id,
                                                            const gchar          *message,
                                                            const gchar          *icon_name,
                                                            PolkitDetails        *details,
                                                            const gchar          *cookie,
                                                            GList                *identities,
                                                            GCancellable         *cancellable,
                                                            GAsyncReadyCallback   callback,
                                                            gpointer              user_data)
{
  FlatpakPolkitAgentTextListener *listener = FLATPAK_POLKIT_AGENT_TEXT_LISTENER (_listener);
  GSimpleAsyncResult *simple;
  PolkitIdentity *identity;

  simple = g_simple_async_result_new (G_OBJECT (listener),
                                      callback,
                                      user_data,
                                      flatpak_polkit_agent_text_listener_initiate_authentication);
  if (listener->active_session != NULL)
    {
      g_simple_async_result_set_error (simple, POLKIT_ERROR, POLKIT_ERROR_FAILED,
                                       "An authentication session is already underway.");
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      goto out;
    }

  g_assert (g_list_length (identities) >= 1);

  if (flatpak_fancy_output ())
    {
      fprintf (listener->tty, FLATPAK_ANSI_ALT_SCREEN_ON FLATPAK_ANSI_RED);
    }

  fprintf (listener->tty, "==== AUTHENTICATING FOR %s ====\n", action_id);

  if (flatpak_fancy_output ())
    fprintf (listener->tty, FLATPAK_ANSI_COLOR_RESET);

  fprintf (listener->tty, "%s\n", message);

  /* handle multiple identies by asking which one to use */
  if (g_list_length (identities) > 1)
    {
      identity = choose_identity (listener, identities);
      if (identity == NULL)
        {
          fprintf (listener->tty, "\x1B[1;31m");
          fprintf (listener->tty, "==== AUTHENTICATION CANCELED ====\n");
          fprintf (listener->tty, "\x1B[0m");
          fflush (listener->tty);
          g_simple_async_result_set_error (simple,
                                           POLKIT_ERROR,
                                           POLKIT_ERROR_FAILED,
                                           "Authentication was canceled.");
          g_simple_async_result_complete_in_idle (simple);
          g_object_unref (simple);
          goto out;
        }
    }
  else
    {
      gchar *s;
      identity = identities->data;
      s = identity_to_human_readable_string (identity);
      fprintf (listener->tty,
               "Authenticating as: %s\n",
               s);
      g_free (s);
    }

  listener->active_session = polkit_agent_session_new (identity, cookie);
  g_signal_connect (listener->active_session,
                    "completed",
                    G_CALLBACK (on_completed),
                    listener);
  g_signal_connect (listener->active_session,
                    "request",
                    G_CALLBACK (on_request),
                    listener);
  g_signal_connect (listener->active_session,
                    "show-info",
                    G_CALLBACK (on_show_info),
                    listener);
  g_signal_connect (listener->active_session,
                    "show-error",
                    G_CALLBACK (on_show_error),
                    listener);

  listener->simple = simple;
  listener->cancellable = g_object_ref (cancellable);
  listener->cancel_id = g_cancellable_connect (cancellable,
                                               G_CALLBACK (on_cancelled),
                                               listener,
                                               NULL);

  polkit_agent_session_initiate (listener->active_session);

 out:
  ;
}

static gboolean
flatpak_polkit_agent_text_listener_initiate_authentication_finish (PolkitAgentListener  *_listener,
                                                                   GAsyncResult         *res,
                                                                   GError              **error)
{
  g_warn_if_fail (g_simple_async_result_get_source_tag (G_SIMPLE_ASYNC_RESULT (res)) ==
                  flatpak_polkit_agent_text_listener_initiate_authentication);

  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
    return FALSE;

  return TRUE;
}
