/* Small test helper that exercises flatpak_download_http_uri() */

#include "libglnx.h"
#include "common/flatpak-utils-http-private.h"
#include "common/flatpak-utils-private.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

static void
progress_cb (guint64  downloaded_bytes,
             gpointer user_data)
{
  GString *progress = user_data;

  if (progress->len > 0)
    g_string_append_c (progress, ',');

  g_string_append_printf (progress, "%" G_GUINT64_FORMAT, downloaded_bytes);
}

int
main (int argc, char *argv[])
{
  g_autoptr(FlatpakHttpSession) session = flatpak_create_http_session (PACKAGE_STRING);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) dest_file = NULL;
  g_autoptr(GOutputStream) out_stream = NULL;
  g_autoptr(GString) progress = NULL;
  gboolean use_unix_output_stream = FALSE;
  gboolean print_progress = FALSE;
  const char *url, *dest;
  int arg = 1;

  while (arg < argc && g_str_has_prefix (argv[arg], "--"))
    {
      if (strcmp (argv[arg], "--unix-output-stream") == 0)
        use_unix_output_stream = TRUE;
      else if (strcmp (argv[arg], "--progress") == 0)
        print_progress = TRUE;
      else
        {
          g_printerr ("Unknown option %s\n", argv[arg]);
          return 1;
        }

      arg++;
    }

  if (argc - arg != 2)
    {
      g_printerr ("Usage: httpdownload [--unix-output-stream] [--progress] URL DEST\n");
      return 1;
    }

  url = argv[arg++];
  dest = argv[arg++];
  progress = g_string_new ("");

  if (use_unix_output_stream)
    {
      int fd = g_open (dest, O_CREAT | O_TRUNC | O_WRONLY, 0600);

      if (fd == -1)
        {
          g_printerr ("Failed to open %s: %s\n", dest, g_strerror (errno));
          return 1;
        }

      out_stream = g_unix_output_stream_new (fd, TRUE);
    }
  else
    {
      dest_file = g_file_new_for_path (dest);
      out_stream = (GOutputStream *) g_file_replace (dest_file, NULL, FALSE,
                                                     G_FILE_CREATE_REPLACE_DESTINATION,
                                                     NULL, &error);
    }

  if (out_stream == NULL)
    {
      g_printerr ("Failed to open %s: %s\n", dest, error->message);
      return 1;
    }

  if (!flatpak_download_http_uri (session, url, NULL,
                                  FLATPAK_HTTP_FLAGS_NONE,
                                  G_OUTPUT_STREAM (out_stream),
                                  NULL,
                                  print_progress ? progress_cb : NULL, progress,
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

  if (print_progress)
    g_print ("Download succeeded; progress=%s\n", progress->str);
  else
    g_print ("Download succeeded\n");

  return 0;
}
