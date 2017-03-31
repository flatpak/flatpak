/* builder-source.c
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

#include "builder-utils.h"
#include "builder-source.h"
#include "builder-source-archive.h"
#include "builder-source-patch.h"
#include "builder-source-git.h"
#include "builder-source-bzr.h"
#include "builder-source-file.h"
#include "builder-source-script.h"
#include "builder-source-shell.h"

static void serializable_iface_init (JsonSerializableIface *serializable_iface);

G_DEFINE_TYPE_WITH_CODE (BuilderSource, builder_source, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (JSON_TYPE_SERIALIZABLE, serializable_iface_init));

enum {
  PROP_0,
  PROP_DEST,
  PROP_ONLY_ARCHES,
  PROP_SKIP_ARCHES,
  LAST_PROP
};


static void
builder_source_finalize (GObject *object)
{
  BuilderSource *self = BUILDER_SOURCE (object);

  g_free (self->dest);

  G_OBJECT_CLASS (builder_source_parent_class)->finalize (object);
}

static void
builder_source_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  BuilderSource *self = BUILDER_SOURCE (object);

  switch (prop_id)
    {
    case PROP_DEST:
      g_value_set_string (value, self->dest);
      break;

    case PROP_ONLY_ARCHES:
      g_value_set_boxed (value, self->only_arches);
      break;

    case PROP_SKIP_ARCHES:
      g_value_set_boxed (value, self->skip_arches);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  BuilderSource *self = BUILDER_SOURCE (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_DEST:
      g_free (self->dest);
      self->dest = g_value_dup_string (value);
      break;

    case PROP_ONLY_ARCHES:
      tmp = self->only_arches;
      self->only_arches = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_SKIP_ARCHES:
      tmp = self->skip_arches;
      self->skip_arches = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}
static gboolean
builder_source_real_show_deps (BuilderSource  *self,
                               GError        **error)
{
  return TRUE;
}

static gboolean
builder_source_real_download (BuilderSource  *self,
                              gboolean        update_vcs,
                              BuilderContext *context,
                              GError        **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Download not implemented for type %s", g_type_name_from_instance ((GTypeInstance *) self));
  return FALSE;
}

static gboolean
builder_source_real_extract (BuilderSource  *self,
                             GFile          *dest,
                             BuilderOptions *build_options,
                             BuilderContext *context,
                             GError        **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Extract not implemented for type %s", g_type_name_from_instance ((GTypeInstance *) self));
  return FALSE;
}

static gboolean
builder_source_real_bundle (BuilderSource  *self,
                            BuilderContext *context,
                            GError        **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Bundle not implemented for type %s", g_type_name_from_instance ((GTypeInstance *) self));
  return FALSE;
}

static gboolean
builder_source_real_update (BuilderSource  *self,
                            BuilderContext *context,
                            GError        **error)
{
  return TRUE;
}

static void
builder_source_class_init (BuilderSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_source_finalize;
  object_class->get_property = builder_source_get_property;
  object_class->set_property = builder_source_set_property;

  klass->show_deps = builder_source_real_show_deps;
  klass->download = builder_source_real_download;
  klass->extract = builder_source_real_extract;
  klass->bundle = builder_source_real_bundle;
  klass->update = builder_source_real_update;

  g_object_class_install_property (object_class,
                                   PROP_DEST,
                                   g_param_spec_string ("dest",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ONLY_ARCHES,
                                   g_param_spec_boxed ("only-arches",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SKIP_ARCHES,
                                   g_param_spec_boxed ("skip-arches",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
}

static void
builder_source_init (BuilderSource *self)
{
}

static void
serializable_iface_init (JsonSerializableIface *serializable_iface)
{
}

JsonNode *
builder_source_to_json (BuilderSource *self)
{
  JsonNode *node;
  JsonObject *object;
  const gchar *type = NULL;

  node = json_gobject_serialize (G_OBJECT (self));
  object = json_node_get_object (node);

  if (BUILDER_IS_SOURCE_ARCHIVE (self))
    type = "archive";
  else if (BUILDER_IS_SOURCE_FILE (self))
    type = "file";
  else if (BUILDER_IS_SOURCE_SCRIPT (self))
    type = "script";
  else if (BUILDER_IS_SOURCE_SHELL (self))
    type = "shell";
  else if (BUILDER_IS_SOURCE_PATCH (self))
    type = "patch";
  else if (BUILDER_IS_SOURCE_GIT (self))
    type = "git";
  else if (BUILDER_IS_SOURCE_BZR (self))
    type = "bzr";
  else
    g_warning ("Unknown source type");

  if (type)
    json_object_set_string_member (object, "type", type);

  return node;
}

BuilderSource *
builder_source_from_json (JsonNode *node)
{
  JsonObject *object = json_node_get_object (node);
  const gchar *type;

  type = json_object_get_string_member (object, "type");

  if (type == NULL)
    g_warning ("Missing source type");
  else if (strcmp (type, "archive") == 0)
    return (BuilderSource *) json_gobject_deserialize (BUILDER_TYPE_SOURCE_ARCHIVE, node);
  else if (strcmp (type, "file") == 0)
    return (BuilderSource *) json_gobject_deserialize (BUILDER_TYPE_SOURCE_FILE, node);
  else if (strcmp (type, "script") == 0)
    return (BuilderSource *) json_gobject_deserialize (BUILDER_TYPE_SOURCE_SCRIPT, node);
  else if (strcmp (type, "shell") == 0)
    return (BuilderSource *) json_gobject_deserialize (BUILDER_TYPE_SOURCE_SHELL, node);
  else if (strcmp (type, "patch") == 0)
    return (BuilderSource *) json_gobject_deserialize (BUILDER_TYPE_SOURCE_PATCH, node);
  else if (strcmp (type, "git") == 0)
    return (BuilderSource *) json_gobject_deserialize (BUILDER_TYPE_SOURCE_GIT, node);
  else if (strcmp (type, "bzr") == 0)
    return (BuilderSource *) json_gobject_deserialize (BUILDER_TYPE_SOURCE_BZR, node);
  else
    g_warning ("Unknown source type %s", type);

  return NULL;
}

gboolean
builder_source_show_deps (BuilderSource  *self,
                          GError        **error)
{
  BuilderSourceClass *class;

  class = BUILDER_SOURCE_GET_CLASS (self);

  return class->show_deps (self, error);
}

gboolean
builder_source_download (BuilderSource  *self,
                         gboolean        update_vcs,
                         BuilderContext *context,
                         GError        **error)
{
  BuilderSourceClass *class;

  class = BUILDER_SOURCE_GET_CLASS (self);

  return class->download (self, update_vcs, context, error);
}

gboolean
builder_source_extract (BuilderSource  *self,
                        GFile          *dest,
                        BuilderOptions *build_options,
                        BuilderContext *context,
                        GError        **error)
{
  BuilderSourceClass *class;

  g_autoptr(GFile) real_dest = NULL;

  class = BUILDER_SOURCE_GET_CLASS (self);

  if (self->dest != NULL)
    {
      real_dest = g_file_resolve_relative_path (dest, self->dest);

      if (!g_file_query_exists (real_dest, NULL) &&
          !g_file_make_directory_with_parents (real_dest, NULL, error))
        return FALSE;
    }
  else
    {
      real_dest = g_object_ref (dest);
    }


  return class->extract (self, real_dest, build_options, context, error);
}

gboolean
builder_source_bundle (BuilderSource  *self,
                       BuilderContext *context,
                       GError        **error)
{
  BuilderSourceClass *class;
  class = BUILDER_SOURCE_GET_CLASS (self);

  return class->bundle (self, context, error);
}

gboolean
builder_source_update (BuilderSource  *self,
                       BuilderContext *context,
                       GError        **error)
{
  BuilderSourceClass *class = BUILDER_SOURCE_GET_CLASS (self);

  return class->update (self, context, error);
}

void
builder_source_checksum (BuilderSource  *self,
                         BuilderCache   *cache,
                         BuilderContext *context)
{
  BuilderSourceClass *class;

  class = BUILDER_SOURCE_GET_CLASS (self);

  builder_cache_checksum_str (cache, self->dest);
  builder_cache_checksum_strv (cache, self->only_arches);
  builder_cache_checksum_strv (cache, self->skip_arches);

  class->checksum (self, cache, context);
}

gboolean
builder_source_is_enabled (BuilderSource *self,
                           BuilderContext *context)
{
  if (self->only_arches != NULL &&
      self->only_arches[0] != NULL &&
      !g_strv_contains ((const char * const *) self->only_arches, builder_context_get_arch (context)))
    return FALSE;

  if (self->skip_arches != NULL &&
      g_strv_contains ((const char * const *)self->skip_arches, builder_context_get_arch (context)))
    return FALSE;

  return TRUE;
}
