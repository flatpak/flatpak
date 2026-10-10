#include "libglnx.h"
#include "common/flatpak-utils-http-private.h"
#include "common/flatpak-utils-private.h"

static gpointer
cancel_request (gpointer user_data)
{
  g_usleep (200 * 1000);
  g_cancellable_cancel (user_data);
  return NULL;
}

int
main (int argc, char *argv[])
{
  g_autoptr(FlatpakHttpSession) session = flatpak_create_http_session (PACKAGE_STRING);
  g_autoptr(GError) error = NULL;
  const char *url, *dest;
  int flags = 0;
  g_autoptr(GCancellable) cancellable = NULL;
  g_autoptr(GThread) cancel_thread = NULL;
  gboolean success;

  if (argc == 3)
    {
      url = argv[1];
      dest = argv[2];
    }
  else if (argc == 4 && g_strcmp0 (argv[1], "--compressed") == 0)
    {
      url = argv[2];
      dest = argv[3];
      flags |= FLATPAK_HTTP_FLAGS_STORE_COMPRESSED;
    }
  else if (argc == 4 && g_strcmp0 (argv[1], "--cancel") == 0)
    {
      url = argv[2];
      dest = argv[3];
      cancellable = g_cancellable_new ();
      cancel_thread = g_thread_new ("cancel-request", cancel_request, cancellable);
    }
  else
    {
      g_printerr ("Usage httpcache [--compressed|--cancel] URL DEST\n");
      return 1;
    }


  success = flatpak_cache_http_uri (session,
                                   url, NULL,
                                   flags,
                                   AT_FDCWD, dest,
                                   NULL, NULL, cancellable, &error);
  if (cancel_thread)
    g_thread_join (g_steal_pointer (&cancel_thread));

  if (!success)
    {
      if (cancellable && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_print ("Cancelled\n");
          return 0;
        }
      g_print ("%s\n", error->message);
      return 1;
    }
  else
    {
      g_print ("Server returned status 200: ok\n");
      return 0;
    }
}
