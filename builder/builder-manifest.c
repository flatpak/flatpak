/* builder-manifest.c
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

#include "builder-manifest.h"
#include "builder-utils.h"
#include "flatpak-utils.h"

#include "libglnx/libglnx.h"

#define LOCALES_SEPARATE_DIR "share/runtime/locale"

struct BuilderManifest
{
  GObject         parent;

  char           *id;
  char           *id_platform;
  char           *branch;
  char           *runtime;
  char           *runtime_commit;
  char           *runtime_version;
  char           *sdk;
  char           *sdk_commit;
  char           *var;
  char           *metadata;
  char           *metadata_platform;
  gboolean        separate_locales;
  char          **cleanup;
  char          **cleanup_commands;
  char          **cleanup_platform;
  char          **finish_args;
  char          **tags;
  char           *rename_desktop_file;
  char           *rename_appdata_file;
  char           *rename_icon;
  gboolean        copy_icon;
  char           *desktop_file_name_prefix;
  char           *desktop_file_name_suffix;
  gboolean        build_runtime;
  gboolean        writable_sdk;
  gboolean        appstream_compose;
  char          **sdk_extensions;
  char          **platform_extensions;
  char           *command;
  BuilderOptions *build_options;
  GList          *modules;
  GList          *expanded_modules;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderManifestClass;

static void serializable_iface_init (JsonSerializableIface *serializable_iface);

G_DEFINE_TYPE_WITH_CODE (BuilderManifest, builder_manifest, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (JSON_TYPE_SERIALIZABLE, serializable_iface_init));

enum {
  PROP_0,
  PROP_APP_ID, /* Backwards compat with early version, use id */
  PROP_ID,
  PROP_ID_PLATFORM,
  PROP_BRANCH,
  PROP_RUNTIME,
  PROP_RUNTIME_VERSION,
  PROP_SDK,
  PROP_SDK_COMMIT,
  PROP_VAR,
  PROP_METADATA,
  PROP_METADATA_PLATFORM,
  PROP_BUILD_OPTIONS,
  PROP_COMMAND,
  PROP_MODULES,
  PROP_CLEANUP,
  PROP_CLEANUP_COMMANDS,
  PROP_CLEANUP_PLATFORM,
  PROP_BUILD_RUNTIME,
  PROP_SEPARATE_LOCALES,
  PROP_WRITABLE_SDK,
  PROP_APPSTREAM_COMPOSE,
  PROP_SDK_EXTENSIONS,
  PROP_PLATFORM_EXTENSIONS,
  PROP_FINISH_ARGS,
  PROP_TAGS,
  PROP_RENAME_DESKTOP_FILE,
  PROP_RENAME_APPDATA_FILE,
  PROP_RENAME_ICON,
  PROP_COPY_ICON,
  PROP_DESKTOP_FILE_NAME_PREFIX,
  PROP_DESKTOP_FILE_NAME_SUFFIX,
  LAST_PROP
};

static void
builder_manifest_finalize (GObject *object)
{
  BuilderManifest *self = (BuilderManifest *) object;

  g_free (self->id);
  g_free (self->branch);
  g_free (self->runtime);
  g_free (self->runtime_commit);
  g_free (self->runtime_version);
  g_free (self->sdk);
  g_free (self->sdk_commit);
  g_free (self->var);
  g_free (self->metadata);
  g_free (self->metadata_platform);
  g_free (self->command);
  g_clear_object (&self->build_options);
  g_list_free_full (self->modules, g_object_unref);
  g_list_free (self->expanded_modules);
  g_strfreev (self->cleanup);
  g_strfreev (self->cleanup_commands);
  g_strfreev (self->cleanup_platform);
  g_strfreev (self->finish_args);
  g_strfreev (self->tags);
  g_free (self->rename_desktop_file);
  g_free (self->rename_appdata_file);
  g_free (self->rename_icon);
  g_free (self->desktop_file_name_prefix);
  g_free (self->desktop_file_name_suffix);

  G_OBJECT_CLASS (builder_manifest_parent_class)->finalize (object);
}

static gboolean
expand_modules (GList *modules, GList **expanded, GHashTable *names, GError **error)
{
  GList *l;

  for (l = modules; l; l = l->next)
    {
      BuilderModule *m = l->data;
      GList *submodules = NULL;
      const char *name;

      if (builder_module_get_disabled (m))
        continue;

      if (!expand_modules (builder_module_get_modules (m), &submodules, names, error))
        return FALSE;

      *expanded = g_list_concat (*expanded, submodules);

      name = builder_module_get_name (m);
      if (g_hash_table_lookup (names, name) != NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Duplicate modules named '%s'", name);
          return FALSE;
        }
      g_hash_table_insert (names, (char *)name, (char *)name);
      *expanded = g_list_append (*expanded, m);
    }

  return TRUE;
}

static void
builder_manifest_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  BuilderManifest *self = BUILDER_MANIFEST (object);

  switch (prop_id)
    {
    case PROP_APP_ID:
      g_value_set_string (value, NULL);
      break;

    case PROP_ID:
      g_value_set_string (value, self->id);
      break;

    case PROP_ID_PLATFORM:
      g_value_set_string (value, self->id_platform);
      break;

    case PROP_BRANCH:
      g_value_set_string (value, self->branch);
      break;

    case PROP_RUNTIME:
      g_value_set_string (value, self->runtime);
      break;

    case PROP_RUNTIME_VERSION:
      g_value_set_string (value, self->runtime_version);
      break;

    case PROP_SDK:
      g_value_set_string (value, self->sdk);
      break;

    case PROP_SDK_COMMIT:
      g_value_set_string (value, self->sdk_commit);
      break;

    case PROP_VAR:
      g_value_set_string (value, self->var);
      break;

    case PROP_METADATA:
      g_value_set_string (value, self->metadata);
      break;

    case PROP_METADATA_PLATFORM:
      g_value_set_string (value, self->metadata_platform);
      break;

    case PROP_COMMAND:
      g_value_set_string (value, self->command);
      break;

    case PROP_BUILD_OPTIONS:
      g_value_set_object (value, self->build_options);
      break;

    case PROP_MODULES:
      g_value_set_pointer (value, self->modules);
      break;

    case PROP_CLEANUP:
      g_value_set_boxed (value, self->cleanup);
      break;

    case PROP_CLEANUP_COMMANDS:
      g_value_set_boxed (value, self->cleanup_commands);
      break;

    case PROP_CLEANUP_PLATFORM:
      g_value_set_boxed (value, self->cleanup_platform);
      break;

    case PROP_FINISH_ARGS:
      g_value_set_boxed (value, self->finish_args);
      break;

    case PROP_TAGS:
      g_value_set_boxed (value, self->tags);
      break;

    case PROP_BUILD_RUNTIME:
      g_value_set_boolean (value, self->build_runtime);
      break;

    case PROP_SEPARATE_LOCALES:
      g_value_set_boolean (value, self->separate_locales);
      break;

    case PROP_WRITABLE_SDK:
      g_value_set_boolean (value, self->writable_sdk);
      break;

    case PROP_APPSTREAM_COMPOSE:
      g_value_set_boolean (value, self->appstream_compose);
      break;

    case PROP_SDK_EXTENSIONS:
      g_value_set_boxed (value, self->sdk_extensions);
      break;

    case PROP_PLATFORM_EXTENSIONS:
      g_value_set_boxed (value, self->platform_extensions);
      break;

    case PROP_COPY_ICON:
      g_value_set_boolean (value, self->copy_icon);
      break;

    case PROP_RENAME_DESKTOP_FILE:
      g_value_set_string (value, self->rename_desktop_file);
      break;

    case PROP_RENAME_APPDATA_FILE:
      g_value_set_string (value, self->rename_appdata_file);
      break;

    case PROP_RENAME_ICON:
      g_value_set_string (value, self->rename_icon);
      break;

    case PROP_DESKTOP_FILE_NAME_PREFIX:
      g_value_set_string (value, self->desktop_file_name_prefix);
      break;

    case PROP_DESKTOP_FILE_NAME_SUFFIX:
      g_value_set_string (value, self->desktop_file_name_suffix);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_manifest_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  BuilderManifest *self = BUILDER_MANIFEST (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_APP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case PROP_ID:
      g_free (self->id);
      self->id = g_value_dup_string (value);
      break;

    case PROP_ID_PLATFORM:
      g_free (self->id_platform);
      self->id_platform = g_value_dup_string (value);
      break;

    case PROP_BRANCH:
      g_free (self->branch);
      self->branch = g_value_dup_string (value);
      break;

    case PROP_RUNTIME:
      g_free (self->runtime);
      self->runtime = g_value_dup_string (value);
      break;

    case PROP_RUNTIME_VERSION:
      g_free (self->runtime_version);
      self->runtime_version = g_value_dup_string (value);
      break;

    case PROP_SDK:
      g_free (self->sdk);
      self->sdk = g_value_dup_string (value);
      break;

    case PROP_SDK_COMMIT:
      g_free (self->sdk_commit);
      self->sdk_commit = g_value_dup_string (value);
      break;

    case PROP_VAR:
      g_free (self->var);
      self->var = g_value_dup_string (value);
      break;

    case PROP_METADATA:
      g_free (self->metadata);
      self->metadata = g_value_dup_string (value);
      break;

    case PROP_METADATA_PLATFORM:
      g_free (self->metadata_platform);
      self->metadata_platform = g_value_dup_string (value);
      break;

    case PROP_COMMAND:
      g_free (self->command);
      self->command = g_value_dup_string (value);
      break;

    case PROP_BUILD_OPTIONS:
      g_set_object (&self->build_options,  g_value_get_object (value));
      break;

    case PROP_MODULES:
      g_list_free_full (self->modules, g_object_unref);
      /* NOTE: This takes ownership of the list! */
      self->modules = g_value_get_pointer (value);
      break;

    case PROP_CLEANUP:
      tmp = self->cleanup;
      self->cleanup = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_CLEANUP_COMMANDS:
      tmp = self->cleanup_commands;
      self->cleanup_commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_CLEANUP_PLATFORM:
      tmp = self->cleanup_platform;
      self->cleanup_platform = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_FINISH_ARGS:
      tmp = self->finish_args;
      self->finish_args = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_TAGS:
      tmp = self->tags;
      self->tags = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_BUILD_RUNTIME:
      self->build_runtime = g_value_get_boolean (value);
      break;

    case PROP_SEPARATE_LOCALES:
      self->separate_locales = g_value_get_boolean (value);
      break;

    case PROP_WRITABLE_SDK:
      self->writable_sdk = g_value_get_boolean (value);
      break;

    case PROP_APPSTREAM_COMPOSE:
      self->appstream_compose = g_value_get_boolean (value);
      break;

    case PROP_SDK_EXTENSIONS:
      tmp = self->sdk_extensions;
      self->sdk_extensions = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_PLATFORM_EXTENSIONS:
      tmp = self->platform_extensions;
      self->platform_extensions = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_COPY_ICON:
      self->copy_icon = g_value_get_boolean (value);
      break;

    case PROP_RENAME_DESKTOP_FILE:
      g_free (self->rename_desktop_file);
      self->rename_desktop_file = g_value_dup_string (value);
      break;

    case PROP_RENAME_APPDATA_FILE:
      g_free (self->rename_appdata_file);
      self->rename_appdata_file = g_value_dup_string (value);
      break;

    case PROP_RENAME_ICON:
      g_free (self->rename_icon);
      self->rename_icon = g_value_dup_string (value);
      break;

    case PROP_DESKTOP_FILE_NAME_PREFIX:
      g_free (self->desktop_file_name_prefix);
      self->desktop_file_name_prefix = g_value_dup_string (value);
      break;

    case PROP_DESKTOP_FILE_NAME_SUFFIX:
      g_free (self->desktop_file_name_suffix);
      self->desktop_file_name_suffix = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_manifest_class_init (BuilderManifestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_manifest_finalize;
  object_class->get_property = builder_manifest_get_property;
  object_class->set_property = builder_manifest_set_property;

  g_object_class_install_property (object_class,
                                   PROP_APP_ID,
                                   g_param_spec_string ("app-id",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ID,
                                   g_param_spec_string ("id",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ID_PLATFORM,
                                   g_param_spec_string ("id-platform",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BRANCH,
                                   g_param_spec_string ("branch",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RUNTIME,
                                   g_param_spec_string ("runtime",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RUNTIME_VERSION,
                                   g_param_spec_string ("runtime-version",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SDK,
                                   g_param_spec_string ("sdk",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SDK_COMMIT,
                                   g_param_spec_string ("sdk-commit",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_VAR,
                                   g_param_spec_string ("var",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_METADATA,
                                   g_param_spec_string ("metadata",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_METADATA_PLATFORM,
                                   g_param_spec_string ("metadata-platform",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_COMMAND,
                                   g_param_spec_string ("command",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_OPTIONS,
                                   g_param_spec_object ("build-options",
                                                        "",
                                                        "",
                                                        BUILDER_TYPE_OPTIONS,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_MODULES,
                                   g_param_spec_pointer ("modules",
                                                         "",
                                                         "",
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP,
                                   g_param_spec_boxed ("cleanup",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CLEANUP_COMMANDS,
                                   g_param_spec_boxed ("cleanup-commands",
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
                                   PROP_FINISH_ARGS,
                                   g_param_spec_boxed ("finish-args",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_RUNTIME,
                                   g_param_spec_boolean ("build-runtime",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SEPARATE_LOCALES,
                                   g_param_spec_boolean ("separate-locales",
                                                         "",
                                                         "",
                                                         TRUE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_WRITABLE_SDK,
                                   g_param_spec_boolean ("writable-sdk",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_APPSTREAM_COMPOSE,
                                   g_param_spec_boolean ("appstream-compose",
                                                         "",
                                                         "",
                                                         TRUE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SDK_EXTENSIONS,
                                   g_param_spec_boxed ("sdk-extensions",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_PLATFORM_EXTENSIONS,
                                   g_param_spec_boxed ("platform-extensions",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_TAGS,
                                   g_param_spec_boxed ("tags",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RENAME_DESKTOP_FILE,
                                   g_param_spec_string ("rename-desktop-file",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RENAME_APPDATA_FILE,
                                   g_param_spec_string ("rename-appdata-file",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_RENAME_ICON,
                                   g_param_spec_string ("rename-icon",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_COPY_ICON,
                                   g_param_spec_boolean ("copy-icon",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DESKTOP_FILE_NAME_PREFIX,
                                   g_param_spec_string ("desktop-file-name-prefix",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DESKTOP_FILE_NAME_SUFFIX,
                                   g_param_spec_string ("desktop-file-name-suffix",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_manifest_init (BuilderManifest *self)
{
  self->appstream_compose = TRUE;
  self->separate_locales = TRUE;
}

static JsonNode *
builder_manifest_serialize_property (JsonSerializable *serializable,
                                     const gchar      *property_name,
                                     const GValue     *value,
                                     GParamSpec       *pspec)
{
  if (strcmp (property_name, "modules") == 0)
    {
      BuilderManifest *self = BUILDER_MANIFEST (serializable);
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
  else
    {
      return json_serializable_default_serialize_property (serializable,
                                                           property_name,
                                                           value,
                                                           pspec);
    }
}

static gboolean
builder_manifest_deserialize_property (JsonSerializable *serializable,
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
  serializable_iface->serialize_property = builder_manifest_serialize_property;
  serializable_iface->deserialize_property = builder_manifest_deserialize_property;
}

const char *
builder_manifest_get_id (BuilderManifest *self)
{
  return self->id;
}

const char *
builder_manifest_get_id_platform (BuilderManifest *self)
{
  return self->id_platform;
}

BuilderOptions *
builder_manifest_get_build_options (BuilderManifest *self)
{
  return self->build_options;
}

GList *
builder_manifest_get_modules (BuilderManifest *self)
{
  return self->modules;
}

static const char *
builder_manifest_get_runtime_version (BuilderManifest *self)
{
  return self->runtime_version ? self->runtime_version : "master";
}

const char *
builder_manifest_get_branch (BuilderManifest *self)
{
  return self->branch ? self->branch : "master";
}

static char *
flatpak (GError **error,
         ...)
{
  gboolean res;
  g_autofree char *output = NULL;
  va_list ap;

  va_start (ap, error);
  res = flatpak_spawn (NULL, &output, error, "flatpak", ap);
  va_end (ap);

  if (res)
    {
      g_strchomp (output);
      return g_steal_pointer (&output);
    }
  return NULL;
}

gboolean
builder_manifest_start (BuilderManifest *self,
                        BuilderContext  *context,
                        GError         **error)
{
  g_autofree char *arch_option = NULL;
  g_autoptr(GHashTable) names = g_hash_table_new (g_str_hash, g_str_equal);
  const char *stop_at;

  if (self->sdk == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "sdk not specified");
      return FALSE;
    }

  arch_option = g_strdup_printf ("--arch=%s", builder_context_get_arch (context));

  self->sdk_commit = flatpak (NULL, "info", arch_option, "--show-commit", self->sdk,
                              builder_manifest_get_runtime_version (self), NULL);
  if (self->sdk_commit == NULL)
    return flatpak_fail (error, "Unable to find sdk %s version %s",
                         self->sdk,
                         builder_manifest_get_runtime_version (self));

  self->runtime_commit = flatpak (NULL, "info", arch_option, "--show-commit", self->runtime,
                                  builder_manifest_get_runtime_version (self), NULL);
  if (self->runtime_commit == NULL)
    return flatpak_fail (error, "Unable to find runtime %s version %s",
                         self->runtime,
                         builder_manifest_get_runtime_version (self));

  if (!expand_modules (self->modules, &self->expanded_modules, names, error))
    return FALSE;

  stop_at = builder_context_get_stop_at (context);
  if (stop_at != NULL && g_hash_table_lookup (names, stop_at) == NULL)
    return flatpak_fail (error, "No module named %s (specified with --stop-at)", stop_at);

  return TRUE;
}

gboolean
builder_manifest_init_app_dir (BuilderManifest *self,
                               BuilderContext  *context,
                               GError         **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);

  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GPtrArray) args = NULL;
  g_autofree char *commandline = NULL;
  int i;

  g_print ("Initializing build dir\n");

  if (self->id == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "id not specified");
      return FALSE;
    }

  if (self->runtime == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "runtime not specified");
      return FALSE;
    }

  if (self->sdk == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "sdk not specified");
      return FALSE;
    }

  args = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build-init"));
  if (self->writable_sdk || self->build_runtime)
    {
      g_ptr_array_add (args, g_strdup ("-w"));

      for (i = 0; self->sdk_extensions != NULL && self->sdk_extensions[i] != NULL; i++)
        {
          const char *ext = self->sdk_extensions[i];
          g_ptr_array_add (args, g_strdup_printf ("--sdk-extension=%s", ext));
        }
    }
  if (self->tags)
    {
      for (i = 0; self->tags[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--tag=%s", self->tags[i]));
    }
  if (self->var)
    g_ptr_array_add (args, g_strdup_printf ("--var=%s", self->var));
  g_ptr_array_add (args, g_strdup_printf ("--arch=%s", builder_context_get_arch (context)));
  g_ptr_array_add (args, g_file_get_path (app_dir));
  g_ptr_array_add (args, g_strdup (self->id));
  g_ptr_array_add (args, g_strdup (self->sdk));
  g_ptr_array_add (args, g_strdup (self->runtime));
  g_ptr_array_add (args, g_strdup (builder_manifest_get_runtime_version (self)));
  g_ptr_array_add (args, NULL);

  commandline = g_strjoinv (" ", (char **) args->pdata);
  g_debug ("Running '%s'", commandline);

  subp =
    g_subprocess_newv ((const gchar * const *) args->pdata,
                       G_SUBPROCESS_FLAGS_NONE,
                       error);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  if (self->build_runtime && self->separate_locales)
    {
      g_autoptr(GFile) root_dir = NULL;

      root_dir = g_file_get_child (app_dir, "usr");

      if (!builder_migrate_locale_dirs (root_dir, error))
        return FALSE;
    }

  return TRUE;
}

/* This gets the checksum of everything that globally affects the build */
void
builder_manifest_checksum (BuilderManifest *self,
                           BuilderCache    *cache,
                           BuilderContext  *context)
{
  builder_cache_checksum_str (cache, BUILDER_MANIFEST_CHECKSUM_VERSION);
  builder_cache_checksum_str (cache, self->id);
  /* No need to include version here, it doesn't affect the build */
  builder_cache_checksum_str (cache, self->runtime);
  builder_cache_checksum_str (cache, builder_manifest_get_runtime_version (self));
  builder_cache_checksum_str (cache, self->sdk);
  builder_cache_checksum_str (cache, self->sdk_commit);
  builder_cache_checksum_str (cache, self->var);
  builder_cache_checksum_str (cache, self->metadata);
  builder_cache_checksum_strv (cache, self->tags);
  builder_cache_checksum_boolean (cache, self->writable_sdk);
  builder_cache_checksum_strv (cache, self->sdk_extensions);
  builder_cache_checksum_boolean (cache, self->build_runtime);
  builder_cache_checksum_boolean (cache, self->separate_locales);

  if (self->build_options)
    builder_options_checksum (self->build_options, cache, context);
}

void
builder_manifest_checksum_for_cleanup (BuilderManifest *self,
                                       BuilderCache    *cache,
                                       BuilderContext  *context)
{
  GList *l;

  builder_cache_checksum_str (cache, BUILDER_MANIFEST_CHECKSUM_CLEANUP_VERSION);
  builder_cache_checksum_strv (cache, self->cleanup);
  builder_cache_checksum_strv (cache, self->cleanup_commands);
  builder_cache_checksum_str (cache, self->rename_desktop_file);
  builder_cache_checksum_str (cache, self->rename_appdata_file);
  builder_cache_checksum_str (cache, self->rename_icon);
  builder_cache_checksum_boolean (cache, self->copy_icon);
  builder_cache_checksum_str (cache, self->desktop_file_name_prefix);
  builder_cache_checksum_str (cache, self->desktop_file_name_suffix);
  builder_cache_checksum_boolean (cache, self->appstream_compose);

  for (l = self->expanded_modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      builder_module_checksum_for_cleanup (m, cache, context);
    }
}

void
builder_manifest_checksum_for_finish (BuilderManifest *self,
                                      BuilderCache    *cache,
                                      BuilderContext  *context)
{
  builder_cache_checksum_str (cache, BUILDER_MANIFEST_CHECKSUM_FINISH_VERSION);
  builder_cache_checksum_strv (cache, self->finish_args);
  builder_cache_checksum_str (cache, self->command);

  if (self->metadata)
    {
      GFile *base_dir = builder_context_get_base_dir (context);
      g_autoptr(GFile) metadata = g_file_resolve_relative_path (base_dir, self->metadata);
      g_autofree char *data = NULL;
      g_autoptr(GError) my_error = NULL;
      gsize len;

      if (g_file_load_contents (metadata, NULL, &data, &len, NULL, &my_error))
        builder_cache_checksum_data (cache, (guchar *) data, len);
      else
        g_warning ("Can't load metadata file %s: %s", self->metadata, my_error->message);
    }
}


void
builder_manifest_checksum_for_platform (BuilderManifest *self,
                                        BuilderCache    *cache,
                                        BuilderContext  *context)
{
  builder_cache_checksum_str (cache, BUILDER_MANIFEST_CHECKSUM_PLATFORM_VERSION);
  builder_cache_checksum_str (cache, self->id_platform);
  builder_cache_checksum_str (cache, self->runtime_commit);
  builder_cache_checksum_str (cache, self->metadata_platform);
  builder_cache_checksum_strv (cache, self->cleanup_platform);
  builder_cache_checksum_strv (cache, self->platform_extensions);

  if (self->metadata_platform)
    {
      GFile *base_dir = builder_context_get_base_dir (context);
      g_autoptr(GFile) metadata = g_file_resolve_relative_path (base_dir, self->metadata_platform);
      g_autofree char *data = NULL;
      g_autoptr(GError) my_error = NULL;
      gsize len;

      if (g_file_load_contents (metadata, NULL, &data, &len, NULL, &my_error))
        builder_cache_checksum_data (cache, (guchar *) data, len);
      else
        g_warning ("Can't load metadata-platform file %s: %s", self->metadata_platform, my_error->message);
    }
}

gboolean
builder_manifest_download (BuilderManifest *self,
                           gboolean         update_vcs,
                           BuilderContext  *context,
                           GError         **error)
{
  GList *l;

  g_print ("Downloading sources\n");
  for (l = self->expanded_modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;

      if (!builder_module_download_sources (m, update_vcs, context, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
builder_manifest_build (BuilderManifest *self,
                        BuilderCache    *cache,
                        BuilderContext  *context,
                        GError         **error)
{
  const char *stop_at = builder_context_get_stop_at (context);
  GList *l;

  builder_context_set_options (context, self->build_options);
  builder_context_set_global_cleanup (context, (const char **) self->cleanup);
  builder_context_set_global_cleanup_platform (context, (const char **) self->cleanup_platform);
  builder_context_set_build_runtime (context, self->build_runtime);
  builder_context_set_separate_locales (context, self->separate_locales);

  g_print ("Starting build of %s\n", self->id ? self->id : "app");
  for (l = self->expanded_modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      g_autoptr(GPtrArray) changes = NULL;
      const char *name = builder_module_get_name (m);

      g_autofree char *stage = g_strdup_printf ("build-%s", name);

      if (stop_at != NULL && strcmp (name, stop_at) == 0)
        {
          g_print ("Stopping at module %s\n", stop_at);
          return TRUE;
        }

      if (!builder_module_get_sources (m))
        {
          g_print ("Skipping module %s (no sources)\n", name);
          continue;
        }

      builder_module_checksum (m, cache, context);

      if (!builder_cache_lookup (cache, stage))
        {
          g_autofree char *body =
            g_strdup_printf ("Built %s\n", name);
          if (!builder_module_build (m, cache, context, error))
            return FALSE;
          if (!builder_cache_commit (cache, body, error))
            return FALSE;
        }
      else
        {
          g_print ("Cache hit for %s, skipping build\n", name);
        }

      changes = builder_cache_get_changes (cache, error);
      if (changes == NULL)
        return FALSE;

      builder_module_set_changes (m, changes);

      builder_module_update (m, context, error);
    }

  return TRUE;
}

static gboolean
command (GFile      *app_dir,
         char      **env_vars,
         const char *commandline,
         GError    **error)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GPtrArray) args = NULL;
  int i;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));

  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));

  if (env_vars)
    {
      for (i = 0; env_vars[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--env=%s", env_vars[i]));
    }

  g_ptr_array_add (args, g_file_get_path (app_dir));

  g_ptr_array_add (args, g_strdup ("/bin/sh"));
  g_ptr_array_add (args, g_strdup ("-c"));
  g_ptr_array_add (args, g_strdup (commandline));
  g_ptr_array_add (args, NULL);

  return builder_maybe_host_spawnv (NULL, NULL, error, (const char * const *)args->pdata);
}

typedef gboolean (*ForeachFileFunc) (BuilderManifest *self,
                                     int              source_parent_fd,
                                     const char      *source_name,
                                     const char      *full_dir,
                                     const char      *rel_dir,
                                     struct stat     *stbuf,
                                     gboolean        *found,
                                     int              depth,
                                     GError         **error);

static gboolean
foreach_file_helper (BuilderManifest *self,
                     ForeachFileFunc  func,
                     int              source_parent_fd,
                     const char      *source_name,
                     const char      *full_dir,
                     const char      *rel_dir,
                     gboolean        *found,
                     int              depth,
                     GError         **error)
{
  g_auto(GLnxDirFdIterator) source_iter = {0};
  struct dirent *dent;
  g_autoptr(GError) my_error = NULL;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, &my_error))
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;

      g_propagate_error (error, g_steal_pointer (&my_error));
      return FALSE;
    }

  while (TRUE)
    {
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, NULL, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            {
              continue;
            }
          else
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          g_autofree char *child_dir = g_build_filename (full_dir, dent->d_name, NULL);
          g_autofree char *child_rel_dir = g_build_filename (rel_dir, dent->d_name, NULL);
          if (!foreach_file_helper (self, func, source_iter.fd, dent->d_name, child_dir, child_rel_dir, found, depth + 1, error))
            return FALSE;
        }

      if (!func (self, source_iter.fd, dent->d_name, full_dir, rel_dir, &stbuf, found, depth, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
foreach_file (BuilderManifest *self,
              ForeachFileFunc  func,
              gboolean        *found,
              GFile           *root,
              GError         **error)
{
  return foreach_file_helper (self, func, AT_FDCWD,
                              flatpak_file_get_path_cached (root),
                              flatpak_file_get_path_cached (root),
                              "",
                              found, 0,
                              error);
}

static gboolean
rename_icon_cb (BuilderManifest *self,
                int              source_parent_fd,
                const char      *source_name,
                const char      *full_dir,
                const char      *rel_dir,
                struct stat     *stbuf,
                gboolean        *found,
                int              depth,
                GError         **error)
{
  if (S_ISREG (stbuf->st_mode) &&
      depth == 3 &&
      g_str_has_prefix (source_name, self->rename_icon) &&
      (g_str_has_prefix (source_name + strlen (self->rename_icon), ".") ||
       g_str_has_prefix (source_name + strlen (self->rename_icon), "-symbolic.")))
    {
      const char *extension = source_name + strlen (self->rename_icon);
      g_autofree char *new_name = g_strconcat (self->id, extension, NULL);
      int res;

      *found = TRUE;

      g_print ("%s icon %s/%s to %s/%s\n", self->copy_icon ? "Copying" : "Renaming", rel_dir, source_name, rel_dir, new_name);

      if (self->copy_icon)
        res = linkat (source_parent_fd, source_name, source_parent_fd, new_name, AT_SYMLINK_FOLLOW);
      else
        res = renameat (source_parent_fd, source_name, source_parent_fd, new_name);

      if (res != 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Can't rename icon %s/%s", rel_dir, source_name);
          return FALSE;
        }
    }

  return TRUE;
}

static int
cmpstringp (const void *p1, const void *p2)
{
  return strcmp (*(char * const *) p1, *(char * const *) p2);
}

static gboolean
appstream_compose (GFile   *app_dir,
                   GError **error,
                   ...)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GPtrArray) args = NULL;
  const gchar *arg;
  va_list ap;
  g_autoptr(GError) local_error = NULL;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));
  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));
  g_ptr_array_add (args, g_file_get_path (app_dir));
  g_ptr_array_add (args, g_strdup ("appstream-compose"));

  va_start (ap, error);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, g_strdup (arg));
  g_ptr_array_add (args, NULL);
  va_end (ap);

  if (!builder_maybe_host_spawnv (NULL, NULL, &local_error, (const char * const *)args->pdata))
    g_print ("WARNING: appstream-compose failed: %s\n", local_error->message);

  return TRUE;
}


gboolean
builder_manifest_cleanup (BuilderManifest *self,
                          BuilderCache    *cache,
                          BuilderContext  *context,
                          GError         **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);

  g_autoptr(GFile) app_root = NULL;
  GList *l;
  g_auto(GStrv) env = NULL;
  g_autoptr(GFile) appdata_dir = NULL;
  g_autofree char *appdata_basename = NULL;
  g_autoptr(GFile) appdata_file = NULL;
  int i;

  builder_manifest_checksum_for_cleanup (self, cache, context);
  if (!builder_cache_lookup (cache, "cleanup"))
    {
      g_autoptr(GHashTable) to_remove_ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_autofree char **keys = NULL;
      guint n_keys;

      g_print ("Cleaning up\n");

      if (self->cleanup_commands)
        {
          env = builder_options_get_env (self->build_options, context);
          for (i = 0; self->cleanup_commands[i] != NULL; i++)
            {
              if (!command (app_dir, env, self->cleanup_commands[i], error))
                return FALSE;
            }
        }

      for (l = self->expanded_modules; l != NULL; l = l->next)
        {
          BuilderModule *m = l->data;

          builder_module_cleanup_collect (m, FALSE, context, to_remove_ht);
        }

      keys = (char **) g_hash_table_get_keys_as_array (to_remove_ht, &n_keys);

      qsort (keys, n_keys, sizeof (char *), cmpstringp);
      /* Iterate in reverse to remove leafs first */
      for (i = n_keys - 1; i >= 0; i--)
        {
          g_autoptr(GError) my_error = NULL;
          g_autoptr(GFile) f = g_file_resolve_relative_path (app_dir, keys[i]);
          g_print ("Removing %s\n", keys[i]);
          if (!g_file_delete (f, NULL, &my_error))
            {
              if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
                  !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
                {
                  g_propagate_error (error, g_steal_pointer (&my_error));
                  return FALSE;
                }
            }
        }

      app_root = g_file_get_child (app_dir, "files");
      appdata_dir = g_file_resolve_relative_path (app_root, "share/appdata");
      appdata_basename = g_strdup_printf ("%s.appdata.xml", self->id);
      appdata_file = g_file_get_child (appdata_dir, appdata_basename);

      if (self->rename_appdata_file != NULL)
        {
          g_autoptr(GFile) src = g_file_get_child (appdata_dir, self->rename_appdata_file);

          g_print ("Renaming %s to %s\n", self->rename_appdata_file, appdata_basename);
          if (!g_file_move (src, appdata_file, 0, NULL, NULL, NULL, error))
            return FALSE;
        }

      if (self->rename_desktop_file != NULL)
        {
          g_autoptr(GFile) applications_dir = g_file_resolve_relative_path (app_root, "share/applications");
          g_autoptr(GFile) src = g_file_get_child (applications_dir, self->rename_desktop_file);
          g_autofree char *desktop_basename = g_strdup_printf ("%s.desktop", self->id);
          g_autoptr(GFile) dest = g_file_get_child (applications_dir, desktop_basename);

          g_print ("Renaming %s to %s\n", self->rename_desktop_file, desktop_basename);
          if (!g_file_move (src, dest, 0, NULL, NULL, NULL, error))
            return FALSE;

          if (g_file_query_exists (appdata_file, NULL))
            {
              g_autofree char *contents;
              const char *to_replace;
              const char *match;
              g_autoptr(GString) new_contents = NULL;

              if (!g_file_load_contents (appdata_file, NULL, &contents, NULL, NULL, error))
                return FALSE;

              new_contents = g_string_sized_new (strlen (contents));

              to_replace = contents;

              while ((match = strstr (to_replace, self->rename_desktop_file)) != NULL)
                {
                  g_string_append_len (new_contents, to_replace, match - to_replace);
                  g_string_append (new_contents, desktop_basename);
                  to_replace = match + strlen (self->rename_desktop_file);
                }

              g_string_append (new_contents, to_replace);

              if (!g_file_replace_contents (appdata_file,
                                            new_contents->str,
                                            new_contents->len,
                                            NULL,
                                            FALSE,
                                            G_FILE_CREATE_NONE,
                                            NULL,
                                            NULL, error))
                return FALSE;
            }
        }

      if (self->rename_icon)
        {
          gboolean found_icon = FALSE;
          g_autoptr(GFile) icons_dir = g_file_resolve_relative_path (app_root, "share/icons");

          if (!foreach_file (self, rename_icon_cb, &found_icon, icons_dir, error))
            return FALSE;

          if (!found_icon)
            {
              g_autofree char *icon_path = g_file_get_path (icons_dir);
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "icon %s not found below %s",
                           self->rename_icon, icon_path);
              return FALSE;
            }
        }

      if (self->rename_icon ||
          self->desktop_file_name_prefix ||
          self->desktop_file_name_suffix)
        {
          g_autoptr(GFile) applications_dir = g_file_resolve_relative_path (app_root, "share/applications");
          g_autofree char *desktop_basename = g_strdup_printf ("%s.desktop", self->id);
          g_autoptr(GFile) desktop = g_file_get_child (applications_dir, desktop_basename);
          g_autoptr(GKeyFile) keyfile = g_key_file_new ();
          g_autofree char *desktop_contents = NULL;
          gsize desktop_size;
          g_auto(GStrv) desktop_keys = NULL;

          g_print ("Rewriting contents of %s\n", desktop_basename);
          if (!g_file_load_contents (desktop, NULL,
                                     &desktop_contents, &desktop_size, NULL, error))
            {
              g_autofree char *desktop_path = g_file_get_path (desktop);
              g_prefix_error (error, "Can't load desktop file %s: ", desktop_path);
              return FALSE;
            }

          if (!g_key_file_load_from_data (keyfile,
                                          desktop_contents, desktop_size,
                                          G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                          error))
            return FALSE;

          if (self->rename_icon)
            {
              g_key_file_set_string (keyfile,
                                     G_KEY_FILE_DESKTOP_GROUP,
                                     G_KEY_FILE_DESKTOP_KEY_ICON,
                                     self->id);
            }

          if (self->desktop_file_name_suffix ||
              self->desktop_file_name_prefix)
            {
              desktop_keys = g_key_file_get_keys (keyfile,
                                                  G_KEY_FILE_DESKTOP_GROUP,
                                                  NULL, NULL);
              for (i = 0; desktop_keys[i]; i++)
                {
                  if (strcmp (desktop_keys[i], "Name") == 0 ||
                      g_str_has_prefix (desktop_keys[i], "Name["))
                    {
                      g_autofree char *name = g_key_file_get_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, desktop_keys[i], NULL);
                      if (name)
                        {
                          g_autofree char *new_name =
                            g_strdup_printf ("%s%s%s",
                                             self->desktop_file_name_prefix ? self->desktop_file_name_prefix : "",
                                             name,
                                             self->desktop_file_name_suffix ? self->desktop_file_name_suffix : "");
                          g_key_file_set_string (keyfile,
                                                 G_KEY_FILE_DESKTOP_GROUP,
                                                 desktop_keys[i],
                                                 new_name);
                        }
                    }
                }
            }

          g_free (desktop_contents);
          desktop_contents = g_key_file_to_data (keyfile, &desktop_size, error);
          if (desktop_contents == NULL)
            return FALSE;

          if (!g_file_replace_contents (desktop, desktop_contents, desktop_size, NULL, FALSE,
                                        0, NULL, NULL, error))
            return FALSE;
        }

      if (self->appstream_compose &&
          g_file_query_exists (appdata_file, NULL))
        {
          g_autofree char *basename_arg = g_strdup_printf ("--basename=%s", self->id);
          g_print ("Running appstream-compose\n");
          if (!appstream_compose (app_dir, error,
                                  self->build_runtime ?  "--prefix=/usr" : "--prefix=/app",
                                  "--origin=flatpak",
                                  basename_arg,
                                  self->id,
                                  NULL))
            return FALSE;
        }

      if (!builder_cache_commit (cache, "Cleanup", error))
        return FALSE;
    }
  else
    {
      g_print ("Cache hit for cleanup, skipping\n");
    }

  return TRUE;
}


gboolean
builder_manifest_finish (BuilderManifest *self,
                         BuilderCache    *cache,
                         BuilderContext  *context,
                         GError         **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);

  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(GFile) debuginfo_dir = NULL;
  g_autoptr(GFile) locale_parent_dir = NULL;
  g_autofree char *app_dir_path = g_file_get_path (app_dir);
  g_autofree char *json = NULL;
  g_autofree char *commandline = NULL;
  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  int i;
  JsonNode *node;
  JsonGenerator *generator;

  builder_manifest_checksum_for_finish (self, cache, context);
  if (!builder_cache_lookup (cache, "finish"))
    {
      g_print ("Finishing app\n");

      if (self->metadata)
        {
          GFile *base_dir = builder_context_get_base_dir (context);
          g_autoptr(GFile) dest_metadata = g_file_get_child (app_dir, "metadata");
          g_autoptr(GFile) src_metadata = g_file_resolve_relative_path (base_dir, self->metadata);

          if (!g_file_copy (src_metadata, dest_metadata, G_FILE_COPY_OVERWRITE, NULL,
                            NULL, NULL, error))
            return FALSE;
        }

      if (self->command)
        {
          g_autoptr(GFile) bin_dir = g_file_resolve_relative_path (app_dir, "files/bin");
          g_autoptr(GFile) bin_command = g_file_get_child (bin_dir, self->command);

          if (!g_file_query_exists (bin_command, NULL))
            {
              const char *help = "";

              if (strchr (self->command, ' '))
                help = ". Use a shell wrapper for passing arguments";

              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Command '%s' not found%s", self->command, help);

              return FALSE;
            }
        }

      args = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (args, g_strdup ("flatpak"));
      g_ptr_array_add (args, g_strdup ("build-finish"));
      if (self->command)
        g_ptr_array_add (args, g_strdup_printf ("--command=%s", self->command));

      if (self->finish_args)
        {
          for (i = 0; self->finish_args[i] != NULL; i++)
            g_ptr_array_add (args, g_strdup (self->finish_args[i]));
        }

      g_ptr_array_add (args, g_strdup (app_dir_path));
      g_ptr_array_add (args, NULL);

      commandline = g_strjoinv (" ", (char **) args->pdata);
      g_debug ("Running '%s'", commandline);

      subp =
        g_subprocess_newv ((const gchar * const *) args->pdata,
                           G_SUBPROCESS_FLAGS_NONE,
                           error);

      if (subp == NULL ||
          !g_subprocess_wait_check (subp, NULL, error))
        return FALSE;

      node = json_gobject_serialize (G_OBJECT (self));
      generator = json_generator_new ();
      json_generator_set_pretty (generator, TRUE);
      json_generator_set_root (generator, node);
      json = json_generator_to_data (generator, NULL);
      g_object_unref (generator);
      json_node_free (node);

      if (self->build_runtime)
        manifest_file = g_file_resolve_relative_path (app_dir, "usr/manifest.json");
      else
        manifest_file = g_file_resolve_relative_path (app_dir, "files/manifest.json");

      if (g_file_query_exists (manifest_file, NULL))
        {
          /* Move existing base manifest aside */
          g_autoptr(GFile) manifest_dir = g_file_get_parent (manifest_file);
          g_autoptr(GFile) old_manifest = NULL;
          int ver = 0;

          do
            {
              g_autofree char *basename = g_strdup_printf ("manifest-base-%d.json", ++ver);
              g_clear_object (&old_manifest);
              old_manifest = g_file_get_child (manifest_dir, basename);
            }
          while (g_file_query_exists (old_manifest, NULL));

          if (!g_file_move (manifest_file, old_manifest, 0,
                            NULL, NULL, NULL, error))
            return FALSE;
        }

      if (!g_file_replace_contents (manifest_file, json, strlen (json), NULL, FALSE,
                                    0, NULL, NULL, error))
        return FALSE;

      if (self->build_runtime)
        {
          debuginfo_dir = g_file_resolve_relative_path (app_dir, "usr/lib/debug");
          locale_parent_dir = g_file_resolve_relative_path (app_dir, "usr/" LOCALES_SEPARATE_DIR);
        }
      else
        {
          debuginfo_dir = g_file_resolve_relative_path (app_dir, "files/lib/debug");
          locale_parent_dir = g_file_resolve_relative_path (app_dir, "files/" LOCALES_SEPARATE_DIR);
        }

      if (self->separate_locales && g_file_query_exists (locale_parent_dir, NULL))
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autofree char *extension_contents = NULL;
          g_autoptr(GFileOutputStream) output = NULL;
          g_autoptr(GFile) metadata_locale_file = NULL;
          g_autofree char *metadata_contents = NULL;

          metadata_file = g_file_get_child (app_dir, "metadata");

          extension_contents = g_strdup_printf ("\n"
                                                "[Extension %s.Locale]\n"
                                                "directory=%s\n"
                                                "autodelete=true\n",
                                                self->id,
                                                LOCALES_SEPARATE_DIR);

          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                          extension_contents, strlen (extension_contents),
                                          NULL, NULL, error))
            return FALSE;


          metadata_locale_file = g_file_get_child (app_dir, "metadata.locale");
          metadata_contents = g_strdup_printf ("[Runtime]\n"
                                               "name=%s.Locale\n", self->id);
          if (!g_file_replace_contents (metadata_locale_file,
                                        metadata_contents, strlen (metadata_contents),
                                        NULL, FALSE,
                                        G_FILE_CREATE_REPLACE_DESTINATION,
                                        NULL, NULL, error))
            return FALSE;
        }


      if (g_file_query_exists (debuginfo_dir, NULL))
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autoptr(GFile) metadata_debuginfo_file = NULL;
          g_autofree char *metadata_contents = NULL;
          g_autofree char *extension_contents = NULL;
          g_autoptr(GFileOutputStream) output = NULL;

          metadata_file = g_file_get_child (app_dir, "metadata");
          metadata_debuginfo_file = g_file_get_child (app_dir, "metadata.debuginfo");

          extension_contents = g_strdup_printf ("\n"
                                                "[Extension %s.Debug]\n"
                                                "directory=lib/debug\n"
                                                "autodelete=true\n"
                                                "no-autodownload=true\n",
                                                self->id);

          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output), extension_contents, strlen (extension_contents),
                                          NULL, NULL, error))
            return FALSE;

          metadata_contents = g_strdup_printf ("[Runtime]\n"
                                               "name=%s.Debug\n", self->id);
          if (!g_file_replace_contents (metadata_debuginfo_file,
                                        metadata_contents, strlen (metadata_contents), NULL, FALSE,
                                        G_FILE_CREATE_REPLACE_DESTINATION,
                                        NULL, NULL, error))
            return FALSE;
        }

      if (!builder_cache_commit (cache, "Finish", error))
        return FALSE;
    }
  else
    {
      g_print ("Cache hit for finish, skipping\n");
    }

  return TRUE;
}

gboolean
builder_manifest_create_platform (BuilderManifest *self,
                                  BuilderCache    *cache,
                                  BuilderContext  *context,
                                  GError         **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autofree char *commandline = NULL;

  g_autoptr(GFile) locale_dir = NULL;
  int i;

  if (!self->build_runtime ||
      self->id_platform == NULL)
    return TRUE;

  builder_manifest_checksum_for_platform (self, cache, context);
  if (!builder_cache_lookup (cache, "platform"))
    {
      g_autoptr(GHashTable) to_remove_ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_autoptr(GPtrArray) changes = NULL;
      GList *l;
      g_autoptr(GFile) platform_dir = NULL;
      g_autoptr(GSubprocess) subp = NULL;
      g_autoptr(GPtrArray) args = NULL;

      g_print ("Creating platform based on %s\n", self->runtime);

      platform_dir = g_file_get_child (app_dir, "platform");

      args = g_ptr_array_new_with_free_func (g_free);

      g_ptr_array_add (args, g_strdup ("flatpak"));
      g_ptr_array_add (args, g_strdup ("build-init"));
      g_ptr_array_add (args, g_strdup ("--update"));
      g_ptr_array_add (args, g_strdup ("--writable-sdk"));
      g_ptr_array_add (args, g_strdup ("--sdk-dir=platform"));
      g_ptr_array_add (args, g_strdup_printf ("--arch=%s", builder_context_get_arch (context)));

      for (i = 0; self->platform_extensions != NULL && self->platform_extensions[i] != NULL; i++)
        {
          const char *ext = self->platform_extensions[i];
          g_ptr_array_add (args, g_strdup_printf ("--sdk-extension=%s", ext));
        }

      g_ptr_array_add (args, g_file_get_path (app_dir));
      g_ptr_array_add (args, g_strdup (self->id));
      g_ptr_array_add (args, g_strdup (self->runtime));
      g_ptr_array_add (args, g_strdup (self->runtime));
      g_ptr_array_add (args, g_strdup (builder_manifest_get_runtime_version (self)));

      g_ptr_array_add (args, NULL);

      commandline = g_strjoinv (" ", (char **) args->pdata);
      g_debug ("Running '%s'", commandline);

      subp =
        g_subprocess_newv ((const gchar * const *) args->pdata,
                           G_SUBPROCESS_FLAGS_NONE,
                           error);

      if (subp == NULL ||
          !g_subprocess_wait_check (subp, NULL, error))
        return FALSE;

      if (self->separate_locales)
        {
          g_autoptr(GFile) root_dir = NULL;

          root_dir = g_file_get_child (app_dir, "platform");

          if (!builder_migrate_locale_dirs (root_dir, error))
            return FALSE;

          locale_dir = g_file_resolve_relative_path (root_dir, LOCALES_SEPARATE_DIR);
        }

      if (self->metadata_platform)
        {
          GFile *base_dir = builder_context_get_base_dir (context);
          g_autoptr(GFile) dest_metadata = g_file_get_child (app_dir, "metadata.platform");
          g_autoptr(GFile) src_metadata = g_file_resolve_relative_path (base_dir, self->metadata_platform);

          if (!g_file_copy (src_metadata, dest_metadata, G_FILE_COPY_OVERWRITE, NULL,
                            NULL, NULL, error))
            return FALSE;
        }

      for (l = self->expanded_modules; l != NULL; l = l->next)
        {
          BuilderModule *m = l->data;

          builder_module_cleanup_collect (m, TRUE, context, to_remove_ht);
        }

      changes = builder_cache_get_all_changes (cache, error);
      if (changes == NULL)
        return FALSE;

      g_ptr_array_sort (changes, cmpstringp);

      for (i = 0; i < changes->len; i++)
        {
          const char *changed = g_ptr_array_index (changes, i);
          g_autoptr(GFile) src = NULL;
          g_autoptr(GFile) dest = NULL;
          g_autoptr(GFileInfo) info = NULL;
          g_autoptr(GError) my_error = NULL;

          if (!g_str_has_prefix (changed, "usr/"))
            continue;

          if (g_str_has_prefix (changed, "usr/lib/debug/") &&
              !g_str_equal (changed, "usr/lib/debug/app"))
            continue;

          if (g_hash_table_contains (to_remove_ht, changed))
            {
              g_print ("Ignoring %s\n", changed);
              continue;
            }

          src = g_file_resolve_relative_path (app_dir, changed);
          dest = g_file_resolve_relative_path (platform_dir, changed + strlen ("usr/"));

          info = g_file_query_info (src, "standard::type,standard::symlink-target",
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    NULL, &my_error);
          if (info == NULL)
            {
              if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                continue;

              g_propagate_error (error, g_steal_pointer (&my_error));
              return FALSE;
            }
          g_clear_error (&my_error);

          if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
            {
              if (!flatpak_mkdir_p (dest, NULL, error))
                return FALSE;
            }
          else
            {
              g_autoptr(GFile) dest_parent = g_file_get_parent (dest);

              if (!flatpak_mkdir_p (dest_parent, NULL, error))
                return FALSE;

              if (!g_file_delete (dest, NULL, &my_error) &&
                  !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                {
                  g_propagate_error (error, g_steal_pointer (&my_error));
                  return FALSE;
                }
              g_clear_error (&my_error);

              if (g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK)
                {
                  if (!g_file_make_symbolic_link (dest,
                                                  g_file_info_get_symlink_target (info),
                                                  NULL, error))
                    return FALSE;
                }
              else
                {
                  g_autofree char *src_path = g_file_get_path (src);
                  g_autofree char *dest_path = g_file_get_path (dest);

                  if (link (src_path, dest_path))
                    {
                      glnx_set_error_from_errno (error);
                      return FALSE;
                    }
                }
            }
        }

      if (self->separate_locales && locale_dir && g_file_query_exists (locale_dir, NULL))
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autofree char *extension_contents = NULL;
          g_autoptr(GFileOutputStream) output = NULL;
          g_autoptr(GFile) metadata_locale_file = NULL;
          g_autofree char *metadata_contents = NULL;

          metadata_file = g_file_get_child (app_dir, "metadata.platform");

          extension_contents = g_strdup_printf ("\n"
                                                "[Extension %s.Locale]\n"
                                                "directory=%s\n"
                                                "autodelete=true\n",
                                                self->id_platform,
                                                LOCALES_SEPARATE_DIR);

          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output),
                                          extension_contents, strlen (extension_contents),
                                          NULL, NULL, error))
            return FALSE;


          metadata_locale_file = g_file_get_child (app_dir, "metadata.platform.locale");
          metadata_contents = g_strdup_printf ("[Runtime]\n"
                                               "name=%s.Locale\n", self->id_platform);
          if (!g_file_replace_contents (metadata_locale_file,
                                        metadata_contents, strlen (metadata_contents),
                                        NULL, FALSE,
                                        G_FILE_CREATE_REPLACE_DESTINATION,
                                        NULL, NULL, error))
            return FALSE;
        }

      if (!builder_cache_commit (cache, "Created platform", error))
        return FALSE;
    }
  else
    {
      g_print ("Cache hit for create platform, skipping\n");
    }

  return TRUE;
}


gboolean
builder_manifest_run (BuilderManifest *self,
                      BuilderContext  *context,
                      FlatpakContext  *arg_context,
                      char           **argv,
                      int              argc,
                      GError         **error)
{
  g_autoptr(GPtrArray) args = NULL;
  g_autofree char *commandline = NULL;
  g_autofree char *build_dir_path = NULL;
  g_autofree char *ccache_dir_path = NULL;
  g_auto(GStrv) env = NULL;
  g_auto(GStrv) build_args = NULL;
  int i;

  if (!flatpak_mkdir_p (builder_context_get_build_dir (context),
                        NULL, error))
    return FALSE;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));

  build_dir_path = g_file_get_path (builder_context_get_build_dir (context));
  g_ptr_array_add (args, g_strdup_printf ("--bind-mount=/run/%s=%s",
                                          builder_context_get_build_runtime (context) ? "build-runtime" : "build",
                                          build_dir_path));

  if (g_file_query_exists (builder_context_get_ccache_dir (context), NULL))
    {
      ccache_dir_path = g_file_get_path (builder_context_get_ccache_dir (context));
      g_ptr_array_add (args, g_strdup_printf ("--bind-mount=/run/ccache=%s", ccache_dir_path));
    }

  build_args = builder_options_get_build_args (self->build_options, context, error);
  if (build_args == NULL)
    return FALSE;

  for (i = 0; build_args[i] != NULL; i++)
    g_ptr_array_add (args, g_strdup (build_args[i]));

  env = builder_options_get_env (self->build_options, context);
  if (env)
    {
      for (i = 0; env[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup_printf ("--env=%s", env[i]));
    }

  /* Inherit all finish args except the filesystem and command
   * ones so the command gets the same access as the final app
   */
  if (self->finish_args)
    {
      for (i = 0; self->finish_args[i] != NULL; i++)
        {
          const char *arg = self->finish_args[i];
          if (!g_str_has_prefix (arg, "--filesystem") &&
              !g_str_has_prefix (arg, "--command"))
            g_ptr_array_add (args, g_strdup (arg));
        }
    }

  flatpak_context_to_args (arg_context, args);

  g_ptr_array_add (args, g_file_get_path (builder_context_get_app_dir (context)));

  for (i = 0; i < argc; i++)
    g_ptr_array_add (args, g_strdup (argv[i]));
  g_ptr_array_add (args, NULL);

  commandline = g_strjoinv (" ", (char **) args->pdata);

  if (execvp ((char *) args->pdata[0], (char **) args->pdata) == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start flatpak build");
      return FALSE;
    }

  /* Not reached */
  return TRUE;
}
