#include "flatpak-glib-backports-private.h"
#include "portal/flatpak-portal.h"
#include "portal/flatpak-portal-dbus.h"

typedef struct {
  GMainLoop *loop;
  gboolean   done;
} SpawnData;

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
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GError) error = NULL;
  SpawnData data = { NULL, FALSE };
  guint pid;

  if (argc < 2)
    {
      g_printerr ("A command to run is required\n");
      return 1;
    }

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

  if (!portal_flatpak_call_spawn_sync (portal,
                                       "/",   /* cwd */
                                       (const char * const *) &argv[1],
                                       g_variant_new ("a{uh}", NULL),
                                       g_variant_new ("a{ss}", NULL),
                                       FLATPAK_SPAWN_FLAGS_NONE,
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
