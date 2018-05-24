/*
 * Copyright © 2016 Red Hat, Inc
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

#ifndef __FLATPAK_JSON_H__
#define __FLATPAK_JSON_H__

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_JSON flatpak_json_get_type ()

typedef struct _FlatpakJsonProp FlatpakJsonProp;

typedef enum {
  FLATPAK_JSON_PROP_TYPE_PARENT,
  FLATPAK_JSON_PROP_TYPE_INT64,
  FLATPAK_JSON_PROP_TYPE_BOOL,
  FLATPAK_JSON_PROP_TYPE_STRING,
  FLATPAK_JSON_PROP_TYPE_STRUCT,
  FLATPAK_JSON_PROP_TYPE_STRUCTV,
  FLATPAK_JSON_PROP_TYPE_STRV,
  FLATPAK_JSON_PROP_TYPE_STRMAP,
  FLATPAK_JSON_PROP_TYPE_BOOLMAP,
} FlatpakJsonPropType;

typedef enum {
  FLATPAK_JSON_PROP_FLAGS_NONE = 0,
  FLATPAK_JSON_PROP_FLAGS_OPTIONAL = 1<<0,
  FLATPAK_JSON_PROP_FLAGS_STRICT = 1<<1,
  FLATPAK_JSON_PROP_FLAGS_MANDATORY = 1<<2,
} FlatpakJsonPropFlags;


struct _FlatpakJsonProp {
  const char *name;
  gsize offset;
  FlatpakJsonPropType type;
  gpointer type_data;
  gpointer type_data2;
  FlatpakJsonPropFlags flags;
} ;

#define FLATPAK_JSON_STRING_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRING }
#define FLATPAK_JSON_MANDATORY_STRING_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRING, 0, 0, FLATPAK_JSON_PROP_FLAGS_MANDATORY }
#define FLATPAK_JSON_INT64_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_INT64 }
#define FLATPAK_JSON_BOOL_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_BOOL }
#define FLATPAK_JSON_STRV_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRV }
#define FLATPAK_JSON_STRMAP_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRMAP }
#define FLATPAK_JSON_BOOLMAP_PROP(_struct, _field, _name) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_BOOLMAP }
#define FLATPAK_JSON_STRUCT_PROP(_struct, _field, _name, _props) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRUCT, (gpointer)_props}
#define FLATPAK_JSON_OPT_STRUCT_PROP(_struct, _field, _name, _props) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRUCT, (gpointer)_props, 0, FLATPAK_JSON_PROP_FLAGS_OPTIONAL}
#define FLATPAK_JSON_STRICT_STRUCT_PROP(_struct, _field, _name, _props) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRUCT, (gpointer)_props, 0, FLATPAK_JSON_PROP_FLAGS_STRICT}
#define FLATPAK_JSON_MANDATORY_STRICT_STRUCT_PROP(_struct, _field, _name, _props) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRUCT, (gpointer)_props, 0, FLATPAK_JSON_PROP_FLAGS_STRICT | FLATPAK_JSON_PROP_FLAGS_MANDATORY}
#define FLATPAK_JSON_PARENT_PROP(_struct, _field, _props) \
  { "parent", G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_PARENT, (gpointer)_props}
#define FLATPAK_JSON_STRUCTV_PROP(_struct, _field, _name, _props) \
  { _name, G_STRUCT_OFFSET (_struct, _field), FLATPAK_JSON_PROP_TYPE_STRUCTV, (gpointer)_props, (gpointer) sizeof (**((_struct *) 0)->_field) }
#define FLATPAK_JSON_LAST_PROP { NULL }

G_DECLARE_DERIVABLE_TYPE (FlatpakJson, flatpak_json, FLATPAK, JSON, GObject)

struct _FlatpakJsonClass {
  GObjectClass parent_class;

  FlatpakJsonProp *props;
  const char *mediatype;
};

FlatpakJson *flatpak_json_from_node (JsonNode       *node,
                                     GType           type,
                                     GError        **error);
JsonNode   *flatpak_json_to_node   (FlatpakJson  *self);
FlatpakJson *flatpak_json_from_bytes (GBytes         *bytes,
                                      GType           type,
                                      GError        **error);
GBytes     *flatpak_json_to_bytes  (FlatpakJson  *self);

G_END_DECLS

#endif /* __FLATPAK_JSON_H__ */
