#include "libglnx.h"
#include "common/flatpak-utils-http-private.h"
#include "common/flatpak-utils-private.h"

int
main (int argc, char *argv[])
{
  gboolean opt_compressed = FALSE;
  gboolean opt_force_expired = FALSE;
  GOptionEntry entries[] = {
    { "compressed", 0, 0, G_OPTION_ARG_NONE, &opt_compressed, "Compress the cached file", NULL },
    { "force-expired", 0, 0, G_OPTION_ARG_NONE, &opt_force_expired, "Force time-based expiration of the cache", NULL },
    { NULL }
  };

  g_autoptr(FlatpakHttpSession) session = flatpak_create_http_session (PACKAGE_STRING);
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;
  const char *url, *dest;
  int flags = 0;

  context = g_option_context_new ("URL DEST - Test downloading a file with HTTP caching");
  g_option_context_add_main_entries (context, entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Error: %s\n", error->message);
      return 1;
    }

  if (argc != 3)
    {
      g_printerr ("Usage: %s [--compressed] [--force-expired] URL DEST\n", argv[0]);
      return 1;
    }

  url = argv[1];
  dest = argv[2];

  if (opt_compressed)
    flags |= FLATPAK_HTTP_FLAGS_STORE_COMPRESSED;
  if (opt_force_expired)
      flags |= FLATPAK_HTTP_FLAGS_FORCE_EXPIRED;

  if (!flatpak_cache_http_uri (session,
                               url, NULL,
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
