/* builder-source-file.c
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

#include "builder-utils.h"
#include "builder-source-file.h"

struct BuilderSourceFile {
  BuilderSource parent;

  char *path;
  char *dest_filename;
};

typedef struct {
  BuilderSourceClass parent_class;
} BuilderSourceFileClass;

G_DEFINE_TYPE (BuilderSourceFile, builder_source_file, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_PATH,
  PROP_DEST_FILENAME,
  LAST_PROP
};

static void
builder_source_file_finalize (GObject *object)
{
  BuilderSourceFile *self = (BuilderSourceFile *)object;

  g_free (self->path);
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

    case PROP_DEST_FILENAME:
      g_free (self->dest_filename);
      self->dest_filename = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static GFile *
get_source_file (BuilderSourceFile *self,
                 BuilderContext *context,
                 GError **error)
{
  GFile *base_dir = builder_context_get_base_dir (context);

  if (self->path == NULL || self->path[0] == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "path not specified");
      return NULL;
    }

  return g_file_resolve_relative_path (base_dir, self->path);
}

static gboolean
builder_source_file_download (BuilderSource *source,
                              BuilderContext *context,
                              GError **error)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (source);
  g_autoptr(GFile) src = NULL;

  src = get_source_file (self, context, error);
  if (src == NULL)
    return FALSE;

  if (!g_file_query_exists (src, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find file at %s", self->path);
      return FALSE;
    }

  return TRUE;
}

static gboolean
builder_source_file_extract (BuilderSource *source,
                             GFile *dest,
                             BuilderContext *context,
                             GError **error)
{
  BuilderSourceFile *self = BUILDER_SOURCE_FILE (source);
  g_autoptr(GFile) src = NULL;
  g_autoptr(GFile) dest_file = NULL;
  g_autofree char *dest_filename = NULL;

  src = get_source_file (self, context, error);
  if (src == NULL)
    return FALSE;

  if (self->dest_filename)
    dest_filename = g_strdup (self->dest_filename);
  else
    dest_filename = g_file_get_basename (src);

  dest_file = g_file_get_child (dest, dest_filename);

  if (!g_file_copy (src, dest_file,
                    G_FILE_COPY_OVERWRITE,
                    NULL,
                    NULL, NULL,
                    error))
    return FALSE;

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

  src = get_source_file (self, context, NULL);
  if (src == NULL)
    return;

  if (g_file_load_contents (src, NULL, &data, &len, NULL, NULL))
    builder_cache_checksum_data (cache, (guchar *)data, len);

  builder_cache_checksum_str (cache, self->path);
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
  source_class->checksum = builder_source_file_checksum;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
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
