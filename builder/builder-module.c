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

struct BuilderModule
{
  GObject         parent;

  char           *name;
  char           *subdir;
  char          **post_install;
  char          **config_opts;
  char          **make_args;
  char          **make_install_args;
  gboolean        disabled;
  gboolean        rm_configure;
  gboolean        no_autogen;
  gboolean        no_parallel_make;
  gboolean        no_python_timestamp_fix;
  gboolean        cmake;
  gboolean        builddir;
  BuilderOptions *build_options;
  GPtrArray      *changes;
  char          **cleanup;
  char          **cleanup_platform;
  GList          *sources;
  GList          *modules;
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
  PROP_NO_PYTHON_TIMESTAMP_FIX,
  PROP_CMAKE,
  PROP_BUILDDIR,
  PROP_CONFIG_OPTS,
  PROP_MAKE_ARGS,
  PROP_MAKE_INSTALL_ARGS,
  PROP_SOURCES,
  PROP_BUILD_OPTIONS,
  PROP_CLEANUP,
  PROP_CLEANUP_PLATFORM,
  PROP_POST_INSTALL,
  PROP_MODULES,
  LAST_PROP
};


static void
builder_module_finalize (GObject *object)
{
  BuilderModule *self = (BuilderModule *) object;

  g_free (self->name);
  g_free (self->subdir);
  g_strfreev (self->post_install);
  g_strfreev (self->config_opts);
  g_strfreev (self->make_args);
  g_strfreev (self->make_install_args);
  g_clear_object (&self->build_options);
  g_list_free_full (self->sources, g_object_unref);
  g_strfreev (self->cleanup);
  g_strfreev (self->cleanup_platform);
  g_list_free_full (self->modules, g_object_unref);

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

    case PROP_NO_PYTHON_TIMESTAMP_FIX:
      g_value_set_boolean (value, self->no_python_timestamp_fix);
      break;

    case PROP_CMAKE:
      g_value_set_boolean (value, self->cmake);
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

    case PROP_NO_PYTHON_TIMESTAMP_FIX:
      self->no_python_timestamp_fix = g_value_get_boolean (value);
      break;

    case PROP_CMAKE:
      self->cmake = g_value_get_boolean (value);
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
builder_module_download_sources (BuilderModule  *self,
                                 gboolean        update_vcs,
                                 BuilderContext *context,
                                 GError        **error)
{
  GList *l;

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

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

      if (!builder_source_extract (source, dest, self->build_options, context, error))
        {
          g_prefix_error (error, "module %s: ", self->name);
          return FALSE;
        }
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
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GPtrArray) args = NULL;
  const gchar *arg;
  const gchar **argv;
  g_autofree char *source_dir_path = g_file_get_path (source_dir);
  g_autofree char *source_dir_path_canonical = NULL;
  g_autofree char *ccache_dir_path = NULL;
  g_autoptr(GFile) source_dir_path_canonical_file = NULL;
  const char *builddir;
  va_list ap;
  int i;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build"));

  source_dir_path_canonical = canonicalize_file_name (source_dir_path);

  if (builder_context_get_build_runtime (context))
    builddir = "/run/build-runtime/";
  else
    builddir = "/run/build/";

  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));
  g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s", source_dir_path_canonical));

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
  g_ptr_array_add (args, NULL);
  va_end (ap);

  source_dir_path_canonical_file = g_file_new_for_path (source_dir_path_canonical);

  if (!builder_maybe_host_spawnv (source_dir_path_canonical_file, NULL, error, (const char * const *)args->pdata))
    {
      g_prefix_error (error, "module %s: ", module_name);
      return FALSE;
    }

  return TRUE;
}

static gboolean
builder_module_handle_debuginfo (BuilderModule  *self,
                                 GFile          *app_dir,
                                 BuilderCache   *cache,
                                 BuilderContext *context,
                                 GError        **error)
{
  g_autofree char *app_dir_path = g_file_get_path (app_dir);
  int i;

  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GPtrArray) modified = NULL;
  g_autoptr(GPtrArray) added_or_modified = g_ptr_array_new ();

  if (!builder_cache_get_outstanding_changes (cache, &added, &modified, NULL, error))
    return FALSE;

  for (i = 0; i < added->len; i++)
    g_ptr_array_add (added_or_modified, g_ptr_array_index (added, i));

  for (i = 0; i < modified->len; i++)
    g_ptr_array_add (added_or_modified, g_ptr_array_index (modified, i));

  g_ptr_array_sort (added_or_modified, flatpak_strcmp0_ptr);

  for (i = 0; i < added_or_modified->len; i++)
    {
      const char *rel_path = (char *) g_ptr_array_index (added_or_modified, i);
      g_autoptr(GFile) file = g_file_resolve_relative_path (app_dir, rel_path);
      g_autofree char *path = g_file_get_path (file);
      g_autofree char *debug_path = NULL;
      g_autofree char *real_debug_path = NULL;
      gboolean is_shared, is_stripped;

      if (is_elf_file (path, &is_shared, &is_stripped))
        {
          if (builder_options_get_strip (self->build_options, context))
            {
              g_print ("stripping: %s\n", rel_path);
              if (is_shared)
                {
                  if (!strip (error, "--remove-section=.comment", "--remove-section=.note", "--strip-unneeded", path, NULL))
                    {
                      g_prefix_error (error, "module %s: ", self->name);
                      return FALSE;
                    }
                }
              else
                {
                  if (!strip (error, "--remove-section=.comment", "--remove-section=.note", path, NULL))
                    {
                      g_prefix_error (error, "module %s: ", self->name);
                      return FALSE;
                    }
                }
            }
          else if (!builder_options_get_no_debuginfo (self->build_options, context))
            {
              g_autofree char *rel_path_dir = g_path_get_dirname (rel_path);
              g_autofree char *filename = g_path_get_basename (rel_path);
              g_autofree char *filename_debug = g_strconcat (filename, ".debug", NULL);
              g_autofree char *debug_dir = NULL;
              g_autofree char *source_dir_path = NULL;
              g_autoptr(GFile) source_dir = NULL;
              g_autofree char *real_debug_dir = NULL;

              if (g_str_has_prefix (rel_path_dir, "files/"))
                {
                  debug_dir = g_build_filename (app_dir_path, "files/lib/debug", rel_path_dir + strlen ("files/"), NULL);
                  real_debug_dir = g_build_filename ("/app/lib/debug", rel_path_dir + strlen ("files/"), NULL);
                  source_dir_path = g_build_filename (app_dir_path, "files/lib/debug/source", NULL);
                }
              else if (g_str_has_prefix (rel_path_dir, "usr/"))
                {
                  debug_dir = g_build_filename (app_dir_path, "usr/lib/debug", rel_path_dir, NULL);
                  real_debug_dir = g_build_filename ("/usr/lib/debug", rel_path_dir, NULL);
                  source_dir_path = g_build_filename (app_dir_path, "usr/lib/debug/source", NULL);
                }

              if (debug_dir)
                {
                  const char *builddir;
                  g_autoptr(GError) local_error = NULL;
                  g_auto(GStrv) file_refs = NULL;

                  if (g_mkdir_with_parents (debug_dir, 0755) != 0)
                    {
                      glnx_set_error_from_errno (error);
                      g_prefix_error (error, "module %s: ", self->name);
                      return FALSE;
                    }

                  source_dir = g_file_new_for_path (source_dir_path);
                  if (g_mkdir_with_parents (source_dir_path, 0755) != 0)
                    {
                      glnx_set_error_from_errno (error);
                      g_prefix_error (error, "module %s: ", self->name);
                      return FALSE;
                    }

                  if (builder_context_get_build_runtime (context))
                    builddir = "/run/build-runtime/";
                  else
                    builddir = "/run/build/";

                  debug_path = g_build_filename (debug_dir, filename_debug, NULL);
                  real_debug_path = g_build_filename (real_debug_dir, filename_debug, NULL);

                  file_refs = builder_get_debuginfo_file_references (path, &local_error);

                  if (file_refs == NULL)
                    {
                      g_warning ("%s", local_error->message);
                    }
                  else
                    {
                      GFile *build_dir = builder_context_get_build_dir (context);
                      int i;
                      for (i = 0; file_refs[i] != NULL; i++)
                        {
                          if (g_str_has_prefix (file_refs[i], builddir))
                            {
                              const char *relative_path = file_refs[i] + strlen (builddir);
                              g_autoptr(GFile) src = g_file_resolve_relative_path (build_dir, relative_path);
                              g_autoptr(GFile) dst = g_file_resolve_relative_path (source_dir, relative_path);
                              g_autoptr(GFile) dst_parent = g_file_get_parent (dst);
                              GFileType file_type;

                              if (!flatpak_mkdir_p (dst_parent, NULL, error))
                                {
                                  g_prefix_error (error, "module %s: ", self->name);
                                  return FALSE;
                                }

                              file_type = g_file_query_file_type (src, 0, NULL);
                              if (file_type == G_FILE_TYPE_DIRECTORY)
                                {
                                  if (!flatpak_mkdir_p (dst, NULL, error))
                                    {
                                      g_prefix_error (error, "module %s: ", self->name);
                                      return FALSE;
                                    }
                                }
                              else if (file_type == G_FILE_TYPE_REGULAR)
                                {
                                  if (!g_file_copy (src, dst,
                                                    G_FILE_COPY_OVERWRITE,
                                                    NULL, NULL, NULL, error))
                                    {
                                      g_prefix_error (error, "module %s: ", self->name);
                                      return FALSE;
                                    }
                                }
                            }
                        }
                    }

                  g_print ("stripping %s to %s\n", path, debug_path);
                  if (!eu_strip (error, "--remove-comment", "--reloc-debug-sections",
                                 "-f", debug_path,
                                 "-F", real_debug_path,
                                 path, NULL))
                    {
                      g_prefix_error (error, "module %s: ", self->name);
                      return FALSE;
                    }
                }
            }
        }
    }

  return TRUE;
}

static gboolean
fixup_python_timestamp (int dfd,
                        const char *rel_path,
                        const char *full_path,
                        GCancellable  *cancellable,
                        GError       **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  glnx_dirfd_iterator_init_at (dfd, rel_path, FALSE, &dfd_iter, NULL);

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, NULL) || dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        {
          g_autofree char *child_full_path = g_build_filename (full_path, dent->d_name, NULL);
          if (!fixup_python_timestamp (dfd_iter.fd, dent->d_name, child_full_path,
                                       cancellable, error))
            return FALSE;
        }
      else if (dent->d_type == DT_REG &&
               dfd != AT_FDCWD &&
               (g_str_has_suffix (dent->d_name, ".pyc") ||
                g_str_has_suffix (dent->d_name, ".pyo")))
        {
          glnx_fd_close int fd = -1;
          guint8 buffer[8];
          ssize_t res;
          guint32 pyc_mtime;
          g_autofree char *py_path = NULL;
          struct stat stbuf;
          gboolean remove_pyc = FALSE;

          fd = openat (dfd_iter.fd, dent->d_name, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
          if (fd == -1)
            {
              g_warning ("Can't open %s", dent->d_name);
              continue;
            }

          res = read (fd, buffer, 8);
          if (res != 8)
            {
              g_warning ("Short read for %s", dent->d_name);
              continue;
            }

          if (buffer[2] != 0x0d || buffer[3] != 0x0a)
            {
              g_debug ("Not matching python magic: %s", dent->d_name);
              continue;
            }

          pyc_mtime =
            (buffer[4] << 8*0) |
            (buffer[5] << 8*1) |
            (buffer[6] << 8*2) |
            (buffer[7] << 8*3);

          if (strcmp (rel_path, "__pycache__") == 0)
            {
              /* Python3 */
              g_autofree char *base = g_strdup (dent->d_name);
              char *dot;

              dot = strrchr (base, '.');
              if (dot == NULL)
                continue;
              *dot = 0;

              dot = strrchr (base, '.');
              if (dot == NULL)
                continue;
              *dot = 0;

              py_path = g_strconcat ("../", base, ".py", NULL);
            }
          else
            {
              /* Python2 */
              py_path = g_strndup (dent->d_name, strlen (dent->d_name) - 1);
            }

          /* Here we found a .pyc (or .pyo) file an a possible .py file that apply for it.
           * There are several possible cases wrt their mtimes:
           *
           * py not existing: pyc is stale, remove it
           * pyc mtime == 1: (.pyc is from an old commited module)
           *     py mtime == 1: Do nothing, already correct
           *     py mtime != 1: The py changed in this module, remove pyc
           * pyc mtime != 1: (.pyc changed this module, or was never rewritten in base layer)
           *     py == 1: Shouldn't happen in flatpak-builder, but could be an un-rewritten ctime lower layer, assume it matches and update timestamp
           *     py mtime != pyc mtime: new pyc doesn't match last py written in this module, remove it
           *     py mtime == pyc mtime: These match, but the py will be set to mtime 1 by ostree, so update timestamp in pyc.
           */

          if (fstatat (dfd_iter.fd, py_path, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
            {
              remove_pyc = TRUE;
            }
          else if (pyc_mtime == 1)
            {
              if (stbuf.st_mtime == 1)
                continue; /* Previously handled pyc */

              remove_pyc = TRUE;
            }
          else /* pyc_mtime != 1 */
            {
              if (pyc_mtime != stbuf.st_mtime && stbuf.st_mtime != 1)
                remove_pyc = TRUE;
              /* else change mtime */
            }

          if (remove_pyc)
            {
              g_autofree char *child_full_path = g_build_filename (full_path, dent->d_name, NULL);
              g_print ("Removing stale python bytecode file %s\n", child_full_path);
              if (unlinkat (dfd_iter.fd, dent->d_name, 0) != 0)
                g_warning ("Unable to delete %s", child_full_path);
              continue;
            }

          /* Change to mtime 1 which is what ostree uses for checkouts */
          buffer[4] = 1;
          buffer[5] = buffer[6] = buffer[7] = 0;

          res = pwrite (fd, buffer, 8, 0);
          if (res != 8)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }

          {
            g_autofree char *child_full_path = g_build_filename (full_path, dent->d_name, NULL);
            g_print ("Fixed up header mtime for %s\n", child_full_path);
          }

          /* The mtime will be zeroed on cache commit. We don't want to do that now, because multiple
             files could reference one .py file and we need the mtimes to match for them all */
        }
    }

  return TRUE;
}

gboolean
builder_module_build (BuilderModule  *self,
                      BuilderCache   *cache,
                      BuilderContext *context,
                      GError        **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autofree char *make_j = NULL;
  g_autofree char *make_l = NULL;

  g_autoptr(GFile) configure_file = NULL;
  g_autoptr(GFile) cmake_file = NULL;
  const char *makefile_names[] =  {"Makefile", "makefile", "GNUmakefile", NULL};
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
  int count;

  build_parent_dir = builder_context_get_build_dir (context);

  if (!flatpak_mkdir_p (build_parent_dir,
                        NULL, error))
    {
      g_prefix_error (error, "module %s: ", self->name);
      return FALSE;
    }

  for (count = 1; source_dir_path == NULL; count++)
    {
      g_autoptr(GFile) source_dir_count = NULL;

      g_free (buildname);
      buildname = g_strdup_printf ("%s-%d", self->name, count);

      source_dir_count = g_file_get_child (build_parent_dir, buildname);

      if (g_file_make_directory (source_dir_count, NULL, &my_error))
        {
          source_dir_path = g_file_get_path (source_dir_count);
        }
      else
        {
          if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              g_propagate_error (error, g_steal_pointer (&my_error));
              g_prefix_error (error, "module %s: ", self->name);
              return FALSE;
            }
          g_clear_error (&my_error);
          /* Already exists, try again */
        }
    }

  source_dir = g_file_new_for_path (source_dir_path);

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

  if (self->cmake)
    {
      cmake_file = g_file_get_child (source_subdir, "CMakeLists.txt");
      if (!g_file_query_exists (cmake_file, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "module: %s: Can't find CMakeLists.txt", self->name);
          return FALSE;
        }
      configure_file = g_object_ref (cmake_file);
    }
  else
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

  has_configure = g_file_query_exists (configure_file, NULL);

  if (!has_configure && !self->no_autogen)
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

  if (has_configure)
    {
      const char *configure_cmd;
      const char *configure_final_arg = skip_arg;
      g_autofree char *configure_prefix_arg = NULL;
      g_autofree char *configure_content = NULL;

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

          if (self->cmake)
            {
              configure_cmd = "cmake";
              configure_final_arg = "..";
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
          if (self->cmake)
            {
              configure_cmd = "cmake";
              configure_final_arg = ".";
            }
          else
            {
              configure_cmd = "./configure";
            }
        }

      if (self->cmake)
        configure_prefix_arg = g_strdup_printf ("-DCMAKE_INSTALL_PREFIX:PATH='%s'",
                                                builder_options_get_prefix (self->build_options, context));
      else
        configure_prefix_arg = g_strdup_printf ("--prefix=%s",
                                                builder_options_get_prefix (self->build_options, context));

      if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
                  configure_cmd, configure_prefix_arg, strv_arg, config_opts, configure_final_arg, NULL))
        return FALSE;
    }
  else
    {
      build_dir_relative = g_strdup (source_subdir_relative);
      build_dir = g_object_ref (source_subdir);
    }

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

  if (!self->no_parallel_make)
    {
      make_j = g_strdup_printf ("-j%d", builder_context_get_n_cpu (context));
      make_l = g_strdup_printf ("-l%d", 2 * builder_context_get_n_cpu (context));
    }

  /* Build and install */

  if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
              "make", make_j ? make_j : skip_arg, make_l ? make_l : skip_arg, strv_arg, self->make_args, NULL))
    return FALSE;

  if (!build (app_dir, self->name, context, source_dir, build_dir_relative, build_args, env, error,
              "make", "install", strv_arg, self->make_install_args, NULL))
    return FALSE;

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
    {
      if (!fixup_python_timestamp (AT_FDCWD,
                                   flatpak_file_get_path_cached (app_dir), "/",
                                   NULL,
                                   error))
        return FALSE;
    }

  if (!builder_module_handle_debuginfo (self, app_dir, cache, context, error))
    return FALSE;

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
  builder_cache_checksum_boolean (cache, self->rm_configure);
  builder_cache_checksum_boolean (cache, self->no_autogen);
  builder_cache_checksum_boolean (cache, self->disabled);
  builder_cache_checksum_boolean (cache, self->no_parallel_make);
  builder_cache_checksum_boolean (cache, self->no_python_timestamp_fix);
  builder_cache_checksum_boolean (cache, self->cmake);
  builder_cache_checksum_boolean (cache, self->builddir);

  if (self->build_options)
    builder_options_checksum (self->build_options, cache, context);

  for (l = self->sources; l != NULL; l = l->next)
    {
      BuilderSource *source = l->data;

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
