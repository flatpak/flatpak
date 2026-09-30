#include "libglnx.h"

#include "flatpak-glib-backports-private.h"
#include "portal/flatpak-portal.h"
#include "portal/flatpak-portal-dbus.h"

static gboolean opt_notify_start;
static const char *opt_write_after_start;

static GOptionEntry options[] = {
  { "notify-start", 0, 0, G_OPTION_ARG_NONE, &opt_notify_start,
    "Pass FLATPAK_SPAWN_FLAGS_NOTIFY_START", NULL },
  { "write-after-start", 0, 0, G_OPTION_ARG_STRING, &opt_write_after_start,
    "Write 'done\\n' to the given path after SpawnStarted is received", NULL },
  { NULL }
};

typedef struct {
  GMainLoop *loop;
  gboolean   done;
} SpawnData;

static void
spawn_started_cb (PortalFlatpak *portal,
                  guint          pid,
                  guint          relpid,
                  gpointer       user_data)
{
  g_print ("spawn-started pid=%u relpid=%u\n", pid, relpid);

  if (opt_write_after_start != NULL)
    {
      glnx_autofd int fd = open (opt_write_after_start, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      const char done[] = "done\n";
      if (fd == -1 || glnx_loop_write (fd, done, strlen (done)) == -1)
        {
          int saved_errno = errno;
          g_error ("writing to %s: %s", opt_write_after_start, strerror (saved_errno));
        }
    }
}

static void
spawn_exited_cb (PortalFlatpak *portal,
                 guint          pid,
                 guint          exit_status,
                 gpointer       user_data)
{
  SpawnData *data = user_data;

  g_print ("spawn-exited pid=%u status=%u\n", pid, exit_status);
  data->done = TRUE;
  g_main_loop_quit (data->loop);
}

static gboolean
spawn_timeout_cb (gpointer user_data)
{
  SpawnData *data = user_data;

  g_printerr ("Timed out waiting for SpawnStarted or SpawnExited\n");
  g_main_loop_quit (data->loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char *argv[])
{
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(PortalFlatpak) portal = NULL;
  g_autoptr(GUnixFDList) fds_out = NULL;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GError) error = NULL;
  guint flags = FLATPAK_SPAWN_FLAGS_NONE;
  SpawnData data = { NULL, FALSE };
  guint pid;

  context = g_option_context_new ("COMMAND [ARG...]");
  g_option_context_set_strict_posix (context, TRUE);
  g_option_context_add_main_entries (context, options, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s\n", error->message);
      return 1;
    }

  if (argc < 2)
    {
      g_printerr ("A command to run is required\n");
      return 1;
    }

  if (opt_notify_start)
    flags |= FLATPAK_SPAWN_FLAGS_NOTIFY_START;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_printerr ("Error connecting to session bus: %s\n", error->message);
      return 1;
    }

  portal = portal_flatpak_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_NONE,
                                          FLATPAK_PORTAL_BUS_NAME,
                                          FLATPAK_PORTAL_PATH,
                                          NULL, &error);
  if (portal == NULL)
    {
      g_printerr ("Error creating portal proxy: %s\n", error->message);
      return 1;
    }

  loop = g_main_loop_new (NULL, FALSE);
  data.loop = loop;

  g_signal_connect (portal, "spawn-exited", G_CALLBACK (spawn_exited_cb), &data);
  if (opt_notify_start)
    g_signal_connect (portal, "spawn-started", G_CALLBACK (spawn_started_cb), &data);

  if (!portal_flatpak_call_spawn_sync (portal,
                                       "/",   /* cwd */
                                       (const char * const *) &argv[1],
                                       g_variant_new ("a{uh}", NULL),
                                       g_variant_new ("a{ss}", NULL),
                                       flags,
                                       g_variant_new ("a{sv}", NULL),
                                       NULL, /* fd list */
                                       &pid,
                                       &fds_out,
                                       NULL,
                                       &error))
    {
      g_printerr ("Error calling Spawn: %s\n", error->message);
      return 1;
    }

  if (!data.done)
    {
      g_timeout_add_seconds (15, spawn_timeout_cb, &data);
      g_main_loop_run (loop);

      if (!data.done)
        return 1;
    }

  return 0;
}
