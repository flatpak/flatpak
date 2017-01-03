/* builder-options.c
 *
 * Copyright © 2016 Kinvolk GmbH
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
 *       Michal Rostecki <michal@kinvolk.io>
 */

#include "config.h"

#include <string.h>

#include "builder-finish-options.h"
#include "builder-context.h"

struct BuilderFinishOptions
{
  GObject     parent;

  GHashTable *arch;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderFinishOptionsClass;

static void serializable_iface_init (JsonSerializableIface *serializable_iface);

G_DEFINE_TYPE_WITH_CODE (BuilderFinishOptions, builder_finish_options, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (JSON_TYPE_SERIALIZABLE, serializable_iface_init));

enum {
  PROP_0,
  PROP_ARCH,
  LAST_PROP
};

static void
builder_finish_options_finalize (GObject *object)
{
  BuilderFinishOptions *self = (BuilderFinishOptions *) object;

  g_hash_table_destroy (self->arch);

  G_OBJECT_CLASS (builder_finish_options_parent_class)->finalize (object);
}

static void
builder_finish_options_get_property (GObject    *object,
		                     guint       prop_id,
				     GValue     *value,
				     GParamSpec *pspec)
{
  BuilderFinishOptions *self = BUILDER_FINISH_OPTIONS (object);

  switch (prop_id)
    {
    case PROP_ARCH:
      g_value_set_boxed (value, self->arch);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_finish_options_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  BuilderFinishOptions *self = BUILDER_FINISH_OPTIONS (object);

  switch (prop_id)
    {
    case PROP_ARCH:
      g_hash_table_destroy (self->arch);
      self->arch = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_finish_options_class_init (BuilderFinishOptionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_finish_options_finalize;
  object_class->get_property = builder_finish_options_get_property;
  object_class->set_property = builder_finish_options_set_property;

  g_object_class_install_property (object_class,
                                   PROP_ARCH,
                                   g_param_spec_boxed ("arch",
                                                       "",
                                                       "",
                                                       G_TYPE_HASH_TABLE,
                                                       G_PARAM_READWRITE));
}

static void
builder_finish_options_init (BuilderFinishOptions *self)
{
  self->arch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

static JsonNode *
builder_finish_options_serialize_property (JsonSerializable *serializable,
                                   const gchar *property_name,
                                   const GValue *value,
                                   GParamSpec *pspec)
{
  if (strcmp (property_name, "arch") == 0)
    {
      BuilderFinishOptions *self = BUILDER_FINISH_OPTIONS (serializable);
      JsonNode *retval = NULL;

      if (self->arch && g_hash_table_size (self->arch) > 0)
        {
          JsonObject *object;
          GHashTableIter iter;
	  gpointer key, value;

	  object = json_object_new ();

	  g_hash_table_iter_init (&iter, self->arch);

	  while (g_hash_table_iter_next (&iter, &key, &value))
            {
              JsonNode *child = json_gobject_serialize (value);
              json_object_set_member (object, (char *) key, child);
	    }

	  retval = json_node_init_object (json_node_alloc (), object);
	  json_object_unref (object);
	}

      return retval;
    }

  return json_serializable_default_serialize_property (serializable,
                                                       property_name,
                                                       value,
                                                       pspec);
}

static gboolean
builder_finish_options_deserialize_property (JsonSerializable *serializable,
                                     const gchar *property_name,
				     GValue *value,
				     GParamSpec *pspec,
				     JsonNode *property_node)
{
  if (strcmp (property_name, "arch") == 0)
    {
      if (JSON_NODE_TYPE (property_node) == JSON_NODE_NULL)
        {
          g_value_set_boxed (value, NULL);
	  return TRUE;
	}
      else if (JSON_NODE_TYPE (property_node) == JSON_NODE_OBJECT)
	{
	  JsonObject *object = json_node_get_object (property_node);
	  g_autoptr(GHashTable) hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
	  g_autoptr(GList) members = NULL;

	  members = json_object_get_members (object);

	  return TRUE;
	}
      return FALSE;
    }
  return json_serializable_default_deserialize_property (serializable,
		                                         property_name,
							 value,
							 pspec, property_node);
}

static void
serializable_iface_init (JsonSerializableIface *serializable_iface)
{
  serializable_iface->serialize_property = builder_finish_options_serialize_property;
  serializable_iface->deserialize_property = builder_finish_options_deserialize_property;
}

char **
builder_finish_options_get_finish_args (BuilderFinishOptions *self, BuilderContext *context)
{
  char **options = NULL;
  const char *arch = builder_context_get_arch (context);

  options = g_hash_table_lookup (self->arch, arch);

  return options;
}
