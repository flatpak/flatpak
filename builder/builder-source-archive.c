/* builder-source-archive.c
 *
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statfs.h>

#include "xdg-app-utils.h"

#include "builder-utils.h"
#include "builder-source-archive.h"

struct BuilderSourceArchive {
  BuilderSource parent;

  char *url;
  char *sha256;
  guint strip_components;
};

typedef struct {
  BuilderSourceClass parent_class;
} BuilderSourceArchiveClass;

G_DEFINE_TYPE (BuilderSourceArchive, builder_source_archive, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_URL,
  PROP_SHA256,
  PROP_STRIP_COMPONENTS,
  LAST_PROP
};

typedef enum {
  UNKNOWN,
  TAR,
  TAR_GZIP,
  TAR_COMPRESS,
  TAR_BZIP2,
  TAR_LZIP,
  TAR_LZMA,
  TAR_LZOP,
  TAR_XZ,
  ZIP
} BuilderArchiveType;

gboolean
is_tar (BuilderArchiveType type)
{
  return (type >= TAR) && (type <= TAR_XZ);
}

const char *
tar_decompress_flag (BuilderArchiveType type)
{
  switch (type)
    {
    default:
    case TAR:
      return NULL;
    case TAR_GZIP:
      return "-z";
    case TAR_COMPRESS:
      return "-Z";
    case TAR_BZIP2:
      return "-j";
    case TAR_LZIP:
      return "--lzip";
    case TAR_LZMA:
      return "--lzma";
    case TAR_LZOP:
      return "--lzop";
    case TAR_XZ:
      return "-J";
    }
}

static void
builder_source_archive_finalize (GObject *object)
{
  BuilderSourceArchive *self = (BuilderSourceArchive *)object;

  g_free (self->url);
  g_free (self->sha256);

  G_OBJECT_CLASS (builder_source_archive_parent_class)->finalize (object);
}

static void
builder_source_archive_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  BuilderSourceArchive *self = BUILDER_SOURCE_ARCHIVE (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_value_set_string (value, self->url);
      break;

    case PROP_SHA256:
      g_value_set_string (value, self->sha256);
      break;

    case PROP_STRIP_COMPONENTS:
      g_value_set_uint (value, self->strip_components);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_archive_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  BuilderSourceArchive *self = BUILDER_SOURCE_ARCHIVE (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_free (self->url);
      self->url = g_value_dup_string (value);
      break;

    case PROP_SHA256:
      g_free (self->sha256);
      self->sha256 = g_value_dup_string (value);
      break;

    case PROP_STRIP_COMPONENTS:
      self->strip_components = g_value_get_uint (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static SoupURI *
get_uri (BuilderSourceArchive *self,
         GError **error)
{
  SoupURI *uri;

  if (self->url == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "URL not specified");
      return NULL;
    }

  uri = soup_uri_new (self->url);
  if (uri == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid URL '%s'", self->url);
      return NULL;
    }
  return uri;
}

static GFile *
get_download_location (BuilderSourceArchive *self,
                       BuilderContext *context,
                       GError **error)
{
  g_autoptr(SoupURI) uri = NULL;
  const char *path;
  g_autofree char *base_name = NULL;
  GFile *download_dir = NULL;
  g_autoptr(GFile) sha256_dir = NULL;
  g_autoptr(GFile) file = NULL;

  uri = get_uri (self, error);
  if (uri == NULL)
    return FALSE;

  path = soup_uri_get_path (uri);

  base_name = g_path_get_basename (path);

  if (self->sha256 == NULL || *self->sha256 == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Sha256 not specified");
      return FALSE;
    }

  download_dir = builder_context_get_download_dir (context);
  sha256_dir = g_file_get_child (download_dir, self->sha256);
  file = g_file_get_child (sha256_dir, base_name);

  return g_steal_pointer (&file);
}

static gboolean
builder_source_archive_download (BuilderSource *source,
                                 BuilderContext *context,
                                 GError **error)
{
  BuilderSourceArchive *self = BUILDER_SOURCE_ARCHIVE (source);
  g_autoptr (GFile) file = NULL;
  g_autoptr (GFile) dir = NULL;
  g_autoptr(SoupURI) uri = NULL;
  SoupSession *session;
  g_autofree char *url = NULL;
  g_autofree char *dir_path = NULL;
  g_autofree char *sha256 = NULL;
  g_autofree char *base_name = NULL;
  g_autoptr(SoupMessage) msg = NULL;

  file = get_download_location (self, context, error);
  if (file == NULL)
    return FALSE;

  if (g_file_query_exists (file, NULL))
    return TRUE;

  base_name = g_file_get_basename (file);

  uri = get_uri (self, error);
  if (uri == NULL)
    return FALSE;

  url = g_strdup (self->url);

  session = builder_context_get_soup_session (context);

  while (TRUE)
    {
      g_clear_object (&msg);
      msg = soup_message_new ("GET", url);
      g_debug ("GET %s", self->url);
      g_print ("Downloading %s...", base_name);
      soup_session_send_message (session, msg);
      g_print ("done\n");

      g_debug ("response: %d %s", msg->status_code, msg->reason_phrase);

      if (SOUP_STATUS_IS_REDIRECTION (msg->status_code))
        {
          const char *header = soup_message_headers_get_one (msg->response_headers, "Location");
          if (header)
            {
              g_autoptr(SoupURI) new_uri = soup_uri_new_with_base (soup_message_get_uri (msg), header);
              g_free (url);
              url = soup_uri_to_string (uri, FALSE);
              g_debug ("  -> %s", header);
              continue;
            }
        }
      else if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to download %s (error %d): %s", base_name, msg->status_code, msg->reason_phrase);
          return FALSE;
        }

      break; /* No redirection */
    }

  sha256 = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
                                          msg->response_body->data,
                                          msg->response_body->length);

  if (strcmp (sha256, self->sha256) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Wrong sha256 for %s, expected %s, was %s", base_name, self->sha256, sha256);
      return FALSE;
    }

  dir = g_file_get_parent (file);
  dir_path = g_file_get_path (dir);
  g_mkdir_with_parents (dir_path, 0755);

  if (!g_file_replace_contents (file,
                                msg->response_body->data,
                                msg->response_body->length,
                                NULL, FALSE, G_FILE_CREATE_NONE, NULL,
                                NULL, error))
    return FALSE;

  return TRUE;
}

static gboolean
tar (GFile *dir,
     GError **error,
     const gchar            *argv1,
     ...)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GPtrArray *args;
  const gchar *arg;
  va_list ap;

  args = g_ptr_array_new ();
  g_ptr_array_add (args, "tar");
  va_start (ap, argv1);
  g_ptr_array_add (args, (gchar *) argv1);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, (gchar *) arg);
  g_ptr_array_add (args, NULL);
  va_end (ap);

  launcher = g_subprocess_launcher_new (0);

  if (dir)
    {
      g_autofree char *path = g_file_get_path (dir);
      g_subprocess_launcher_set_cwd (launcher, path);
    }

  subp = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) args->pdata, error);
  g_ptr_array_free (args, TRUE);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  return TRUE;
}

static gboolean
unzip (GFile *dir,
       GError **error,
       const gchar            *argv1,
     ...)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GPtrArray *args;
  const gchar *arg;
  va_list ap;

  args = g_ptr_array_new ();
  g_ptr_array_add (args, "unzip");
  va_start (ap, argv1);
  g_ptr_array_add (args, (gchar *) argv1);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, (gchar *) arg);
  g_ptr_array_add (args, NULL);
  va_end (ap);

  launcher = g_subprocess_launcher_new (0);

  if (dir)
    {
      g_autofree char *path = g_file_get_path (dir);
      g_subprocess_launcher_set_cwd (launcher, path);
    }

  subp = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) args->pdata, error);
  g_ptr_array_free (args, TRUE);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  return TRUE;
}


BuilderArchiveType
get_type (GFile *archivefile)
{
  g_autofree char *base_name = NULL;
  g_autofree gchar *lower = NULL;

  base_name = g_file_get_basename (archivefile);
  lower = g_ascii_strdown (base_name, -1);

  if (g_str_has_suffix (lower, ".tar"))
    return TAR;

  if (g_str_has_suffix (lower, ".tar.gz") ||
      g_str_has_suffix (lower, ".tgz") ||
      g_str_has_suffix (lower, ".taz"))
    return TAR_GZIP;

  if (g_str_has_suffix (lower, ".tar.Z") ||
      g_str_has_suffix (lower, ".taZ"))
    return TAR_COMPRESS;

  if (g_str_has_suffix (lower, ".tar.bz2") ||
      g_str_has_suffix (lower, ".tz2") ||
      g_str_has_suffix (lower, ".tbz2") ||
      g_str_has_suffix (lower, ".tbz"))
    return TAR_BZIP2;

  if (g_str_has_suffix (lower, ".tar.lz"))
    return TAR_LZIP;

  if (g_str_has_suffix (lower, ".tar.lzma") ||
      g_str_has_suffix (lower, ".tlz"))
    return TAR_LZMA;

  if (g_str_has_suffix (lower, ".tar.lzo"))
    return TAR_LZOP;

  if (g_str_has_suffix (lower, ".tar.xz"))
    return TAR_XZ;

  if (g_str_has_suffix (lower, ".zip"))
    return ZIP;

  return UNKNOWN;
}

static gboolean
strip_components_into (GFile *dest,
                       GFile *src,
                       int level,
                       GError **error)
{
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  dir_enum = g_file_enumerate_children (src, "standard::name,standard::type",
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    return FALSE;;

  while ((child_info = g_file_enumerator_next_file (dir_enum, NULL, &temp_error)))
    {
      g_autoptr(GFile) child = NULL;
      g_autoptr(GFile) dest_child = NULL;
      g_autoptr(GFileEnumerator) dir_enum2 = NULL;
      g_autoptr(GFileInfo) child_info2 = NULL;

      child = g_file_get_child (src, g_file_info_get_name (child_info));

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          level > 0)
        {
          if (!strip_components_into (dest, child, level - 1, error))
            return FALSE;

          g_clear_object (&child_info);
          continue;
        }

      dest_child = g_file_get_child (dest, g_file_info_get_name (child_info));
      if (!g_file_move (child, dest_child, G_FILE_COPY_NONE, NULL, NULL, NULL, error))
        return FALSE;

      g_clear_object (&child_info);
      continue;
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  if (!g_file_delete (src, NULL, error))
    return FALSE;

  return TRUE;
}


static gboolean
builder_source_archive_extract (BuilderSource *source,
                                GFile *dest,
                                BuilderContext *context,
                                GError **error)
{
  BuilderSourceArchive *self = BUILDER_SOURCE_ARCHIVE (source);
  g_autoptr(GFile) archivefile = NULL;
  g_autofree char *archive_path = NULL;
  BuilderArchiveType type;

  archivefile = get_download_location (self, context, error);
  if (archivefile == NULL)
    return FALSE;

  type = get_type (archivefile);

  archive_path = g_file_get_path (archivefile);

  if (is_tar (type))
    {
      g_autofree char *strip_components = g_strdup_printf ("--strip-components=%u", self->strip_components);
      /* Note: tar_decompress_flag can return NULL, so put it last */
      if (!tar (dest, error, "xf", archive_path, strip_components, tar_decompress_flag (type), NULL))
        return FALSE;
    }
  else if (type == ZIP)
    {
      g_autoptr(GFile) zip_dest = NULL;

      if (self->strip_components > 0)
        {
          g_autoptr(GFile) tmp_dir_template = g_file_get_child (dest, ".uncompressXXXXXX");
          g_autofree char *tmp_dir_path = g_file_get_path (tmp_dir_template);;

          if (g_mkdtemp (tmp_dir_path) == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't create uncompress directory");
              return FALSE;
            }

          zip_dest = g_file_new_for_path (tmp_dir_path);
        }
      else
        zip_dest = g_object_ref (dest);

      if (!unzip (zip_dest, error, archive_path, NULL))
        return FALSE;

      if (self->strip_components > 0)
        {
          if (!strip_components_into (dest, zip_dest, self->strip_components, error))
            return FALSE;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unknown archive format of '%s'", archive_path);
      return FALSE;
    }

  return TRUE;
}

static void
builder_source_archive_checksum (BuilderSource  *source,
                                 BuilderCache   *cache,
                                 BuilderContext *context)
{
  BuilderSourceArchive *self = BUILDER_SOURCE_ARCHIVE (source);

  builder_cache_checksum_str (cache, self->url);
  builder_cache_checksum_str (cache, self->sha256);
  builder_cache_checksum_uint32 (cache, self->strip_components);
}


static void
builder_source_archive_class_init (BuilderSourceArchiveClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_archive_finalize;
  object_class->get_property = builder_source_archive_get_property;
  object_class->set_property = builder_source_archive_set_property;

  source_class->download = builder_source_archive_download;
  source_class->extract = builder_source_archive_extract;
  source_class->checksum = builder_source_archive_checksum;

  g_object_class_install_property (object_class,
                                   PROP_URL,
                                   g_param_spec_string ("url",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SHA256,
                                   g_param_spec_string ("sha256",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_STRIP_COMPONENTS,
                                   g_param_spec_uint ("strip-components",
                                                      "",
                                                      "",
                                                      0, G_MAXUINT,
                                                      1,
                                                      G_PARAM_READWRITE));
}

static void
builder_source_archive_init (BuilderSourceArchive *self)
{
  self->strip_components = 1;
}
