/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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

#include <glib/gi18n.h>

#include <gio/gunixinputstream.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-chain-input-stream.h"

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

static char *opt_arch;
static char *opt_repo_url;
static gboolean opt_runtime = FALSE;
static char **opt_gpg_file;
static gboolean opt_oci = FALSE;

static GOptionEntry options[] = {
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Export runtime instead of app"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to bundle for"), N_("ARCH") },
  { "repo-url", 0, 0, G_OPTION_ARG_STRING, &opt_repo_url, N_("Url for repo"), N_("URL") },
  { "gpg-keys", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, N_("Add GPG key from FILE (- for stdin)"), N_("FILE") },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Export oci image instead of flatpak bundle"), NULL },

  { NULL }
};

static GBytes *
read_gpg_data (GCancellable *cancellable,
               GError      **error)
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
          input_stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            return NULL;
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) flatpak_chain_input_stream_new (streams);

  return flatpak_read_stream (source_stream, FALSE, error);
}

static gboolean
build_bundle (OstreeRepo *repo, GFile *file,
              const char *name, const char *full_branch,
              GCancellable *cancellable, GError **error)
{
  GVariantBuilder metadata_builder;
  GVariantBuilder param_builder;

  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GFile) xmls_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GFile) appstream_file = NULL;
  g_autofree char *appstream_basename = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GInputStream) xml_in = NULL;
  g_autoptr(GFile) root = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autoptr(GBytes) gpg_data = NULL;

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
  g_variant_builder_add (&metadata_builder, "{sv}", "flatpak",
                         g_variant_new_uint32 (0xe5890001));

  g_variant_builder_add (&metadata_builder, "{sv}", "ref", g_variant_new_string (full_branch));

  metadata_file = g_file_resolve_relative_path (root, "metadata");

  keyfile = g_key_file_new ();

  in = (GInputStream *) g_file_read (metadata_file, cancellable, NULL);
  if (in != NULL)
    {
      g_autoptr(GBytes) bytes = flatpak_read_stream (in, TRUE, error);

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

  xml_in = (GInputStream *) g_file_read (appstream_file, cancellable, NULL);
  if (xml_in)
    {
      g_autoptr(FlatpakXml) appstream_root = NULL;
      g_autoptr(FlatpakXml) xml_root = flatpak_xml_parse (xml_in, TRUE,
                                                          cancellable, error);
      if (xml_root == NULL)
        return FALSE;

      appstream_root = flatpak_appstream_xml_new ();
      if (flatpak_appstream_xml_migrate (xml_root, appstream_root,
                                         full_branch, name, keyfile))
        {
          g_autoptr(GBytes) xml_data = flatpak_appstream_xml_root_to_data (appstream_root, error);
          int i;
          g_autoptr(GFile) icons_dir =
            g_file_resolve_relative_path (root,
                                          "files/share/app-info/icons/flatpak");
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
              g_autoptr(GFile) size_dir = g_file_get_child (icons_dir, icon_sizes[i]);
              g_autoptr(GFile) icon_file = g_file_get_child (size_dir, icon_name);
              g_autoptr(GInputStream) png_in = NULL;

              png_in = (GInputStream *) g_file_read (icon_file, cancellable, NULL);
              if (png_in != NULL)
                {
                  g_autoptr(GBytes) png_data = flatpak_read_stream (png_in, FALSE, error);
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
    {
      g_variant_builder_add (&metadata_builder, "{sv}", "gpg-keys",
                             g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                        g_bytes_get_data (gpg_data, NULL),
                                                        g_bytes_get_size (gpg_data),
                                                        1));
    }

  g_variant_builder_init (&param_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&param_builder, "{sv}", "min-fallback-size", g_variant_new_uint32 (0));
  g_variant_builder_add (&param_builder, "{sv}", "compression", g_variant_new_byte ('x'));
  g_variant_builder_add (&param_builder, "{sv}", "bsdiff-enabled", g_variant_new_boolean (FALSE));
  g_variant_builder_add (&param_builder, "{sv}", "inline-parts", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&param_builder, "{sv}", "include-detached", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&param_builder, "{sv}", "filename", g_variant_new_bytestring (flatpak_file_get_path_cached (file)));

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

#if defined(HAVE_LIBARCHIVE) && defined(HAVE_OSTREE_EXPORT_PATH_PREFIX)

GLNX_DEFINE_CLEANUP_FUNCTION (void *, flatpak_local_free_write_archive, archive_write_free)
#define free_write_archive __attribute__((cleanup (flatpak_local_free_write_archive)))

GLNX_DEFINE_CLEANUP_FUNCTION (void *, flatpak_local_free_archive_entry, archive_entry_free)
#define free_archive_entry __attribute__((cleanup (flatpak_local_free_archive_entry)))


typedef struct
{
  GString *str;
  int      depth;
  GList   *index;
} JsonWriter;

static void
json_writer_init (JsonWriter *writer)
{
  memset (writer, 0, sizeof (*writer));
  writer->str = g_string_new ("");
  writer->index = g_list_prepend (writer->index, 0);
}

static void
json_writer_indent (JsonWriter *writer)
{
  int i;

  for (i = 0; i < writer->depth; i++)
    g_string_append (writer->str, "    ");
}

static void
json_writer_add_bool (JsonWriter *writer, gboolean val)
{
  if (val)
    g_string_append (writer->str, "true");
  else
    g_string_append (writer->str, "false");
}

static void
json_writer_add_string (JsonWriter *writer, const gchar *str)
{
  const gchar *p;

  g_string_append_c (writer->str, '"');

  for (p = str; *p != 0; p++)
    {
      if (*p == '\\' || *p == '"')
        {
          g_string_append_c (writer->str, '\\');
          g_string_append_c (writer->str, *p);
        }
      else if ((*p > 0 && *p < 0x1f) || *p == 0x7f)
        {
          switch (*p)
            {
            case '\b':
              g_string_append (writer->str, "\\b");
              break;

            case '\f':
              g_string_append (writer->str, "\\f");
              break;

            case '\n':
              g_string_append (writer->str, "\\n");
              break;

            case '\r':
              g_string_append (writer->str, "\\r");
              break;

            case '\t':
              g_string_append (writer->str, "\\t");
              break;

            default:
              g_string_append_printf (writer->str, "\\u00%02x", (guint) * p);
              break;
            }
        }
      else
        {
          g_string_append_c (writer->str, *p);
        }
    }

  g_string_append_c (writer->str, '"');
}

static void
json_writer_start_item (JsonWriter *writer)
{
  int index = GPOINTER_TO_INT (writer->index->data);

  if (index != 0)
    g_string_append (writer->str, ",\n");
  else
    g_string_append (writer->str, "\n");
  json_writer_indent (writer);
  writer->index->data = GINT_TO_POINTER (index + 1);
}

static void
json_writer_open_scope (JsonWriter *writer)
{
  writer->depth += 1;
  writer->index = g_list_prepend (writer->index, 0);
}

static void
json_writer_close_scope (JsonWriter *writer)
{
  GList *l;

  writer->depth -= 1;
  l = writer->index;
  writer->index = g_list_remove_link (writer->index, l);
  g_list_free (l);
  g_string_append (writer->str, "\n");
  json_writer_indent (writer);
}

static void
json_writer_open_struct (JsonWriter *writer)
{
  g_string_append (writer->str, "{");
  json_writer_open_scope (writer);
}

static void
json_writer_close_struct (JsonWriter *writer)
{
  int index;

  json_writer_close_scope (writer);
  g_string_append (writer->str, "}");

  /* Last newline in file */
  index = GPOINTER_TO_INT (writer->index->data);
  if (index == 0)
    g_string_append (writer->str, "\n");
}

static void
json_writer_open_array (JsonWriter *writer)
{
  g_string_append (writer->str, "[");
  json_writer_open_scope (writer);
}

static void
json_writer_close_array (JsonWriter *writer)
{
  json_writer_close_scope (writer);
  g_string_append (writer->str, "]");
}

static void
json_writer_add_property (JsonWriter *writer, const gchar *name)
{
  json_writer_start_item (writer);
  json_writer_add_string (writer, name);
  g_string_append (writer->str, ": ");
}

static void
json_writer_add_struct_property (JsonWriter *writer, const gchar *name)
{
  json_writer_add_property (writer, name);
  json_writer_open_struct (writer);
}

static void
json_writer_add_array_property (JsonWriter *writer, const gchar *name)
{
  json_writer_add_property (writer, name);
  json_writer_open_array (writer);
}

static void
json_writer_add_string_property (JsonWriter *writer, const gchar *name, const char *value)
{
  json_writer_add_property (writer, name);
  json_writer_add_string (writer, value);
}

static void
json_writer_add_bool_property (JsonWriter *writer, const gchar *name, gboolean value)
{
  json_writer_add_property (writer, name);
  json_writer_add_bool (writer, value);
}

static void
json_writer_add_array_item (JsonWriter *writer, const gchar *string)
{
  json_writer_start_item (writer);
  json_writer_add_string (writer, string);
}

static void
json_writer_add_array_struct (JsonWriter *writer)
{
  json_writer_start_item (writer);
  json_writer_open_struct (writer);
}

static gboolean
propagate_libarchive_error (GError        **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
  return FALSE;
}

struct archive_entry *
new_entry (struct archive                 *a,
           const char                     *name,
           OstreeRepoExportArchiveOptions *opts)
{
  struct archive_entry *entry = archive_entry_new2 (a);
  time_t ts = (time_t) opts->timestamp_secs;

  archive_entry_update_pathname_utf8 (entry, name);
  archive_entry_set_ctime (entry, ts, 0);
  archive_entry_set_mtime (entry, ts, 0);
  archive_entry_set_atime (entry, ts, 0);
  archive_entry_set_uid (entry, 0);
  archive_entry_set_gid (entry, 0);

  return entry;
}


static gboolean
add_dir (struct archive                 *a,
         const char                     *name,
         OstreeRepoExportArchiveOptions *opts,
         GError                        **error)
{
  g_autofree char *full_name = g_build_filename ("rootfs", name, NULL);

  free_archive_entry struct archive_entry *entry = new_entry (a, full_name, opts);

  archive_entry_set_mode (entry, AE_IFDIR | 0755);

  if (archive_write_header (a, entry) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  return TRUE;
}

static gboolean
add_symlink (struct archive                 *a,
             const char                     *name,
             const char                     *target,
             OstreeRepoExportArchiveOptions *opts,
             GError                        **error)
{
  g_autofree char *full_name = g_build_filename ("rootfs", name, NULL);

  free_archive_entry struct archive_entry *entry = new_entry (a, full_name, opts);

  archive_entry_set_mode (entry, AE_IFLNK | 0755);
  archive_entry_set_symlink (entry, target);

  if (archive_write_header (a, entry) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  return TRUE;
}

static gboolean
add_file (struct archive                 *a,
          const char                     *name,
          OstreeRepo                     *repo,
          GFile                          *file,
          OstreeRepoExportArchiveOptions *opts,
          GCancellable                   *cancellable,
          GError                        **error)
{
  free_archive_entry struct archive_entry *entry = new_entry (a, name, opts);
  guint8 buf[8192];
  g_autoptr(GInputStream) file_in = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  const char *checksum;

  checksum = ostree_repo_file_get_checksum ((OstreeRepoFile *) file);

  if (!ostree_repo_load_file (repo, checksum, &file_in, &file_info, NULL,
                              cancellable, error))
    return FALSE;

  archive_entry_set_uid (entry, g_file_info_get_attribute_uint32 (file_info, "unix::uid"));
  archive_entry_set_gid (entry, g_file_info_get_attribute_uint32 (file_info, "unix::gid"));
  archive_entry_set_mode (entry, g_file_info_get_attribute_uint32 (file_info, "unix::mode"));
  archive_entry_set_size (entry, g_file_info_get_size (file_info));

  if (archive_write_header (a, entry) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  while (TRUE)
    {
      ssize_t r;
      gssize bytes_read = g_input_stream_read (file_in, buf, sizeof (buf),
                                               cancellable, error);
      if (bytes_read < 0)
        return FALSE;
      if (bytes_read == 0)
        break;

      r = archive_write_data (a, buf, bytes_read);
      if (r != bytes_read)
        return propagate_libarchive_error (error, a);
    }

  if (archive_write_finish_entry (a) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  return TRUE;
}

static gboolean
add_file_from_data (struct archive                 *a,
                    const char                     *name,
                    OstreeRepo                     *repo,
                    const char                     *data,
                    gsize                           size,
                    OstreeRepoExportArchiveOptions *opts,
                    GCancellable                   *cancellable,
                    GError                        **error)
{
  free_archive_entry struct archive_entry *entry = new_entry (a, name, opts);
  ssize_t r;

  archive_entry_set_mode (entry, AE_IFREG | 0755);
  archive_entry_set_size (entry, size);

  if (archive_write_header (a, entry) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  r = archive_write_data (a, data, size);
  if (r != size)
    return propagate_libarchive_error (error, a);

  if (archive_write_finish_entry (a) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  return TRUE;
}

static const char *
get_oci_arch (const char *arch)
{
  if (strcmp (arch, "x86_64") == 0)
    return "amd64";
  if (strcmp (arch, "aarch64") == 0)
    return "arm64";
  return arch;
}

static GString *
generate_config_json (const char *arch)
{
  JsonWriter writer;

  json_writer_init (&writer);

  json_writer_open_struct (&writer);
  json_writer_add_string_property (&writer, "ociVersion", "0.5.0");
  json_writer_add_struct_property (&writer, "platform");
  {
    json_writer_add_string_property (&writer, "os", "linux");
    json_writer_add_string_property (&writer, "arch", get_oci_arch (arch));
    json_writer_close_struct (&writer);
  }
  json_writer_add_struct_property (&writer, "process");
  {
    json_writer_add_bool_property (&writer, "terminal", TRUE);
    json_writer_add_array_property (&writer, "args");
    json_writer_add_array_item (&writer, "/bin/sh");
    json_writer_close_array (&writer);
    json_writer_add_array_property (&writer, "envs");
    json_writer_add_array_item (&writer, "PATH=/app/bin:/usr/bin");
    json_writer_add_array_item (&writer, "LD_LIBRARY_PATH=/app/lib");
    json_writer_add_array_item (&writer, "XDG_CONFIG_DIRS=/app/etc/xdg:/etc/xdg");
    json_writer_add_array_item (&writer, "XDG_DATA_DIRS=/app/share:/usr/share");
    json_writer_add_array_item (&writer, "SHELL=/bin/sh");
    json_writer_close_array (&writer);
    json_writer_add_string_property (&writer, "cwd", "/");
    json_writer_add_bool_property (&writer, "noNewPrivileges", TRUE);
    json_writer_close_struct (&writer);
  }
  json_writer_add_struct_property (&writer, "root");
  {
    json_writer_add_string_property (&writer, "path", "rootfs");
    json_writer_add_bool_property (&writer, "readonly", TRUE);
    json_writer_close_struct (&writer);
  }
  json_writer_add_array_property (&writer, "mounts");
  {
    json_writer_add_array_struct (&writer);
    {
      json_writer_add_string_property (&writer, "destination", "/proc");
      json_writer_add_string_property (&writer, "type", "proc");
      json_writer_add_string_property (&writer, "source", "proc");
      json_writer_close_struct (&writer);
    }
    json_writer_add_array_struct (&writer);
    {
      json_writer_add_string_property (&writer, "destination", "/sys");
      json_writer_add_string_property (&writer, "type", "sysfs");
      json_writer_add_string_property (&writer, "source", "sysfs");
      json_writer_add_array_property (&writer, "options");
      {
        json_writer_add_array_item (&writer, "nosuid");
        json_writer_add_array_item (&writer, "noexec");
        json_writer_add_array_item (&writer, "nodev");
        json_writer_close_array (&writer);
      }
      json_writer_close_struct (&writer);
    }
    json_writer_add_array_struct (&writer);
    {
      json_writer_add_string_property (&writer, "destination", "/dev");
      json_writer_add_string_property (&writer, "type", "tmpfs");
      json_writer_add_string_property (&writer, "source", "tmpfs");
      json_writer_add_array_property (&writer, "options");
      {
        json_writer_add_array_item (&writer, "nosuid");
        json_writer_close_array (&writer);
      }
      json_writer_close_struct (&writer);
    }
    json_writer_close_array (&writer);
  }

  json_writer_add_struct_property (&writer, "linux");
  {
    json_writer_add_string_property (&writer, "rootfsPropagation", "slave");
    json_writer_add_struct_property (&writer, "resources");
    {
      json_writer_close_struct (&writer);
    }

    json_writer_add_array_property (&writer, "namespaces");
    {
      json_writer_add_array_struct (&writer);
      {
        json_writer_add_string_property (&writer, "type", "pid");
        json_writer_close_struct (&writer);
      }
      json_writer_add_array_struct (&writer);
      {
        json_writer_add_string_property (&writer, "type", "mount");
        json_writer_close_struct (&writer);
      }
      json_writer_close_array (&writer);
    }

    json_writer_close_struct (&writer);
  }

  json_writer_add_struct_property (&writer, "annotations");
  {
    json_writer_close_struct (&writer);
  }

  json_writer_close_struct (&writer);

  return writer.str;
}

#endif

static gboolean
build_oci (OstreeRepo *repo, GFile *file,
           const char *name, const char *full_branch,
           GCancellable *cancellable, GError **error)
{
#if !defined(HAVE_OSTREE_EXPORT_PATH_PREFIX)
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is to old to support OCI exports");
  return FALSE;
#elif !defined(HAVE_LIBARCHIVE)
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of flatpak is not compiled with libarchive support");
  return FALSE;
#else
  free_write_archive struct archive *a = NULL;
  OstreeRepoExportArchiveOptions opts = { 0, };
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) export = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autoptr(GString) str = g_string_new ("");
  g_auto(GStrv) ref_parts = g_strsplit (full_branch, "/", -1);

  if (!ostree_repo_resolve_rev (repo, full_branch, FALSE, &commit_checksum, error))
    return FALSE;

  if (!ostree_repo_read_commit (repo, commit_checksum, &root, NULL, NULL, error))
    return FALSE;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit_data, error))
    return FALSE;

  if (!ostree_repo_read_commit_detached_metadata (repo, commit_checksum, &commit_metadata, cancellable, error))
    return FALSE;

  a = archive_write_new ();

  if (archive_write_set_format_gnutar (a) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  if (archive_write_add_filter_none (a) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  if (archive_write_open_filename (a, flatpak_file_get_path_cached (file)) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  opts.timestamp_secs = ostree_commit_get_timestamp (commit_data);

  files = g_file_get_child (root, "files");
  export = g_file_get_child (root, "export");
  metadata = g_file_get_child (root, "metadata");

  if (opt_runtime)
    opts.path_prefix = "rootfs/usr/";
  else
    opts.path_prefix = "rootfs/app/";

  {
    const char *root_dirs[] = { "dev", "home", "proc", "run", "sys", "tmp", "var", "opt", "srv", "media", "mnt" };
    const char *root_symlinks[] = {
      "etc", "usr/etc",
      "lib", "usr/lib",
      "lib64", "usr/lib64",
      "lib32", "usr/lib32",
      "bin", "usr/bin",
      "sbin", "usr/sbin",
      "var/tmp", "/tmp",
      "var/run", "/run",
    };
    int i;

    /* Add the "other" of /app & /usr */
    if (!add_dir (a, opt_runtime ? "app" : "usr", &opts, error))
      return FALSE;

    for (i = 0; i < G_N_ELEMENTS (root_dirs); i++)
      if (!add_dir (a, root_dirs[i], &opts, error))
        return FALSE;

    for (i = 0; i < G_N_ELEMENTS (root_symlinks); i += 2)
      if (!add_symlink (a, root_symlinks[i], root_symlinks[i + 1], &opts, error))
        return FALSE;
  }

  if (!ostree_repo_export_tree_to_archive (repo, &opts, (OstreeRepoFile *) files, a,
                                           cancellable, error))
    return FALSE;

  if (!opt_runtime && g_file_query_exists (export, NULL))
    {
      opts.path_prefix = "rootfs/export/";
      if (!ostree_repo_export_tree_to_archive (repo, &opts, (OstreeRepoFile *) export, a,
                                               cancellable, error))
        return FALSE;
    }

  opts.path_prefix = NULL;
  if (!add_file (a, "rootfs/metadata", repo, metadata, &opts, cancellable, error))
    return FALSE;

  if (!add_file_from_data (a, "rootfs/ref",
                           repo,
                           full_branch,
                           strlen (full_branch),
                           &opts, cancellable, error))
    return FALSE;

  if (!add_file_from_data (a, "rootfs/commit",
                           repo,
                           g_variant_get_data (commit_data),
                           g_variant_get_size (commit_data),
                           &opts, cancellable, error))
    return FALSE;

  if (commit_metadata != NULL)
    {
      if (!add_file_from_data (a, "rootfs/commitmeta",
                               repo,
                               g_variant_get_data (commit_metadata),
                               g_variant_get_size (commit_metadata),
                               &opts, cancellable, error))
        return FALSE;
    }

  str = generate_config_json (ref_parts[2]);
  if (!add_file_from_data (a, "config.json",
                           repo,
                           str->str,
                           str->len,
                           &opts, cancellable, error))
    return FALSE;

  if (archive_write_close (a) != ARCHIVE_OK)
    return propagate_libarchive_error (error, a);

  g_print ("WARNING: the oci format produced by flatpak is experimental and unstable.\n"
           "Don't use this for anything but experiments for now\n");

  return TRUE;
#endif
}

gboolean
flatpak_builtin_build_bundle (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  const char *filename;
  const char *name;
  const char *branch;
  g_autofree char *full_branch = NULL;

  context = g_option_context_new (_("LOCATION FILENAME NAME [BRANCH] - Create a single file bundle from a local repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 4)
    return usage_error (context, _("LOCATION, FILENAME and NAME must be specified"), error);

  location = argv[1];
  filename = argv[2];
  name = argv[3];

  if (argc >= 5)
    branch = argv[4];
  else
    branch = "master";

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!g_file_query_exists (repofile, cancellable))
    return flatpak_fail (error, _("'%s' is not a valid repository"), location);

  file = g_file_new_for_commandline_arg (filename);

  if (!flatpak_is_valid_name (name))
    return flatpak_fail (error, _("'%s' is not a valid name"), name);

  if (!flatpak_is_valid_branch (branch))
    return flatpak_fail (error, _("'%s' is not a valid branch name"), branch);

  if (opt_runtime)
    full_branch = flatpak_build_runtime_ref (name, branch, opt_arch);
  else
    full_branch = flatpak_build_app_ref (name, branch, opt_arch);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (opt_oci)
    {
      if (!build_oci (repo, file, name, full_branch, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!build_bundle (repo, file, name, full_branch, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_build_bundle (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* LOCATION */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;

    case 2: /* FILENAME */
      flatpak_complete_file (completion);
      break;
    }

  return TRUE;
}
