/* builder-source-file.c
 *
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
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

#include "flatpak-utils.h"

#include "builder-utils.h"
#include "builder-source-file.h"

struct BuilderSourceFile
{
  BuilderSource parent;

  char         *path;
  char         *url;
  char         *sha256;
  char         *dest_filename;
};

typedef struct
{
  BuilderSourceClass parent_class;
} BuilderSourceFileClass;

G_DEFINE_TYPE (BuilderSourceFile, builder_source_file, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_PATH,
  PROP_URL,
  PROP_SHA256,
  PROP_DEST_FILENAME,
  LAST_PROP
};

static void
builder_source_file_finalize (GObject *object)
{
  BuilderSourceFile *self = (BuilderSourceFile *) object;

  g_free (self->path);
  g_free (self->url);
  g_free (self->sha256);
  g_free (self->dest_filename);

  G_OBJECT_CLASS (builder_source_file_parent_class)->finalize (object);
}

static void
builder_source_file_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_string (value, self->path);
      break;

    case PROP_URL:
      g_value_set_string (value, self->url);
      break;

    case PROP_SHA256:
      g_value_set_string (value, self->sha256);
      break;

    case PROP_DEST_FILENAME:
      g_value_set_string (value, self->dest_filename);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_file_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_free (self->path);
      self->path = g_value_dup_string (value);
      break;

    case PROP_URL:
      g_free (self->url);
      self->url = g_value_dup_string (value);
      break;

    case PROP_SHA256:
      g_free (self->sha256);
      self->sha256 = g_value_dup_string (value);
      break;

    case PROP_DEST_FILENAME:
      g_free (self->dest_filename);
      self->dest_filename = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static SoupURI *
get_uri (BuilderSourceFile *self,
         GError           **error)
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
get_download_location (BuilderSourceFile *self,
                       gboolean          *is_inline,
                       BuilderContext    *context,
                       GError           **error)
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

  if (g_str_has_prefix (self->url, "data:"))
    {
      *is_inline = TRUE;
      return g_file_new_for_path ("inline data");
    }

  base_name = g_path_get_basename (path);

  if (self->sha256 == NULL || *self->sha256 == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Sha256 not specified");
      return FALSE;
    }

  download_dir = builder_context_get_download_dir (context);
  sha256_dir = g_file_get_child (download_dir, self->sha256);
  file = g_file_get_child (sha256_dir, base_name);

  *is_inline = FALSE;
  return g_steal_pointer (&file);
}

static GFile *
get_source_file (BuilderSourceFile *self,
                 BuilderContext    *context,
                 gboolean          *is_local,
                 gboolean          *is_inline,
                 GError           **error)
{
  GFile *base_dir = builder_context_get_base_dir (context);

  if (self->url != NULL && self->url[0] != 0)
    {
      *is_local = FALSE;
      return get_download_location (self, is_inline, context, error);
    }

  if (self->path != NULL && self->path[0] != 0)
    {
      *is_local = TRUE;
      *is_inline = FALSE;
      return g_file_resolve_relative_path (base_dir, self->path);
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "source file path or url not specified");
  return NULL;
}

static GBytes *
download_uri (const char     *url,
              BuilderContext *context,
              GError        **error)
{
  SoupSession *session;

  g_autoptr(SoupRequest) req = NULL;
  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GOutputStream) out = NULL;

  session = builder_context_get_soup_session (context);

  req = soup_session_request (session, url, error);
  if (req == NULL)
    return NULL;

  input = soup_request_send (req, NULL, error);
  if (input == NULL)
    return NULL;

  out = g_memory_output_stream_new_resizable ();
  if (!g_output_stream_splice (out,
                               input,
                               G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                               NULL,
                               error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
}

static gboolean
builder_source_file_download (BuilderSource  *source,
                              gboolean        update_vcs,
                              BuilderContext *context,
                              GError        **error)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (source);

  g_autoptr(GFile) file = NULL;
  gboolean is_local, is_inline;
  g_autoptr(GFile) dir = NULL;
  g_autofree char *dir_path = NULL;
  g_autofree char *sha256 = NULL;
  g_autofree char *base_name = NULL;
  g_autoptr(GBytes) content = NULL;

  file = get_source_file (self, context, &is_local, &is_inline, error);
  if (file == NULL)
    return FALSE;

  base_name = g_file_get_basename (file);

  if (g_file_query_exists (file, NULL))
    {
      if (is_local && self->sha256 != NULL && *self->sha256 != 0)
        {
          g_autofree char *data = NULL;
          gsize len;

          if (!g_file_load_contents (file, NULL, &data, &len, NULL, error))
            return FALSE;

          sha256 = g_compute_checksum_for_string (G_CHECKSUM_SHA256, data, len);
          if (strcmp (sha256, self->sha256) != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Wrong sha256 for %s, expected %s, was %s", base_name, self->sha256, sha256);
              return FALSE;
            }
        }
      return TRUE;
    }

  if (is_local)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find file at %s", self->path);
      return FALSE;
    }

  content = download_uri (self->url,
                          context,
                          error);
  if (content == NULL)
    return FALSE;

  sha256 = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
                                          g_bytes_get_data (content, NULL),
                                          g_bytes_get_size (content));

  /* sha256 is optional for inline data */
  if (((self->sha256 != NULL && *self->sha256 != 0) || !is_inline) &&
      strcmp (sha256, self->sha256) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Wrong sha256 for %s, expected %s, was %s", base_name, self->sha256, sha256);
      return FALSE;
    }

  dir = g_file_get_parent (file);
  dir_path = g_file_get_path (dir);
  g_mkdir_with_parents (dir_path, 0755);

  if (!g_file_replace_contents (file,
                                g_bytes_get_data (content, NULL),
                                g_bytes_get_size (content),
                                NULL, FALSE, G_FILE_CREATE_NONE, NULL,
                                NULL, error))
    return FALSE;

  return TRUE;
}

static gboolean
builder_source_file_extract (BuilderSource  *source,
                             GFile          *dest,
                             BuilderOptions *build_options,
                             BuilderContext *context,
                             GError        **error)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (source);

  g_autoptr(GFile) src = NULL;
  g_autoptr(GFile) dest_file = NULL;
  g_autofree char *dest_filename = NULL;
  gboolean is_local, is_inline;

  src = get_source_file (self, context, &is_local, &is_inline, error);
  if (src == NULL)
    return FALSE;

  if (self->dest_filename)
    {
      dest_filename = g_strdup (self->dest_filename);
    }
  else
    {
      if (is_inline)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No dest-filename set for inline file data");
          return FALSE;
        }
      dest_filename = g_file_get_basename (src);
    }

  dest_file = g_file_get_child (dest, dest_filename);

  /* If the destination file exists, just delete it. We can encounter errors when
   * trying to overwrite files that are not writable.
   */
  if (g_file_query_exists (dest_file, NULL) && !g_file_delete (dest_file, NULL, error))
    return FALSE;

  if (is_inline)
    {
      g_autoptr(GBytes) content = NULL;

      content = download_uri (self->url,
                              context,
                              error);
      if (content == NULL)
        return FALSE;

      if (!g_file_replace_contents (dest_file,
                                    g_bytes_get_data (content, NULL),
                                    g_bytes_get_size (content),
                                    NULL, FALSE, G_FILE_CREATE_NONE, NULL,
                                    NULL, error))
        return FALSE;
    }
  else
    {
      if (is_local)
        {
          g_autofree char *data = NULL;
          g_autofree char *base64 = NULL;
          gsize len;

          if (!g_file_load_contents (src, NULL, &data, &len, NULL, error))
            return FALSE;

          base64 = g_base64_encode ((const guchar *) data, len);
          g_free (self->url);
          self->url = g_strdup_printf ("data:text/plain;charset=utf8;base64,%s", base64);
          if (self->dest_filename == NULL || *self->dest_filename == 0)
            {
              g_free (self->dest_filename);
              self->dest_filename = g_file_get_basename (src);
            }
        }

      if (!g_file_copy (src, dest_file,
                        G_FILE_COPY_OVERWRITE,
                        NULL,
                        NULL, NULL,
                        error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
builder_source_file_update (BuilderSource  *source,
                            BuilderContext *context,
                            GError        **error)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (source);

  g_autoptr(GFile) src = NULL;
  gboolean is_local, is_inline;

  src = get_source_file (self, context, &is_local, &is_inline, error);
  if (src == NULL)
    return FALSE;

  if (is_local)
    {
      g_autofree char *data = NULL;
      g_autofree char *base64 = NULL;
      gsize len;

      if (!g_file_load_contents (src, NULL, &data, &len, NULL, error))
        return FALSE;

      base64 = g_base64_encode ((const guchar *) data, len);
      g_free (self->url);
      self->url = g_strdup_printf ("data:text/plain;charset=utf8;base64,%s", base64);
      if (self->dest_filename == NULL || *self->dest_filename == 0)
        {
          g_free (self->dest_filename);
          self->dest_filename = g_file_get_basename (src);
        }
    }

  return TRUE;
}

static void
builder_source_file_checksum (BuilderSource  *source,
                              BuilderCache   *cache,
                              BuilderContext *context)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (source);

  g_autoptr(GFile) src = NULL;
  g_autofree char *data = NULL;
  gsize len;
  gboolean is_local, is_inline;

  src = get_source_file (self, context, &is_local, &is_inline, NULL);
  if (src == NULL)
    return;

  if (is_local &&
      g_file_load_contents (src, NULL, &data, &len, NULL, NULL))
    builder_cache_checksum_data (cache, (guchar *) data, len);

  builder_cache_checksum_str (cache, self->path);
  builder_cache_checksum_str (cache, self->url);
  builder_cache_checksum_str (cache, self->sha256);
  builder_cache_checksum_str (cache, self->dest_filename);
}

static void
builder_source_file_class_init (BuilderSourceFileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_file_finalize;
  object_class->get_property = builder_source_file_get_property;
  object_class->set_property = builder_source_file_set_property;

  source_class->download = builder_source_file_download;
  source_class->extract = builder_source_file_extract;
  source_class->update = builder_source_file_update;
  source_class->checksum = builder_source_file_checksum;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
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
                                   PROP_DEST_FILENAME,
                                   g_param_spec_string ("dest-filename",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_source_file_init (BuilderSourceFile *self)
{
}
