/* Small test helper that exercises flatpak_download_http_uri() */

#include "libglnx.h"
#include "common/flatpak-utils-http-private.h"
#include "common/flatpak-utils-private.h"

int
main (int argc, char *argv[])
{
  g_autoptr(FlatpakHttpSession) session = flatpak_create_http_session (PACKAGE_STRING);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) dest_file = NULL;
  g_autoptr(GFileOutputStream) out_stream = NULL;
  const char *url, *dest;

  if (argc != 3)
    {
      g_printerr ("Usage: httpdownload URL DEST\n");
      return 1;
    }

  url = argv[1];
  dest = argv[2];

  dest_file = g_file_new_for_path (dest);
  out_stream = g_file_replace (dest_file, NULL, FALSE,
                               G_FILE_CREATE_REPLACE_DESTINATION,
                               NULL, &error);
  if (out_stream == NULL)
    {
      g_printerr ("Failed to open %s: %s\n", dest, error->message);
      return 1;
    }

  if (!flatpak_download_http_uri (session, url, NULL,
                                  FLATPAK_HTTP_FLAGS_NONE,
                                  G_OUTPUT_STREAM (out_stream),
                                  NULL,
                                  NULL, NULL,
                                  NULL, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }

  if (!g_output_stream_close (G_OUTPUT_STREAM (out_stream), NULL, &error))
    {
      g_printerr ("Failed to close %s: %s\n", dest, error->message);
      return 1;
    }

  g_print ("Download succeeded\n");
  return 0;
}
