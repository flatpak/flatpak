/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2019 Red Hat, Inc
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
#include "flatpak-run-x11-private.h"

#include <sys/utsname.h>

#ifdef ENABLE_XAUTH
#include <X11/Xauth.h>
#endif

/* This is part of the X11 protocol, so we can safely hard-code it here */
#define FamilyInternet6 (6)

#ifdef ENABLE_XAUTH
static gboolean
auth_streq (const char *str,
            const char *au_str,
            size_t      au_len)
{
  return au_len == strlen (str) && memcmp (str, au_str, au_len) == 0;
}

static gboolean
xauth_entry_should_propagate (const Xauth *xa,
                              int          family,
                              const char  *remote_hostname,
                              const char  *local_hostname,
                              const char  *number)
{
  /* ensure entry isn't for a different type of access */
  if (family != FamilyWild && xa->family != family && xa->family != FamilyWild)
    return FALSE;

  /* ensure entry isn't for remote access, except that if remote_hostname
   * is specified, then remote access to that hostname is OK */
  if (xa->family != FamilyWild && xa->family != FamilyLocal &&
      (remote_hostname == NULL ||
       !auth_streq (remote_hostname, xa->address, xa->address_length)))
    return FALSE;

  /* ensure entry is for this machine */
  if (xa->family == FamilyLocal && !auth_streq (local_hostname, xa->address, xa->address_length))
    {
      /* OpenSUSE inherits the hostname value from DHCP without updating
       * its X11 authentication cookie. The old hostname value can still
       * be found in the environment variable XAUTHLOCALHOSTNAME.
       * For reference:
       * https://bugzilla.opensuse.org/show_bug.cgi?id=262309
       * For this reason if we have a cookie whose address is equal to the
       * variable XAUTHLOCALHOSTNAME, we still need to propagate it, but
       * we also need to change its address to `unames.nodename`.
       */
      const char *xauth_local_hostname;
      xauth_local_hostname = g_getenv ("XAUTHLOCALHOSTNAME");
      if (xauth_local_hostname == NULL)
        return FALSE;

      if (!auth_streq ((char *) xauth_local_hostname, xa->address, xa->address_length))
        return FALSE;
    }

  /* ensure entry is for this session */
  if (xa->number != NULL && !auth_streq (number, xa->number, xa->number_length))
    return FALSE;

  return TRUE;
}

static void
write_xauth (int family,
             const char *remote_host,
             const char *number,
             FILE       *output)
{
  Xauth *xa, local_xa;
  char *filename;
  FILE *f;
  struct utsname unames;

  if (uname (&unames))
    {
      g_warning ("uname failed");
      return;
    }

  filename = XauFileName ();
  f = fopen (filename, "rb");
  if (f == NULL)
    return;

  while (TRUE)
    {
      xa = XauReadAuth (f);
      if (xa == NULL)
        break;
      if (xauth_entry_should_propagate (xa, family, remote_host,
                                        unames.nodename, number))
        {
          local_xa = *xa;

          if (local_xa.family == FamilyLocal &&
              !auth_streq (unames.nodename, local_xa.address, local_xa.address_length))
            {
              /* If we decided to propagate this cookie, but its address
               * doesn't match `unames.nodename`, we need to change it or
               * inside the container it will not work.
               */
              local_xa.address = unames.nodename;
              local_xa.address_length = strlen (local_xa.address);
            }

          if (!XauWriteAuth (output, &local_xa))
            g_warning ("xauth write error");
        }

      XauDisposeAuth (xa);
    }

  fclose (f);
}
#else /* !ENABLE_XAUTH */

/* When not doing Xauth, any distinct values will do, but use the same
 * ones Xauth does so that we can refer to them in our unit test. */
#define FamilyLocal (256)
#define FamilyWild (65535)

#endif /* !ENABLE_XAUTH */

/*
 * @family: (out) (not optional):
 * @x11_socket: (out) (not optional):
 * @display_nr_out: (out) (not optional):
 */
gboolean
flatpak_run_parse_x11_display (const char  *display,
                               int         *family,
                               char       **x11_socket,
                               char       **remote_host,
                               char       **display_nr_out,
                               GError     **error)
{
  const char *colon;
  const char *display_nr;
  const char *display_nr_end;

  /* Use the last ':', not the first, to cope with [::1]:0 */
  colon = strrchr (display, ':');

  if (colon == NULL)
    return glnx_throw (error, "No colon found in DISPLAY=%s", display);

  if (!g_ascii_isdigit (colon[1]))
    return glnx_throw (error, "Colon not followed by a digit in DISPLAY=%s", display);

  display_nr = &colon[1];
  display_nr_end = display_nr;

  while (g_ascii_isdigit (*display_nr_end))
    display_nr_end++;

  *display_nr_out = g_strndup (display_nr, display_nr_end - display_nr);

  if (display == colon || g_str_has_prefix (display, "unix:"))
    {
      *family = FamilyLocal;
      *x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", *display_nr_out);
    }
  else if (display[0] == '[' && display[colon - display - 1] == ']')
    {
      *family = FamilyInternet6;
      *remote_host = g_strndup (display + 1, colon - display - 2);
    }
  else
    {
      *family = FamilyWild;
      *remote_host = g_strndup (display, colon - display);
    }

  return TRUE;
}

void
flatpak_run_add_x11_args (FlatpakBwrap         *bwrap,
                          gboolean              allowed,
                          FlatpakContextShares  shares)
{
  g_autofree char *x11_socket = NULL;
  const char *display;
  g_autoptr(GError) local_error = NULL;

  /* Always cover /tmp/.X11-unix, that way we never see the host one in case
   * we have access to the host /tmp. If you request X access we'll put the right
   * thing in this anyway.
   *
   * We need to be a bit careful here, because there are two situations in
   * which potentially hostile processes have access to /tmp and could
   * create symlinks, which in principle could cause us to create the
   * directory and mount the tmpfs at the target of the symlink instead
   * of in the intended place:
   *
   * - With --filesystem=/tmp, it's the host /tmp - but because of the
   *   special historical status of /tmp/.X11-unix, we can assume that
   *   it is pre-created by the host system before user code gets to run.
   *
   * - When /tmp is shared between all instances of the same app ID,
   *   in principle the app has control over what's in /tmp, but in
   *   practice it can't interfere with /tmp/.X11-unix, because we do
   *   this unconditionally - therefore by the time app code runs,
   *   /tmp/.X11-unix is already a mount point, meaning the app cannot
   *   rename or delete it.
   */
  flatpak_bwrap_add_args (bwrap,
                          "--tmpfs", "/tmp/.X11-unix",
                          NULL);

  if (!allowed)
    {
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
      return;
    }

  g_info ("Allowing x11 access");

  display = g_getenv ("DISPLAY");

  if (display != NULL)
    {
      g_autofree char *remote_host = NULL;
      g_autofree char *display_nr = NULL;
      int family = -1;

      if (!flatpak_run_parse_x11_display (display, &family, &x11_socket,
                                          &remote_host, &display_nr,
                                          &local_error))
        {
          g_warning ("%s", local_error->message);
          flatpak_bwrap_unset_env (bwrap, "DISPLAY");
          return;
        }

      g_assert (display_nr != NULL);

      if (x11_socket != NULL
          && g_file_test (x11_socket, G_FILE_TEST_EXISTS))
        {
          g_assert (g_str_has_prefix (x11_socket, "/tmp/.X11-unix/X"));
          flatpak_bwrap_add_args (bwrap,
                                  "--ro-bind", x11_socket, x11_socket,
                                  NULL);
          flatpak_bwrap_set_env (bwrap, "DISPLAY", display, TRUE);
        }
      else if ((shares & FLATPAK_CONTEXT_SHARED_NETWORK) == 0)
        {
          /* If DISPLAY is for example :42 but /tmp/.X11-unix/X42
           * doesn't exist, then the only way this is going to work
           * is if the app can connect to abstract socket
           * @/tmp/.X11-unix/X42 or to TCP port localhost:6042,
           * either of which requires a shared network namespace.
           *
           * Alternatively, if DISPLAY is othermachine:23, then we
           * definitely need access to TCP port othermachine:6023. */
          if (x11_socket != NULL)
            g_warning ("X11 socket %s does not exist in filesystem.",
                       x11_socket);
          else
            g_warning ("Remote X11 display detected.");

          g_warning ("X11 access will require --share=network permission.");
        }
      else if (x11_socket != NULL)
        {
          g_warning ("X11 socket %s does not exist in filesystem, "
                     "trying to use abstract socket instead.",
                     x11_socket);
        }
      else
        {
          g_debug ("Assuming --share=network gives access to remote X11");
        }

#ifdef ENABLE_XAUTH
      g_auto(GLnxTmpfile) xauth_tmpf  = { 0, };

      if (glnx_open_anonymous_tmpfile_full (O_RDWR | O_CLOEXEC, "/tmp", &xauth_tmpf, NULL))
        {
          FILE *output = fdopen (xauth_tmpf.fd, "wb");
          if (output != NULL)
            {
              /* fd is now owned by output, steal it from the tmpfile */
              int tmp_fd = dup (glnx_steal_fd (&xauth_tmpf.fd));
              if (tmp_fd != -1)
                {
                  static const char dest[] = "/run/flatpak/Xauthority";

                  write_xauth (family, remote_host, display_nr, output);
                  flatpak_bwrap_add_args_data_fd (bwrap, "--ro-bind-data", tmp_fd, dest);

                  flatpak_bwrap_set_env (bwrap, "XAUTHORITY", dest, TRUE);
                }

              fclose (output);

              if (tmp_fd != -1)
                lseek (tmp_fd, 0, SEEK_SET);
            }
        }
#endif
    }
  else
    {
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
    }
}
