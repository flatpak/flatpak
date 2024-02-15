#include "libglnx.h"
#include "common/flatpak-utils-http-private.h"
#include "common/flatpak-utils-private.h"

int
main (int argc, char *argv[])
{
  g_autoptr(FlatpakHttpSession) session = flatpak_create_http_session (PACKAGE_STRING);
  g_autoptr(GError) error = NULL;
  const char *url, *dest;
  int flags = 0;

  /* Avoid weird recursive type initialization deadlocks from libsoup */
  g_type_ensure (G_TYPE_SOCKET);

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
  else
    {
      g_printerr ("Usage httpcache [--compressed] URL DEST\n");
      return 1;
    }


  if (!flatpak_cache_http_uri (session,
                               url,
                               flags,
                               AT_FDCWD, dest,
                               NULL, NULL, NULL, &error))
    {
      g_print ("%s\n", error->message);
      return 1;
    }
  else
    {
      g_print ("Server returned status 200: ok\n");
      return 0;
    }
}
