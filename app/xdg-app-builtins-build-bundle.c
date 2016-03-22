/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <gio/gunixinputstream.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"
#include "xdg-app-chain-input-stream.h"

static char *opt_arch;
static char *opt_repo_url;
static gboolean opt_runtime = FALSE;
static char **opt_gpg_file;

static GOptionEntry options[] = {
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Export runtime instead of app"},
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to bundle for", "ARCH" },
  { "repo-url", 0, 0, G_OPTION_ARG_STRING, &opt_repo_url, "Url for repo", "URL" },
  { "gpg-keys", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, "Add GPG key from FILE (- for stdin)", "FILE" },

  { NULL }
};

static GBytes *
read_gpg_data (GCancellable *cancellable,
               GError **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  g_autoptr(GPtrArray) streams = NULL;

  if (opt_gpg_file != NULL)
    n_keyrings = g_strv_length (opt_gpg_file);

  guint ii;

  streams = g_ptr_array_new_with_free_func (g_object_unref);

  for (ii = 0; ii < n_keyrings; ii++)
    {
      GInputStream *input_stream = NULL;

      if (strcmp (opt_gpg_file[ii], "-") == 0)
        {
          input_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
        }
      else
        {
          g_autoptr(GFile) file = g_file_new_for_path (opt_gpg_file[ii]);
          input_stream = G_INPUT_STREAM(g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            return NULL;
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) xdg_app_chain_input_stream_new (streams);

  return xdg_app_read_stream (source_stream, FALSE, error);
}

gboolean
xdg_app_builtin_build_bundle (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GInputStream) xml_in = NULL;
  const char *location;
  const char *filename;
  const char *name;
  const char *branch;
  g_autofree char *full_branch = NULL;
  g_autofree char *commit_checksum = NULL;
  GVariantBuilder metadata_builder;
  GVariantBuilder param_builder;
  g_autoptr(XdgAppXml) xml_root = NULL;
  g_autoptr(GFile) appstream_file = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GFile) xmls_dir = NULL;
  g_autofree char *appstream_basename = NULL;

  context = g_option_context_new ("LOCATION FILENAME NAME [BRANCH] - Create a single file bundle from a local repository");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 4)
    return usage_error (context, "LOCATION, FILENAME and NAME must be specified", error);

  location = argv[1];
  filename = argv[2];
  name = argv[3];

  if (argc >= 5)
    branch = argv[4];
  else
    branch = "master";

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!xdg_app_supports_bundles (repo))
    return xdg_app_fail (error, "Your version of ostree is too old to support single-file bundles");

  if (!g_file_query_exists (repofile, cancellable))
    return xdg_app_fail (error, "'%s' is not a valid repository", location);

  file = g_file_new_for_commandline_arg (filename);

  if (!xdg_app_is_valid_name (name))
    return xdg_app_fail (error, "'%s' is not a valid name", name);

  if (!xdg_app_is_valid_branch (branch))
    return xdg_app_fail (error, "'%s' is not a valid branch name", branch);

  if (opt_runtime)
    full_branch = xdg_app_build_runtime_ref (name, branch, opt_arch);
  else
    full_branch = xdg_app_build_app_ref (name, branch, opt_arch);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (!ostree_repo_resolve_rev (repo, full_branch, FALSE, &commit_checksum, error))
    return FALSE;

  if (!ostree_repo_read_commit (repo, commit_checksum, &root, NULL, NULL, error))
    return FALSE;

  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  /* We add this first in the metadata, so this will become the file
   * format header.  The first part is readable to make it easy to
   * figure out the type. The uint32 is basically a random value, but
   * it ensures we have both zero and high bits sets, so we don't get
   * sniffed as text. Also, the last 01 can be used as a version
   * later.  Furthermore, the use of an uint32 lets use detect
   * byteorder issues.
   */
  g_variant_builder_add (&metadata_builder, "{sv}", "xdg-app",
                         g_variant_new_uint32 (0xe5890001));

  g_variant_builder_add (&metadata_builder, "{sv}", "ref", g_variant_new_string (full_branch));

  metadata_file = g_file_resolve_relative_path (root, "metadata");

  keyfile = g_key_file_new ();

  in = (GInputStream*)g_file_read (metadata_file, cancellable, NULL);
  if (in != NULL)
    {
      g_autoptr(GBytes) bytes = xdg_app_read_stream (in, TRUE, error);

      if (bytes == NULL)
        return FALSE;

      if (!g_key_file_load_from_data (keyfile,
                                      g_bytes_get_data (bytes, NULL),
                                      g_bytes_get_size (bytes),
                                      G_KEY_FILE_NONE, error))
        return FALSE;

      g_variant_builder_add (&metadata_builder, "{sv}", "metadata",
                             g_variant_new_string (g_bytes_get_data (bytes, NULL)));
    }

  xmls_dir = g_file_resolve_relative_path (root, "files/share/app-info/xmls");
  appstream_basename = g_strconcat (name, ".xml.gz", NULL);
  appstream_file = g_file_get_child (xmls_dir, appstream_basename);

  xml_in = (GInputStream*)g_file_read (appstream_file, cancellable, NULL);
  if (xml_in)
    {
      g_autoptr(XdgAppXml) appstream_root = NULL;
      g_autoptr(XdgAppXml) xml_root = xdg_app_xml_parse (xml_in, TRUE,
                                                         cancellable, error);
      if (xml_root == NULL)
        return FALSE;

      appstream_root = xdg_app_appstream_xml_new ();
      if (xdg_app_appstream_xml_migrate (xml_root, appstream_root,
                                         full_branch, name, keyfile))
        {
          g_autoptr(GBytes) xml_data = xdg_app_appstream_xml_root_to_data (appstream_root, error);
          int i;
          g_autoptr(GFile) icons_dir =
            g_file_resolve_relative_path (root,
                                          "files/share/app-info/icons/xdg-app");
          const char *icon_sizes[] = { "64x64", "128x128" };
          const char *icon_sizes_key[] = { "icon-64", "icon-128" };
          g_autofree char *icon_name = g_strconcat (name, ".png", NULL);

          if (xml_data == NULL)
            return FALSE;

          g_variant_builder_add (&metadata_builder, "{sv}", "appdata",
                                 g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING,
                                                           xml_data, TRUE));

          for (i = 0; i < G_N_ELEMENTS (icon_sizes); i++)
            {
              g_autoptr(GFile) size_dir =g_file_get_child (icons_dir, icon_sizes[i]);
              g_autoptr(GFile) icon_file = g_file_get_child (size_dir, icon_name);
              g_autoptr(GInputStream) png_in = NULL;

              png_in = (GInputStream*)g_file_read (icon_file, cancellable, NULL);
              if (png_in != NULL)
                {
                  g_autoptr(GBytes) png_data = xdg_app_read_stream (png_in, FALSE, error);
                  if (png_data == NULL)
                    return FALSE;

                  g_variant_builder_add (&metadata_builder, "{sv}", icon_sizes_key[i],
                                         g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING,
                                                                   png_data, TRUE));
                }
            }
        }

    }

  if (opt_repo_url)
    g_variant_builder_add (&metadata_builder, "{sv}", "origin", g_variant_new_string (opt_repo_url));

  if (opt_gpg_file != NULL)
    {
      gpg_data = read_gpg_data (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  if (gpg_data)
    g_variant_builder_add (&metadata_builder, "{sv}", "gpg-keys",
                           g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                      g_bytes_get_data (gpg_data, NULL),
                                                      g_bytes_get_size (gpg_data),
                                                      1));

  g_variant_builder_init (&param_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&param_builder, "{sv}", "min-fallback-size", g_variant_new_uint32 (0));
  g_variant_builder_add (&param_builder, "{sv}", "compression", g_variant_new_byte ('x'));
  g_variant_builder_add (&param_builder, "{sv}", "bsdiff-enabled", g_variant_new_boolean (FALSE));
  g_variant_builder_add (&param_builder, "{sv}", "inline-parts", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&param_builder, "{sv}", "include-detached", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&param_builder, "{sv}", "filename", g_variant_new_bytestring (gs_file_get_path_cached (file)));

  if (!ostree_repo_static_delta_generate (repo,
                                          OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY,
                                          /* from */ NULL,
                                          commit_checksum,
                                          g_variant_builder_end (&metadata_builder),
                                          g_variant_builder_end (&param_builder),
                                          cancellable,
                                          error))
    return FALSE;

  return TRUE;
}
