#include "common/flatpak-utils-private.h"

int
main (int argc, char *argv[])
{
  SoupSession *session = flatpak_create_soup_session (PACKAGE_STRING);
  g_autoptr(GFile) dest = NULL;
  GError *error = NULL;

  if (argc != 3)
    {
      g_printerr("Usage testhttp URL DEST\n");
      return 1;
    }

  if (!flatpak_cache_http_uri (session,
			       argv[1],
			       0,
			       AT_FDCWD, argv[2],
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
