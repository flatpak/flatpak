/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright (C) 2026 Red Hat, Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "flatpak-auth-private.h"
#include "flatpak-dbus-generated.h"

typedef struct {
  int basic_auth_count;
  guint response_code;
  gboolean done;
} TestData;

static void
on_basic_auth (FlatpakAuthenticatorRequest *request,
               const gchar *realm,
               GVariant *options,
               TestData *data)
{
  g_autoptr(GVariant) reply_options = NULL;
  g_autoptr(GError) error = NULL;

  data->basic_auth_count++;
  g_info ("BasicAuth signal received (count=%d), realm=%s", data->basic_auth_count, realm);

  reply_options = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));
  flatpak_authenticator_request_call_basic_auth_reply_sync (request,
                                                            "user", "pass",
                                                            reply_options,
                                                            NULL, &error);
  g_assert_no_error (error);
}

static void
on_response (FlatpakAuthenticatorRequest *request,
             guint response,
             GVariant *results,
             TestData *data)
{
  data->response_code = response;
  data->done = TRUE;
}

static GVariant *
build_test_refs (const char *registry_uri)
{
  GVariantBuilder refs_builder;
  GVariantBuilder metadata_builder;

  g_variant_builder_init (&refs_builder, G_VARIANT_TYPE ("a(ssia{sv})"));
  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (&metadata_builder, "{sv}",
                         "summary.xa.oci-repository",
                         g_variant_new_string ("testrepo"));

  g_variant_builder_add (&refs_builder, "(ssi@a{sv})",
                         "app/org.test.App/x86_64/stable",
                         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
                         1,
                         g_variant_builder_end (&metadata_builder));

  return g_variant_ref_sink (g_variant_builder_end (&refs_builder));
}

static GVariant *
build_test_options (const char *registry_uri)
{
  GVariantBuilder options_builder;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&options_builder, "{sv}",
                         "xa.oci-registry-uri",
                         g_variant_new_string (registry_uri));

  return g_variant_ref_sink (g_variant_builder_end (&options_builder));
}

static void
do_request_ref_tokens (FlatpakAuthenticator *authenticator,
                       const char *registry_uri,
                       TestData *data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(AutoFlatpakAuthenticatorRequest) request = NULL;
  g_autoptr(GVariant) refs = NULL;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GVariant) auth_options = NULL;

  data->done = FALSE;
  data->response_code = -1;

  request = flatpak_auth_create_request (authenticator, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (request);

  g_signal_connect (request, "basic-auth", G_CALLBACK (on_basic_auth), data);
  g_signal_connect (request, "response", G_CALLBACK (on_response), data);

  refs = build_test_refs (registry_uri);
  options = build_test_options (registry_uri);
  auth_options = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));

  g_object_set_data_full (G_OBJECT (authenticator), "authenticator-options",
                          g_variant_ref (auth_options), (GDestroyNotify) g_variant_unref);

  if (!flatpak_auth_request_ref_tokens (authenticator, request,
                                        "test-remote", registry_uri,
                                        refs, options, "",
                                        NULL, &error))
    {
      g_error ("RequestRefTokens failed: %s", error->message);
    }

  while (!data->done)
    g_main_context_iteration (NULL, TRUE);
}

static void
test_credential_cache (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(AutoFlatpakAuthenticator) authenticator = NULL;
  g_autofree char *registry_uri = NULL;
  g_autofree char *port_str = NULL;
  TestData data = { 0 };

  g_file_get_contents ("httpd-port", &port_str, NULL, &error);
  g_assert_no_error (error);
  g_strstrip (port_str);
  registry_uri = g_strdup_printf ("http://127.0.0.1:%s", port_str);

  authenticator = flatpak_authenticator_proxy_new_for_bus_sync (
    G_BUS_TYPE_SESSION,
    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
    "org.flatpak.Authenticator.Oci",
    FLATPAK_AUTHENTICATOR_OBJECT_PATH,
    NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (authenticator);

  /* First request: should trigger BasicAuth */
  do_request_ref_tokens (authenticator, registry_uri, &data);
  g_assert_cmpuint (data.response_code, ==, FLATPAK_AUTH_RESPONSE_OK);
  g_assert_cmpint (data.basic_auth_count, ==, 1);

  /* Second request: should use cached credentials, no BasicAuth */
  do_request_ref_tokens (authenticator, registry_uri, &data);
  g_assert_cmpuint (data.response_code, ==, FLATPAK_AUTH_RESPONSE_OK);
  g_assert_cmpint (data.basic_auth_count, ==, 1);
}

int
main (int argc, char **argv)
{
  test_credential_cache ();
  return 0;
}
