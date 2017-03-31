/* builder-module.c
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

#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-utils.h"
#include "builder-utils.h"
#include "builder-module.h"
#include "builder-post-process.h"

struct BuilderModule
{
  GObject         parent;

  char           *json_path;
  char           *name;
  char           *subdir;
  char          **post_install;
  char          **config_opts;
  char          **make_args;
  char          **make_install_args;
  char           *buildsystem;
  char          **ensure_writable;
  char          **only_arches;
  char          **skip_arches;
  gboolean        disabled;
  gboolean        rm_configure;
  gboolean        no_autogen;
  gboolean        no_parallel_make;
  gboolean        no_make_install;
  gboolean        no_python_timestamp_fix;
  gboolean        cmake;
  gboolean        builddir;
  BuilderOptions *build_options;
  GPtrArray      *changes;
  char          **cleanup;
  char          **cleanup_platform;
  GList          *sources;
  GList          *modules;
  char          **build_commands;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderModuleClass;

static void serializable_iface_init (JsonSerializableIface *serializable_iface);

G_DEFINE_TYPE_WITH_CODE (BuilderModule, builder_module, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (JSON_TYPE_SERIALIZABLE, serializable_iface_init));

enum {
  PROP_0,
  PROP_NAME,
  PROP_SUBDIR,
  PROP_RM_CONFIGURE,
  PROP_DISABLED,
  PROP_NO_AUTOGEN,
  PROP_NO_PARALLEL_MAKE,
  PROP_NO_MAKE_INSTALL,
  PROP_NO_PYTHON_TIMESTAMP_FIX,
  PROP_CMAKE,
  PROP_BUILDSYSTEM,
  PROP_BUILDDIR,
  PROP_CONFIG_OPTS,
  PROP_MAKE_ARGS,
  PROP_MAKE_INSTALL_ARGS,
  PROP_ENSURE_WRITABLE,
  PROP_ONLY_ARCHES,
  PROP_SKIP_ARCHES,
  PROP_SOURCES,
  PROP_BUILD_OPTIONS,
  PROP_CLEANUP,
  PROP_CLEANUP_PLATFORM,
  PROP_POST_INSTALL,
  PROP_MODULES,
  PROP_BUILD_COMMANDS,
  LAST_PROP
};

static void
collect_cleanup_for_path (const char **patterns,
                          const char  *path,
                          const char  *add_prefix,
                          GHashTable  *to_remove_ht)
{
  int i;

  if (patterns == NULL)
    return;

  for (i = 0; patterns[i] != NULL; i++)
    flatpak_collect_matches_for_path_pattern (path, patterns[i], add_prefix, to_remove_ht);
}

static void
builder_module_finalize (GObject *object)
{
  BuilderModule *self = (BuilderModule *) object;

  g_free (self->json_path);
  g_free (self->name);
  g_free (self->subdir);
  g_free (self->buildsystem);
  g_strfreev (self->post_install);
  g_strfreev (self->config_opts);
  g_strfreev (self->make_args);
  g_strfreev (self->make_install_args);
  g_strfreev (self->ensure_writable);
  g_strfreev (self->only_arches);
  g_strfreev (self->skip_arches);
  g_clear_object (&self->build_options);
  g_list_free_full (self->sources, g_object_unref);
  g_strfreev (self->cleanup);
  g_strfreev (self->cleanup_platform);
  g_list_free_full (self->modules, g_object_unref);
  g_strfreev (self->build_commands);

  if (self->changes)
    g_ptr_array_unref (self->changes);

  G_OBJECT_CLASS (builder_module_parent_class)->finalize (object);
}

static void
builder_module_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  BuilderModule *self = BUILDER_MODULE (object);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case PROP_SUBDIR:
      g_value_set_string (value, self->subdir);
      break;

    case PROP_RM_CONFIGURE:
      g_value_set_boolean (value, self->rm_configure);
      break;

    case PROP_DISABLED:
      g_value_set_boolean (value, self->disabled);
      break;

    case PROP_NO_AUTOGEN:
      g_value_set_boolean (value, self->no_autogen);
      break;

    case PROP_NO_PARALLEL_MAKE:
      g_value_set_boolean (value, self->no_parallel_make);
      break;

    case PROP_NO_MAKE_INSTALL:
      g_value_set_boolean (value, self->no_make_install);
      break;

    case PROP_NO_PYTHON_TIMESTAMP_FIX:
      g_value_set_boolean (value, self->no_python_timestamp_fix);
      break;

    case PROP_CMAKE:
      g_value_set_boolean (value, self->cmake);
      break;

    case PROP_BUILDSYSTEM:
      g_value_set_string (value, self->buildsystem);
      break;

    case PROP_BUILDDIR:
      g_value_set_boolean (value, self->builddir);
      break;

    case PROP_CONFIG_OPTS:
      g_value_set_boxed (value, self->config_opts);
      break;

    case PROP_MAKE_ARGS:
      g_value_set_boxed (value, self->make_args);
      break;

    case PROP_MAKE_INSTALL_ARGS:
      g_value_set_boxed (value, self->make_install_args);
      break;

    case PROP_ENSURE_WRITABLE:
      g_value_set_boxed (value, self->ensure_writable);
      break;

    case PROP_ONLY_ARCHES:
      g_value_set_boxed (value, self->only_arches);
      break;

    case PROP_SKIP_ARCHES:
      g_value_set_boxed (value, self->skip_arches);
      break;

    case PROP_POST_INSTALL:
      g_value_set_boxed (value, self->post_install);
      break;

    case PROP_BUILD_OPTIONS:
      g_value_set_object (value, self->build_options);
      break;

    case PROP_SOURCES:
      g_value_set_pointer (value, self->sources);
      break;

    case PROP_CLEANUP:
      g_value_set_boxed (value, self->cleanup);
      break;

    case PROP_CLEANUP_PLATFORM:
      g_value_set_boxed (value, self->cleanup_platform);
      break;

    case PROP_MODULES:
      g_value_set_pointer (value, self->modules);
      break;

    case PROP_BUILD_COMMANDS:
      g_value_set_boxed (value, self->build_commands);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_module_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  BuilderModule *self = BUILDER_MODULE (object);
  gchar **tmp;
  char *p;

  switch (prop_id)
    {
    case PROP_NAME:
      g_clear_pointer (&self->name, g_free);
      self->name = g_value_dup_string (value);
      if ((p = strchr (self->name, ' ')) ||
          (p = strchr (self->name, '/')))
        g_printerr ("Module names like '%s' containing '%c' are problematic. Expect errors.\n", self->name, *p);
      break;

    case PROP_SUBDIR:
      g_clear_pointer (&self->subdir, g_free);
      self->subdir = g_value_dup_string (value);
      break;

    case PROP_RM_CONFIGURE:
      self->rm_configure = g_value_get_boolean (value);
      break;

    case PROP_DISABLED:
      self->disabled = g_value_get_boolean (value);
      break;

    case PROP_NO_AUTOGEN:
      self->no_autogen = g_value_get_boolean (value);
      break;

    case PROP_NO_PARALLEL_MAKE:
      self->no_parallel_make = g_value_get_boolean (value);
      break;

    case PROP_NO_MAKE_INSTALL:
      self->no_make_install = g_value_get_boolean (value);
      break;

    case PROP_NO_PYTHON_TIMESTAMP_FIX:
      self->no_python_timestamp_fix = g_value_get_boolean (value);
      break;

    case PROP_CMAKE:
      self->cmake = g_value_get_boolean (value);
      break;

    case PROP_BUILDSYSTEM:
      g_free (self->buildsystem);
      self->buildsystem = g_value_dup_string (value);
      break;

    case PROP_BUILDDIR:
      self->builddir = g_value_get_boolean (value);
      break;

    case PROP_CONFIG_OPTS:
      tmp = self->config_opts;
      self->config_opts = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_MAKE_ARGS:
      tmp = self->make_args;
      self->make_args = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_MAKE_INSTALL_ARGS:
      tmp = self->make_install_args;
      self->make_install_args = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_ENSURE_WRITABLE:
      tmp = self->ensure_writable;
      self->ensure_writable = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
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

    case PROP_POST_INSTALL:
      tmp = self->post_install;
      self->post_install = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_BUILD_OPTIONS:
      g_set_object (&self->build_options,  g_value_get_object (value));
      break;

    case PROP_SOURCES:
      g_list_free_full (self->sources, g_object_unref);
      /* NOTE: This takes ownership of the list! */
      self->sources = g_value_get_pointer (value);
      break;

    case PROP_CLEANUP:
      tmp = self->cleanup;
      self->cleanup = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_CLEANUP_PLATFORM:
      tmp = self->cleanup_platform;
      self->cleanup_platform = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_MODULES:
      g_list_free_full (self->modules, g_object_unref);
      /* NOTE: This takes ownership of the list! */
      self->modules = g_value_get_pointer (value);
      break;

    case PROP_BUILD_COMMANDS:
      tmp = self->build_commands;
      self->build_commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_module_class_init (BuilderModuleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_module_finalize;
  object_class->get_property = builder_module_get_property;
  object_class->set_property = builder_module_set_property;

  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SUBDIR,
                                   g_param_spec_string ("subdir",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RM_CONFIGURE,
                                   g_param_spec_boolean ("rm-configure",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DISABLED,
                                   g_param_spec_boolean ("disabled",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_NO_AUTOGEN,
                                   g_param_spec_boolean ("no-autogen",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_NO_PARALLEL_MAKE,
                                   g_param_spec_boolean ("no-parallel-make",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_NO_MAKE_INSTALL,
                                   g_param_spec_boolean ("no-make-install",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_NO_PYTHON_TIMESTAMP_FIX,
                                   g_param_spec_boolean ("no-python-timestamp-fix",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CMAKE,
                                   g_param_spec_boolean ("cmake",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE|G_PARAM_DEPRECATED));
  g_object_class_install_property (object_class,
                                   PROP_BUILDSYSTEM,
                                   g_param_spec_string ("buildsystem",
                                                         "",
                                                         "",
                                                         NULL,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILDDIR,
                                   g_param_spec_boolean ("builddir",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SOURCES,
                                   g_param_spec_pointer ("sources",
                                                         "",
                                                         "",
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CONFIG_OPTS,
                                   g_param_spec_boxed ("config-opts",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_MAKE_ARGS,
                                   g_param_spec_boxed ("make-args",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_MAKE_INSTALL_ARGS,
                                   g_param_spec_boxed ("make-install-args",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ENSURE_WRITABLE,
                                   g_param_spec_boxed ("ensure-writable",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
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
  g_object_class_install_property (object_class,
                                   PROP_POST_INSTALL,
                                   g_param_spec_boxed ("post-install",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_OPTIONS,
                                   g_param_spec_object ("build-options",
                                                        "",
                                                        "",
                                                        BUILDER_TYPE_OPTIONS,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP,
                                   g_param_spec_boxed ("cleanup",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP_PLATFORM,
                                   g_param_spec_boxed ("cleanup-platform",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_MODULES,
                                   g_param_spec_pointer ("modules",
                                                         "",
                                                         "",
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_COMMANDS,
                                   g_param_spec_boxed ("build-commands",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
}

static void
builder_module_init (BuilderModule *self)
{
}

static JsonNode *
builder_module_serialize_property (JsonSerializable *serializable,
                                   const gchar      *property_name,
                                   const GValue     *value,
                                   GParamSpec       *pspec)
{
 if (strcmp (property_name, "modules") == 0)
    {
      BuilderModule *self = BUILDER_MODULE (serializable);
      JsonNode *retval = NULL;
      GList *l;

      if (self->modules)
        {
          JsonArray *array;

          array = json_array_sized_new (g_list_length (self->modules));

          for (l = self->modules; l != NULL; l = l->next)
            {
              JsonNode *child = json_gobject_serialize (l->data);
              json_array_add_element (array, child);
            }

          retval = json_node_init_array (json_node_alloc (), array);
          json_array_unref (array);
        }

      return retval;
    }
  else if (strcmp (property_name, "sources") == 0)
    {
      BuilderModule *self = BUILDER_MODULE (serializable);
      JsonNode *retval = NULL;
      GList *l;

      if (self->sources)
        {
          JsonArray *array;

          array = json_array_sized_new (g_list_length (self->sources));

          for (l = self->sources; l != NULL; l = l->next)
            {
              JsonNode *child = builder_source_to_json (BUILDER_SOURCE (l->data));
              json_array_add_element (array, child);
            }

          retval = json_node_init_array (json_node_alloc (), array);
          json_array_unref (array);
        }

      return retval;
    }
  else
    {
      return json_serializable_default_serialize_property (serializable,
                                                           property_name,
                                                           value,
                                                           pspec);
    }
}

static gboolean
builder_module_deserialize_property (JsonSerializable *serializable,
                                     const gchar      *property_name,
                                     GValue           *value,
                                     GParamSpec       *pspec,
                                     JsonNode         *property_node)
{
 if (strcmp (property_name, "modules") == 0)
    {
      if (JSON_NODE_TYPE (property_node) == JSON_NODE_NULL)
        {
          g_value_set_pointer (value, NULL);
          return TRUE;
        }
      else if (JSON_NODE_TYPE (property_node) == JSON_NODE_ARRAY)
        {
          JsonArray *array = json_node_get_array (property_node);
          guint i, array_len = json_array_get_length (array);
          GList *modules = NULL;
          GObject *module;

          for (i = 0; i < array_len; i++)
            {
              JsonNode *element_node = json_array_get_element (array, i);

              module = NULL;

              if (JSON_NODE_HOLDS_VALUE (element_node) &&
                  json_node_get_value_type (element_node) == G_TYPE_STRING)
                {
                  const char *module_path = json_node_get_string (element_node);
                  g_autofree char *json = NULL;

                  if (g_file_get_contents (module_path, &json, NULL, NULL))
                    module = json_gobject_from_data (BUILDER_TYPE_MODULE,
                                                     json, -1, NULL);
                }
              else if (JSON_NODE_HOLDS_OBJECT (element_node))
                module = json_gobject_deserialize (BUILDER_TYPE_MODULE, element_node);

              if (module == NULL)
                {
                  g_list_free_full (modules, g_object_unref);
                  return FALSE;
                }

              modules = g_list_prepend (modules, module);
            }

          g_value_set_pointer (value, g_list_reverse (modules));

          return TRUE;
        }

      return FALSE;
    }
  else if (strcmp (property_name, "sources") == 0)
    {
      if (JSON_NODE_TYPE (property_node) == JSON_NODE_NULL)
        {
          g_value_set_pointer (value, NULL);
          return TRUE;
        }
      else if (JSON_NODE_TYPE (property_node) == JSON_NODE_ARRAY)
        {
          JsonArray *array = json_node_get_array (property_node);
          guint i, array_len = json_array_get_length (array);
          GList *sources = NULL;
          BuilderSource *source;

          for (i = 0; i < array_len; i++)
            {
              JsonNode *element_node = json_array_get_element (array, i);

              if (JSON_NODE_TYPE (element_node) != JSON_NODE_OBJECT)
                {
                  g_list_free_full (sources, g_object_unref);
                  return FALSE;
                }

              source = builder_source_from_json (element_node);
              if (source == NULL)
                {
                  g_list_free_full (sources, g_object_unref);
                  return FALSE;
                }

              sources = g_list_prepend (sources, source);
            }

          g_value_set_pointer (value, g_list_reverse (sources));

          return TRUE;
        }

      return FALSE;
    }
  else
    {
      return json_serializable_default_deserialize_property (serializable,
                                                             property_name,
                                                             value,
                                                             pspec, property_node);
    }
}

static void
serializable_iface_init (JsonSerializableIface *serializable_iface)
{
  serializable_iface->serialize_property = builder_module_serialize_property;
  serializable_iface->deserialize_property = builder_module_deserialize_property;
}

const char *
builder_module_get_name (BuilderModule *self)
{
  return self->name;
}

gboolean
builder_module_is_enabled (BuilderModule *self,
                           BuilderContext *context)
{
  if (self->disabled)
    return FALSE;

  if (self->only_arches != NULL &&
      self->only_arches[0] != NULL &&
      !g_strv_contains ((const char * const *) self->only_arches, builder_context_get_arch (context)))
    return FALSE;

  if (self->skip_arches != NULL &&
      g_strv_contains ((const char * const *)self->skip_arches, builder_context_get_arch (context)))
    return FALSE;

  return TRUE;
}

gboolean
builder_module_get_disabled (BuilderModule *self)
{
  return self->disabled;
}

GList *
builder_module_get_sources (BuilderModule *self)
{
  return self->sources;
}

GList *
builder_module_get_modules (BuilderModule *self)
{
  return self->modules;
}

gboolean
builder_module_show_deps (BuilderModule *self,
                          BuilderContext *context,
                          GError         **error)
{
  GList *l;
  if (self->json_path)
    g_print ("%s\n", self->json_path);

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

      if (!builder_source_is_enabled (source, context))
        continue;

      if (!builder_source_show_deps (source, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
builder_module_download_sources (BuilderModule  *self,
                                 gboolean        update_vcs,
                                 BuilderContext *context,
                                 GError        **error)
{
  GList *l;

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

      if (!builder_source_is_enabled (source, context))
        continue;

      if (!builder_source_download (source, update_vcs, context, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }
    }

  return TRUE;
}

gboolean
builder_module_extract_sources (BuilderModule  *self,
                                GFile          *dest,
                                BuilderContext *context,
                                GError        **error)
{
  GList *l;

  if (!g_file_query_exists (dest, NULL) &&
      !g_file_make_directory_with_parents (dest, NULL, error))
    return FALSE;

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

      if (!builder_source_is_enabled (source, context))
        continue;

      if (!builder_source_extract (source, dest, self->build_options, context, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }
    }

  return TRUE;
}

gboolean
builder_module_bundle_sources (BuilderModule  *self,
                               BuilderContext *context,
                               GError        **error)
{
  GList *l;

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

      if (!builder_source_is_enabled (source, context))
        continue;

      if (!builder_source_bundle (source, context, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }
    }

  return TRUE;
}

static GPtrArray *
setup_build_args (GFile          *app_dir,
                  const char     *module_name,
                  BuilderContext *context,
                  GFile          *source_dir,
                  const char     *cwd_subdir,
                  char          **flatpak_opts,
                  char          **env_vars,
                  GFile         **cwd_file)
{
  g_autoptr(GPtrArray) args = NULL;
  g_autofree char *source_dir_path = g_file_get_path (source_dir);
  g_autofree char *source_dir_path_canonical = NULL;
  g_autofree char *ccache_dir_path = NULL;
  const char *builddir;
  int i;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));

  source_dir_path_canonical = realpath (source_dir_path, NULL);

  if (builder_context_get_build_runtime (context))
    builddir = "/run/build-runtime/";
  else
    builddir = "/run/build/";

  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));

  /* We mount the canonical location, because bind-mounts of symlinks don't really work */
  g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s", source_dir_path_canonical));

  /* Also make sure the original path is available (if it was not canonical, in case something references that. */
  if (strcmp (source_dir_path_canonical, source_dir_path) != 0)
  g_ptr_array_add (args, g_strdup_printf ("--bind-mount=%s=%s", source_dir_path, source_dir_path_canonical));

  g_ptr_array_add (args, g_strdup_printf ("--bind-mount=%s%s=%s", builddir, module_name, source_dir_path_canonical));
  if (cwd_subdir)
    g_ptr_array_add (args, g_strdup_printf ("--build-dir=%s%s/%s", builddir, module_name, cwd_subdir));
  else
    g_ptr_array_add (args, g_strdup_printf ("--build-dir=%s%s", builddir, module_name));

  if (g_file_query_exists (builder_context_get_ccache_dir (context), NULL))
    {
      ccache_dir_path = g_file_get_path (builder_context_get_ccache_dir (context));
      g_ptr_array_add (args, g_strdup_printf ("--bind-mount=/run/ccache=%s", ccache_dir_path));
    }

  if (flatpak_opts)
    {
      for (i = 0; flatpak_opts[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup (flatpak_opts[i]));
    }

  if (env_vars)
    {
      for (i = 0; env_vars[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--env=%s", env_vars[i]));
    }

  g_ptr_array_add (args, g_file_get_path (app_dir));

  *cwd_file = g_file_new_for_path (source_dir_path_canonical);

  return g_steal_pointer (&args);
}

static gboolean
shell (GFile          *app_dir,
       const char     *module_name,
       BuilderContext *context,
       GFile          *source_dir,
       const char     *cwd_subdir,
       char          **flatpak_opts,
       char          **env_vars,
       GError        **error)
{
  g_autoptr(GFile) cwd_file = NULL;
  g_autoptr(GPtrArray) args =
    setup_build_args (app_dir, module_name, context, source_dir, cwd_subdir, flatpak_opts, env_vars, &cwd_file);

  g_ptr_array_add (args, "sh");
  g_ptr_array_add (args, NULL);

  if (chdir (flatpak_file_get_path_cached (cwd_file)))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (execvp ((char *) args->pdata[0], (char **) args->pdata) == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start flatpak build");
      return FALSE;
    }

  return TRUE;
}

static const char skip_arg[] = "skip";
static const char strv_arg[] = "strv";

static gboolean
build (GFile          *app_dir,
       const char     *module_name,
       BuilderContext *context,
       GFile          *source_dir,
       const char     *cwd_subdir,
       char          **flatpak_opts,
       char          **env_vars,
       GError        **error,
       const gchar    *argv1,
       ...)
{
  g_autoptr(GFile) cwd_file = NULL;
  g_autoptr(GPtrArray) args =
    setup_build_args (app_dir, module_name, context, source_dir, cwd_subdir, flatpak_opts, env_vars, &cwd_file);
  const gchar *arg;
  const gchar **argv;
  va_list ap;
  int i;

  va_start (ap, argv1);
  g_ptr_array_add (args, g_strdup (argv1));
  while ((arg = va_arg (ap, const gchar *)))
    {
      if (arg == strv_arg)
        {
          argv = va_arg (ap, const gchar **);
          if (argv != NULL)
            {
              for (i = 0; argv[i] != NULL; i++)
                g_ptr_array_add (args, g_strdup (argv[i]));
            }
        }
      else if (arg != skip_arg)
        {
          g_ptr_array_add (args, g_strdup (arg));
        }
    }
  va_end (ap);

  g_ptr_array_add (args, NULL);

  if (!builder_maybe_host_spawnv (cwd_file, NULL, error, (const char * const *)args->pdata))
    {
      g_prefix_error (error, "module %s: ", module_name);
      return FALSE;
    }

  return TRUE;
}

gboolean
builder_module_ensure_writable (BuilderModule  *self,
                                BuilderCache   *cache,
                                BuilderContext *context,
                                GError        **error)
{
  g_autoptr(GPtrArray) changes = NULL;
  g_autoptr(GHashTable) matches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GFile *app_dir = builder_context_get_app_dir (context);
  GHashTableIter iter;
  gpointer key, value;
  int i;

  if (cache == NULL)
    return TRUE;

  if (self->ensure_writable == NULL ||
      self->ensure_writable[0] == NULL)
    return TRUE;

  changes = builder_cache_get_files (cache, error);
  if (changes == NULL)
    return FALSE;

  for (i = 0; i < changes->len; i++)
    {
      const char *path = g_ptr_array_index (changes, i);
      const char *unprefixed_path;
      const char *prefix;

      if (g_str_has_prefix (path, "files/"))
        prefix = "files/";
      else if (g_str_has_prefix (path, "usr/"))
        prefix = "usr/";
      else
        continue;

      unprefixed_path = path + strlen (prefix);

      collect_cleanup_for_path ((const char **)self->ensure_writable, unprefixed_path, prefix, matches);
    }

  g_hash_table_iter_init (&iter, matches);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *path = key;
      g_autoptr(GFile) dest = g_file_resolve_relative_path (app_dir, path);

      g_debug ("Breaking hardlink %s\n", path);
      if (!flatpak_break_hardlink (dest, error))
        return FALSE;
    }


  return TRUE;
}

gboolean
builder_module_build (BuilderModule  *self,
                      BuilderCache   *cache,
                      BuilderContext *context,
                      gboolean        run_shell,
                      GError        **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autofree char *make_j = NULL;
  g_autofree char *make_l = NULL;
  const char *make_cmd = NULL;

  gboolean autotools = FALSE, cmake = FALSE, cmake_ninja = FALSE, meson = FALSE, simple = FALSE;
  g_autoptr(GFile) configure_file = NULL;
  GFile *build_parent_dir = NULL;
  g_autoptr(GFile) build_dir = NULL;
  g_autoptr(GFile) build_link = NULL;
  g_autofree char *build_dir_relative = NULL;
  gboolean has_configure;
  gboolean var_require_builddir;
  gboolean use_builddir;
  int i;
  g_auto(GStrv) env = NULL;
  g_auto(GStrv) build_args = NULL;
  g_auto(GStrv) config_opts = NULL;
  g_autoptr(GFile) source_dir = NULL;
  g_autoptr(GFile) source_subdir = NULL;
  const char *source_subdir_relative = NULL;
  g_autofree char *source_dir_path = NULL;
  g_autofree char *buildname = NULL;
  g_autoptr(GError) my_error = NULL;
  BuilderPostProcessFlags post_process_flags = 0;

  source_dir = builder_context_allocate_build_subdir (context, self->name, error);
  if (source_dir == NULL)
    {
      g_prefix_error (error, "module %s: ", self->name);
      return FALSE;
    }

  build_parent_dir = g_file_get_parent (source_dir);
  buildname = g_file_get_basename (source_dir);
  source_dir_path = g_file_get_path (source_dir);

  /* Make an unversioned symlink */
  build_link = g_file_get_child (build_parent_dir, self->name);
  if (!g_file_delete (build_link, NULL, &my_error) &&
      !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&my_error));
      g_prefix_error (error, "module %s: ", self->name);
      return FALSE;
    }
  g_clear_error (&my_error);

  if (!g_file_make_symbolic_link (build_link,
                                  buildname,
                                  NULL, error))
    {
      g_prefix_error (error, "module %s: ", self->name);
      return FALSE;
    }

  g_print ("========================================================================\n");
  g_print ("Building module %s in %s\n", self->name, source_dir_path);
  g_print ("========================================================================\n");

  if (!builder_module_extract_sources (self, source_dir, context, error))
    return FALSE;

  if (!builder_module_bundle_sources (self, context, error))
    return FALSE;

  if (self->subdir != NULL && self->subdir[0] != 0)
    {
      source_subdir = g_file_resolve_relative_path (source_dir, self->subdir);
      source_subdir_relative = self->subdir;
    }
  else
    {
      source_subdir = g_object_ref (source_dir);
    }

  build_args = builder_options_get_build_args (self->build_options, context, error);
  if (build_args == NULL)
    return FALSE;

  env = builder_options_get_env (self->build_options, context);
  config_opts = builder_options_get_config_opts (self->build_options, context, self->config_opts);

  if (!self->buildsystem)
    {
      if (self->cmake)
        cmake = TRUE;
      else
        autotools = TRUE;
    }
  else if (!strcmp (self->buildsystem, "cmake"))
    cmake = TRUE;
  else if (!strcmp (self->buildsystem, "meson"))
    meson = TRUE;
  else if (!strcmp (self->buildsystem, "autotools"))
    autotools = TRUE;
  else if (!strcmp (self->buildsystem, "cmake-ninja"))
    cmake_ninja = TRUE;
  else if (!strcmp (self->buildsystem, "simple"))
    simple = TRUE;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module %s: Invalid buildsystem: \"%s\"",
                   self->name, self->buildsystem);
      return FALSE;
    }

  if (simple)
    {
      if (!self->build_commands)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module %s: Buildsystem simple requires specifying \"build-commands\"",
                       self->name);
          return FALSE;
        }
    }
  else if (cmake || cmake_ninja)
    {
      g_autoptr(GFile) cmake_file = NULL;

      cmake_file = g_file_get_child (source_subdir, "CMakeLists.txt");
      if (!g_file_query_exists (cmake_file, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module: %s: Can't find CMakeLists.txt", self->name);
          return FALSE;
        }
      configure_file = g_object_ref (cmake_file);
    }
  else if (meson)
    {
      g_autoptr(GFile) meson_file = NULL;

      meson_file = g_file_get_child (source_subdir, "meson.build");
      if (!g_file_query_exists (meson_file, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module: %s: Can't find meson.build", self->name);
          return FALSE;
        }
      configure_file = g_object_ref (meson_file);
    }
  else if (autotools)
    {
      configure_file = g_file_get_child (source_subdir, "configure");

      if (self->rm_configure)
        {
          if (!g_file_delete (configure_file, NULL, error))
            {
              g_prefix_error (error, "module %s: ", self->name);
              return FALSE;
            }
        }
    }

  if (configure_file)
    has_configure = g_file_query_exists (configure_file, NULL);

  if (configure_file && !has_configure && !self->no_autogen)
    {
      const char *autogen_names[] =  {"autogen", "autogen.sh", "bootstrap", NULL};
      g_autofree char *autogen_cmd = NULL;
      g_auto(GStrv) env_with_noconfigure = NULL;

      for (i = 0; autogen_names[i] != NULL; i++)
        {
          g_autoptr(GFile) autogen_file = g_file_get_child (source_subdir, autogen_names[i]);
          if (g_file_query_exists (autogen_file, NULL))
            {
              autogen_cmd = g_strdup_printf ("./%s", autogen_names[i]);
              break;
            }
        }

      if (autogen_cmd == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module %s: Can't find autogen, autogen.sh or bootstrap", self->name);
          return FALSE;
        }

      env_with_noconfigure = g_environ_setenv (g_strdupv (env), "NOCONFIGURE", "1", TRUE);
      if (!build (app_dir, self->name, context, source_dir, source_subdir_relative, build_args, env_with_noconfigure, error,
                  autogen_cmd, NULL))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }

      if (!g_file_query_exists (configure_file, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module %s: autogen did not create configure", self->name);
          return FALSE;
        }

      has_configure = TRUE;
    }

  if (configure_file && has_configure)
    {
      const char *configure_cmd;
      const char *cmake_generator = NULL;
      gchar *configure_final_arg = NULL;
      g_autofree char *configure_content = NULL;
      g_auto(GStrv) configure_args = NULL;
      g_autoptr(GPtrArray) configure_args_arr = g_ptr_array_new ();

      if (!g_file_load_contents (configure_file, NULL, &configure_content, NULL, NULL, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }

      var_require_builddir = strstr (configure_content, "buildapi-variable-require-builddir") != NULL;
      use_builddir = var_require_builddir || self->builddir;

      if (use_builddir)
        {
          if (source_subdir_relative)
            build_dir_relative = g_build_filename (source_subdir_relative, "_flatpak_build", NULL);
          else
            build_dir_relative = g_strdup ("_flatpak_build");
          build_dir = g_file_get_child (source_subdir, "_flatpak_build");

          if (!g_file_make_directory (build_dir, NULL, error))
            {
              g_prefix_error (error, "module %s: ", self->name);
              return FALSE;
            }

          if (cmake || cmake_ninja)
            {
              configure_cmd = "cmake";
              configure_final_arg = g_strdup("..");
            }
          else if (meson)
            {
              configure_cmd = "meson";
              configure_final_arg = g_strdup ("..");
            }
          else
            {
              configure_cmd = "../configure";
            }
        }
      else
        {
          build_dir_relative = g_strdup (source_subdir_relative);
          build_dir = g_object_ref (source_subdir);
          if (cmake || cmake_ninja)
            {
              configure_cmd = "cmake";
              configure_final_arg = g_strdup (".");
            }
          else if (meson)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module %s: Meson does not support building in sourcedir, set \"builddir\" to true.", self->name);
              return FALSE;
            }
            else
            {
              configure_cmd = "./configure";
            }
        }

      if (cmake)
        cmake_generator = "Unix Makefiles";
      else if (cmake_ninja)
        cmake_generator = "Ninja";

      if (cmake || cmake_ninja)
        {
          g_ptr_array_add (configure_args_arr, g_strdup_printf ("-DCMAKE_INSTALL_PREFIX:PATH='%s'",
                                                                builder_options_get_prefix (self->build_options, context)));
          g_ptr_array_add (configure_args_arr, g_strdup ("-G"));
          g_ptr_array_add (configure_args_arr, g_strdup_printf ("%s", cmake_generator));
        }
      else /* autotools and meson */
        {
          g_ptr_array_add (configure_args_arr, g_strdup_printf ("--prefix=%s",
                                                                builder_options_get_prefix (self->build_options, context)));
        }

      g_ptr_array_add (configure_args_arr, configure_final_arg);
      g_ptr_array_add (configure_args_arr, NULL);

      configure_args = (char **) g_ptr_array_free (g_steal_pointer (&configure_args_arr), FALSE);

      if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
                  configure_cmd, strv_arg, configure_args, strv_arg, config_opts, NULL))
        return FALSE;
    }
  else
    {
      build_dir_relative = g_strdup (source_subdir_relative);
      build_dir = g_object_ref (source_subdir);
    }

  if (meson || cmake_ninja)
    {
      g_autoptr(GFile) ninja_file = g_file_get_child (build_dir, "build.ninja");
      if (!g_file_query_exists (ninja_file, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module %s: Can't find ninja file", self->name);
          return FALSE;
        }
    }
  else if (autotools || cmake)
    {
      const char *makefile_names[] =  {"Makefile", "makefile", "GNUmakefile", NULL};

      for (i = 0; makefile_names[i] != NULL; i++)
        {
          g_autoptr(GFile) makefile_file = g_file_get_child (build_dir, makefile_names[i]);
          if (g_file_query_exists (makefile_file, NULL))
            break;
        }

      if (makefile_names[i] == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module %s: Can't find makefile", self->name);
          return FALSE;
        }
    }

  if (!self->no_parallel_make)
    {
      make_j = g_strdup_printf ("-j%d", builder_context_get_jobs (context));
      make_l = g_strdup_printf ("-l%d", 2 * builder_context_get_jobs (context));
    }

  if (run_shell)
    {
      if (!shell (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error))
        return FALSE;
      return TRUE;
    }

  /* Build and install */

  if (meson || cmake_ninja)
    make_cmd = "ninja";
  else if (simple)
    make_cmd = NULL;
  else
    make_cmd = "make";

  if (make_cmd)
    {
      if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
                  make_cmd, make_j ? make_j : skip_arg, make_l ? make_l : skip_arg, strv_arg, self->make_args, NULL))
        return FALSE;
    }

  for (i = 0; self->build_commands != NULL && self->build_commands[i] != NULL; i++)
    {
      g_print ("Running: %s\n", self->build_commands[i]);
      if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
                  "/bin/sh", "-c", self->build_commands[i], NULL))
        return FALSE;
    }

  if (!self->no_make_install && make_cmd)
    {
      if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
                  make_cmd, "install", strv_arg, self->make_install_args, NULL))
        return FALSE;
    }

  /* Post installation scripts */

  if (builder_context_get_separate_locales (context))
    {
      g_autoptr(GFile) root_dir = NULL;

      if (builder_context_get_build_runtime (context))
        root_dir = g_file_get_child (app_dir, "usr");
      else
        root_dir = g_file_get_child (app_dir, "files");

      if (!builder_migrate_locale_dirs (root_dir, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }
    }

  if (self->post_install)
    {
      for (i = 0; self->post_install[i] != NULL; i++)
        {
          if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
                      "/bin/sh", "-c", self->post_install[i], NULL))
            return FALSE;
        }
    }

  if (!self->no_python_timestamp_fix)
    post_process_flags |= BUILDER_POST_PROCESS_FLAGS_PYTHON_TIMESTAMPS;

  if (builder_options_get_strip (self->build_options, context))
    post_process_flags |= BUILDER_POST_PROCESS_FLAGS_STRIP;
  else if (!builder_options_get_no_debuginfo (self->build_options, context) &&
           /* No support for debuginfo for extensions atm */
           !builder_context_get_build_extension (context))
    post_process_flags |= BUILDER_POST_PROCESS_FLAGS_DEBUGINFO;

  if (!builder_post_process (post_process_flags, app_dir,
                             cache, context, error))
    {
      g_prefix_error (error, "module %s: ", self->name);
      return FALSE;
    }

  /* Clean up build dir */

  if (!builder_context_get_keep_build_dirs (context))
    {
      if (!g_file_delete (build_link, NULL, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }

      if (!flatpak_rm_rf (source_dir, NULL, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }
    }

  return TRUE;
}

gboolean
builder_module_update (BuilderModule  *self,
                       BuilderContext *context,
                       GError        **error)
{
  GList *l;

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

      if (!builder_source_is_enabled (source, context))
        continue;

      if (!builder_source_update (source, context, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }
    }

  return TRUE;
}

void
builder_module_checksum (BuilderModule  *self,
                         BuilderCache   *cache,
                         BuilderContext *context)
{
  GList *l;

  builder_cache_checksum_str (cache, BUILDER_MODULE_CHECKSUM_VERSION);
  builder_cache_checksum_str (cache, self->name);
  builder_cache_checksum_str (cache, self->subdir);
  builder_cache_checksum_strv (cache, self->post_install);
  builder_cache_checksum_strv (cache, self->config_opts);
  builder_cache_checksum_strv (cache, self->make_args);
  builder_cache_checksum_strv (cache, self->make_install_args);
  builder_cache_checksum_strv (cache, self->ensure_writable);
  builder_cache_checksum_strv (cache, self->only_arches);
  builder_cache_checksum_strv (cache, self->skip_arches);
  builder_cache_checksum_boolean (cache, self->rm_configure);
  builder_cache_checksum_boolean (cache, self->no_autogen);
  builder_cache_checksum_boolean (cache, self->disabled);
  builder_cache_checksum_boolean (cache, self->no_parallel_make);
  builder_cache_checksum_boolean (cache, self->no_make_install);
  builder_cache_checksum_boolean (cache, self->no_python_timestamp_fix);
  builder_cache_checksum_boolean (cache, self->cmake);
  builder_cache_checksum_boolean (cache, self->builddir);

  if (self->build_options)
    builder_options_checksum (self->build_options, cache, context);

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

      if (!builder_source_is_enabled (source, context))
        continue;

      builder_source_checksum (source, cache, context);
    }
}

void
builder_module_checksum_for_cleanup (BuilderModule  *self,
                                     BuilderCache   *cache,
                                     BuilderContext *context)
{
  builder_cache_checksum_str (cache, BUILDER_MODULE_CHECKSUM_VERSION);
  builder_cache_checksum_str (cache, self->name);
  builder_cache_checksum_strv (cache, self->cleanup);
}

void
builder_module_checksum_for_platform (BuilderModule  *self,
                                      BuilderCache   *cache,
                                      BuilderContext *context)
{
  builder_cache_checksum_strv (cache, self->cleanup_platform);
}

void
builder_module_set_json_path (BuilderModule *self,
                              const char *json_path)
{
  self->json_path = g_strdup (json_path);
}

GPtrArray *
builder_module_get_changes (BuilderModule *self)
{
  return self->changes;
}

void
builder_module_set_changes (BuilderModule *self,
                            GPtrArray     *changes)
{
  if (self->changes != changes)
    {
      if (self->changes)
        g_ptr_array_unref (self->changes);
      self->changes = g_ptr_array_ref (changes);
    }
}

static gboolean
matches_cleanup_for_path (const char **patterns,
                          const char  *path)
{
  int i;

  if (patterns == NULL)
    return FALSE;

  for (i = 0; patterns[i] != NULL; i++)
    {
      if (flatpak_matches_path_pattern (path, patterns[i]))
        return TRUE;
    }

  return FALSE;
}

void
builder_module_cleanup_collect (BuilderModule  *self,
                                gboolean        platform,
                                BuilderContext *context,
                                GHashTable     *to_remove_ht)
{
  GPtrArray *changed_files;
  int i;
  const char **global_patterns;
  const char **local_patterns;

  if (!self->changes)
    return;

  if (platform)
    {
      global_patterns = builder_context_get_global_cleanup_platform (context);
      local_patterns = (const char **) self->cleanup_platform;
    }
  else
    {
      global_patterns = builder_context_get_global_cleanup (context);
      local_patterns = (const char **) self->cleanup;
    }

  changed_files = self->changes;
  for (i = 0; i < changed_files->len; i++)
    {
      const char *path = g_ptr_array_index (changed_files, i);
      const char *unprefixed_path;
      const char *prefix;

      if (g_str_has_prefix (path, "files/"))
        prefix = "files/";
      else if (g_str_has_prefix (path, "usr/"))
        prefix = "usr/";
      else
        continue;

      unprefixed_path = path + strlen (prefix);

      collect_cleanup_for_path (global_patterns, unprefixed_path, prefix, to_remove_ht);
      collect_cleanup_for_path (local_patterns, unprefixed_path, prefix, to_remove_ht);

      if (g_str_has_prefix (unprefixed_path, "lib/debug/") &&
          g_str_has_suffix (unprefixed_path, ".debug"))
        {
          g_autofree char *real_path = g_strdup (unprefixed_path);
          g_autofree char *real_parent = NULL;
          g_autofree char *parent = NULL;
          g_autofree char *debug_path = NULL;

          debug_path = g_strdup (unprefixed_path + strlen ("lib/debug/"));
          debug_path[strlen (debug_path) - strlen (".debug")] = 0;

          while (TRUE)
            {
              if (matches_cleanup_for_path (global_patterns, debug_path) ||
                  matches_cleanup_for_path (local_patterns, debug_path))
                g_hash_table_insert (to_remove_ht, g_strconcat (prefix, real_path, NULL), GINT_TO_POINTER (1));

              real_parent = g_path_get_dirname (real_path);
              if (strcmp (real_parent, ".") == 0)
                break;
              g_free (real_path);
              real_path = g_steal_pointer (&real_parent);

              parent = g_path_get_dirname (debug_path);
              g_free (debug_path);
              debug_path = g_steal_pointer (&parent);
            }
        }
    }
}
