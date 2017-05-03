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

#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include "permission-store-dbus.h"
#include "xdg-permission-store.h"
#include "flatpak-utils.h"

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  xdg_permission_store_start (connection);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  exit (1);
}

static gboolean opt_verbose;
static gboolean opt_replace;
static gboolean opt_version;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version and exit", NULL },
  { NULL }
};

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    printf ("XDP: %s\n", message);
  else
    printf ("%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const gchar *string)
{
  const char *prefix = "";
  const char *suffix = "";
  if (flatpak_fancy_output ())
    {
      prefix = FLATPAK_ANSI_RED FLATPAK_ANSI_BOLD_ON;
      suffix = FLATPAK_ANSI_BOLD_OFF FLATPAK_ANSI_COLOR_RESET;
    }
  fprintf (stderr, "%serror: %s%s\n", prefix, suffix, string);
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;
  GMainLoop *loop;
  GOptionContext *context;
  g_autoptr(GError) error = NULL;

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  flatpak_migrate_from_xdg_app ();

  g_set_printerr_handler (printerr_handler);

  context = g_option_context_new ("- permission store");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s", error->message);
      return 1;
    }

  if (opt_version)
    {
      g_print ("%s\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.impl.portal.PermissionStore",
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),

                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  return 0;
}
