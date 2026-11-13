#include <curl/curl.h>

#include "libglnx.h"
#include "common/flatpak-utils-http-private.h"

static gboolean
request (FlatpakHttpSession  *session,
         const char          *base_uri,
         const char          *path,
         FlatpakCertificates *certificates,
         GBytes             **out_body)
{
  g_autofree char *uri = g_strconcat (base_uri, path, NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body = NULL;

  body = flatpak_load_uri_full (session, uri, certificates, 0,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, &error);
  if (body == NULL)
    {
      g_printerr ("Request to %s failed: %s\n", uri, error->message);
      return FALSE;
    }

  if (out_body != NULL)
    *out_body = g_steal_pointer (&body);
  return TRUE;
}

int
main (int argc, char *argv[])
{
  g_autoptr(FlatpakHttpSession) session = NULL;
  g_autoptr(FlatpakCertificates) first_certificates = NULL;
  g_autoptr(FlatpakCertificates) second_certificates = NULL;
  g_autoptr(GBytes) first = NULL;
  g_autoptr(GBytes) second = NULL;
  g_autoptr(GError) error = NULL;

  if (argc == 2 && g_str_equal (argv[1], "--default-ca-path"))
    {
      /* This is the build-time default, not the selected CURLOPT_CAINFO.
       * Older libcurl versions can still run the other TLS tests. */
#if LIBCURL_VERSION_NUM >= 0x075400 /* 7.84.0 */
      CURL *curl = curl_easy_init ();
      char *path = NULL;
      int status = 77;

      g_assert (curl != NULL);
      if (curl_easy_getinfo (curl, CURLINFO_CAINFO, &path) == CURLE_OK &&
          path != NULL && g_path_is_absolute (path) &&
          g_file_test (path, G_FILE_TEST_IS_REGULAR))
        {
          g_print ("%s\n", path);
          status = 0;
        }
      curl_easy_cleanup (curl);
      return status;
#else
      return 77;
#endif
    }

  if (argc != 4)
    {
      g_printerr ("Usage: http-session MODE MTLS_URL CA_ONLY_URL\n");
      return 1;
    }

  session = flatpak_create_http_session ("flatpak-http-session-test");
  first_certificates = flatpak_get_certificates_for_uri (argv[2], &error);
  if (first_certificates == NULL)
    {
      g_printerr ("Failed to load certificates: %s\n", error->message);
      return 1;
    }

  second_certificates = flatpak_get_certificates_for_uri (argv[3], &error);
  if (second_certificates == NULL)
    {
      g_printerr ("Failed to load certificates: %s\n", error->message);
      return 1;
    }

  if (g_str_equal (argv[1], "ca-reset"))
    {
      g_autofree char *uri = g_strconcat (argv[3], "/forbid-client-cert?close", NULL);
      g_autofree char *tls_error = g_strdup_printf ("[%u]", CURLE_PEER_FAILED_VERIFICATION);

      if (!request (session, argv[3], "/forbid-client-cert?close", second_certificates, NULL))
        return 1;

      /* A newly generated private CA must not become trusted by a caller
       * that passes NULL certificates after using explicit custom trust. */
      first = flatpak_load_uri (session, uri, 0, NULL, NULL, NULL, NULL, NULL, &error);
      if (first != NULL || !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED) ||
          strstr (error->message, tls_error) == NULL)
        {
          g_printerr ("Expected a TLS verification failure without custom trust, got: %s\n",
                      error != NULL ? error->message : "success");
          return 1;
        }

      return request (session, argv[3], "/forbid-client-cert", second_certificates, NULL) ? 0 : 1;
    }

  if (!g_str_equal (argv[1], "client-cert") &&
      !g_str_equal (argv[1], "same-origin") &&
      !g_str_equal (argv[1], "default-ca"))
    {
      g_printerr ("Unknown mode: %s\n", argv[1]);
      return 1;
    }

  if (!request (session, argv[2], "/require-client-cert", first_certificates, &first))
    return 1;

  if (g_str_equal (argv[1], "same-origin"))
    {
      /* The server returns the peer port so this checks that keep-alive
       * really works before changing TLS profiles on this origin. */
      if (!request (session, argv[2], "/require-client-cert", first_certificates, &second))
        return 1;
      if (!g_bytes_equal (first, second))
        {
          g_printerr ("The control request did not reuse its connection\n");
          return 1;
        }
      if (!request (session, argv[2], "/forbid-client-cert", second_certificates, NULL))
        return 1;
    }
  else if (g_str_equal (argv[1], "default-ca"))
    {
      /* The shell test mounts the fixture CA at libcurl's built-in bundle
       * path in a private mount namespace. No explicit CA is supplied here. */
      if (!request (session, argv[3], "/forbid-client-cert?close", NULL, NULL))
        return 1;
    }
  else if (!request (session, argv[3], "/forbid-client-cert", second_certificates, NULL))
    return 1;

  if (!request (session, argv[2], "/require-client-cert", first_certificates, NULL))
    return 1;

  return 0;
}
