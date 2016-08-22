/* builder-options.c
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
#include <string.h>
#include <stdlib.h>
#include <sys/statfs.h>

#include "builder-options.h"
#include "builder-context.h"
#include "builder-utils.h"

struct BuilderOptions
{
  GObject     parent;

  gboolean    strip;
  gboolean    no_debuginfo;
  char       *cflags;
  char       *cxxflags;
  char       *prefix;
  char      **env;
  char      **build_args;
  char      **config_opts;
  GHashTable *arch;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderOptionsClass;

static void serializable_iface_init (JsonSerializableIface *serializable_iface);

G_DEFINE_TYPE_WITH_CODE (BuilderOptions, builder_options, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (JSON_TYPE_SERIALIZABLE, serializable_iface_init));

enum {
  PROP_0,
  PROP_CFLAGS,
  PROP_CXXFLAGS,
  PROP_PREFIX,
  PROP_ENV,
  PROP_STRIP,
  PROP_NO_DEBUGINFO,
  PROP_ARCH,
  PROP_BUILD_ARGS,
  PROP_CONFIG_OPTS,
  LAST_PROP
};


static void
builder_options_finalize (GObject *object)
{
  BuilderOptions *self = (BuilderOptions *) object;

  g_free (self->cflags);
  g_free (self->cxxflags);
  g_free (self->prefix);
  g_strfreev (self->env);
  g_strfreev (self->build_args);
  g_strfreev (self->config_opts);
  g_hash_table_destroy (self->arch);

  G_OBJECT_CLASS (builder_options_parent_class)->finalize (object);
}

static void
builder_options_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  BuilderOptions *self = BUILDER_OPTIONS (object);

  switch (prop_id)
    {
    case PROP_CFLAGS:
      g_value_set_string (value, self->cflags);
      break;

    case PROP_CXXFLAGS:
      g_value_set_string (value, self->cxxflags);
      break;

    case PROP_PREFIX:
      g_value_set_string (value, self->prefix);
      break;

    case PROP_ENV:
      g_value_set_boxed (value, self->env);
      break;

    case PROP_ARCH:
      g_value_set_boxed (value, self->arch);
      break;

    case PROP_BUILD_ARGS:
      g_value_set_boxed (value, self->build_args);
      break;

    case PROP_CONFIG_OPTS:
      g_value_set_boxed (value, self->config_opts);
      break;

    case PROP_STRIP:
      g_value_set_boolean (value, self->strip);
      break;

    case PROP_NO_DEBUGINFO:
      g_value_set_boolean (value, self->no_debuginfo);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_options_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  BuilderOptions *self = BUILDER_OPTIONS (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_CFLAGS:
      g_clear_pointer (&self->cflags, g_free);
      self->cflags = g_value_dup_string (value);
      break;

    case PROP_CXXFLAGS:
      g_clear_pointer (&self->cxxflags, g_free);
      self->cxxflags = g_value_dup_string (value);
      break;

    case PROP_PREFIX:
      g_clear_pointer (&self->prefix, g_free);
      self->prefix = g_value_dup_string (value);
      break;

    case PROP_ENV:
      tmp = self->env;
      self->env = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_ARCH:
      g_hash_table_destroy (self->arch);
      /* NOTE: This takes ownership of the hash table! */
      self->arch = g_value_dup_boxed (value);
      break;

    case PROP_BUILD_ARGS:
      tmp = self->build_args;
      self->build_args = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_CONFIG_OPTS:
      tmp = self->config_opts;
      self->config_opts = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_STRIP:
      self->strip = g_value_get_boolean (value);
      break;

    case PROP_NO_DEBUGINFO:
      self->no_debuginfo = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_options_class_init (BuilderOptionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_options_finalize;
  object_class->get_property = builder_options_get_property;
  object_class->set_property = builder_options_set_property;

  g_object_class_install_property (object_class,
                                   PROP_CFLAGS,
                                   g_param_spec_string ("cflags",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CXXFLAGS,
                                   g_param_spec_string ("cxxflags",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_PREFIX,
                                   g_param_spec_string ("prefix",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ENV,
                                   g_param_spec_boxed ("env",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ARCH,
                                   g_param_spec_boxed ("arch",
                                                       "",
                                                       "",
                                                       G_TYPE_HASH_TABLE,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUILD_ARGS,
                                   g_param_spec_boxed ("build-args",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_CONFIG_OPTS,
                                   g_param_spec_boxed ("config-opts",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_STRIP,
                                   g_param_spec_boolean ("strip",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_NO_DEBUGINFO,
                                   g_param_spec_boolean ("no-debuginfo",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
}

static void
builder_options_init (BuilderOptions *self)
{
  self->arch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

static JsonNode *
builder_options_serialize_property (JsonSerializable *serializable,
                                    const gchar      *property_name,
                                    const GValue     *value,
                                    GParamSpec       *pspec)
{
  if (strcmp (property_name, "arch") == 0)
    {
      BuilderOptions *self = BUILDER_OPTIONS (serializable);
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
  else if (strcmp (property_name, "env") == 0)
    {
      BuilderOptions *self = BUILDER_OPTIONS (serializable);
      JsonNode *retval = NULL;

      if (self->env && g_strv_length (self->env) > 0)
        {
          JsonObject *object;
          int i;

          object = json_object_new ();

          for (i = 0; self->env[i] != NULL; i++)
            {
              JsonNode *str = json_node_new (JSON_NODE_VALUE);
              const char *equal;
              g_autofree char *member = NULL;

              equal = strchr (self->env[i], '=');
              if (equal)
                {
                  json_node_set_string (str, equal + 1);
                  member = g_strndup (self->env[i], equal - self->env[i]);
                }
              else
                {
                  json_node_set_string (str, "");
                  member = g_strdup (self->env[i]);
                }

              json_object_set_member (object, member, str);
            }

          retval = json_node_init_object (json_node_alloc (), object);
          json_object_unref (object);
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
builder_options_deserialize_property (JsonSerializable *serializable,
                                      const gchar      *property_name,
                                      GValue           *value,
                                      GParamSpec       *pspec,
                                      JsonNode         *property_node)
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
          GList *l;

          members = json_object_get_members (object);
          for (l = members; l != NULL; l = l->next)
            {
              const char *member_name = l->data;
              JsonNode *val;
              GObject *option;

              val = json_object_get_member (object, member_name);
              option = json_gobject_deserialize (BUILDER_TYPE_OPTIONS, val);
              if (option == NULL)
                return FALSE;

              g_hash_table_insert (hash, g_strdup (member_name), option);
            }

          g_value_set_boxed (value, hash);
          return TRUE;
        }

      return FALSE;
    }
  else if (strcmp (property_name, "env") == 0)
    {
      if (JSON_NODE_TYPE (property_node) == JSON_NODE_NULL)
        {
          g_value_set_boxed (value, NULL);
          return TRUE;
        }
      else if (JSON_NODE_TYPE (property_node) == JSON_NODE_OBJECT)
        {
          JsonObject *object = json_node_get_object (property_node);
          g_autoptr(GPtrArray) env = g_ptr_array_new_with_free_func (g_free);
          g_autoptr(GList) members = NULL;
          GList *l;

          members = json_object_get_members (object);
          for (l = members; l != NULL; l = l->next)
            {
              const char *member_name = l->data;
              JsonNode *val;
              const char *val_str;

              val = json_object_get_member (object, member_name);
              val_str = json_node_get_string (val);
              if (val_str == NULL)
                return FALSE;

              g_ptr_array_add (env, g_strdup_printf ("%s=%s", member_name, val_str));
            }

          g_ptr_array_add (env, NULL);
          g_value_set_boxed (value, g_ptr_array_free (g_steal_pointer (&env), FALSE));
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
  serializable_iface->serialize_property = builder_options_serialize_property;
  serializable_iface->deserialize_property = builder_options_deserialize_property;
}

static GList *
get_arched_options (BuilderOptions *self, BuilderContext *context)
{
  GList *options = NULL;
  const char *arch = builder_context_get_arch (context);
  BuilderOptions *arch_options;

  arch_options = g_hash_table_lookup (self->arch, arch);
  if (arch_options)
    options = g_list_prepend (options, arch_options);

  options = g_list_prepend (options, self);

  return options;
}

static GList *
get_all_options (BuilderOptions *self, BuilderContext *context)
{
  GList *options = NULL;
  BuilderOptions *global_options = builder_context_get_options (context);

  if (self)
    options = get_arched_options (self, context);

  if (global_options && global_options != self)
    options = g_list_concat (options,  get_arched_options (global_options, context));

  return options;
}

const char *
builder_options_get_cflags (BuilderOptions *self, BuilderContext *context)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;
      if (o->cflags)
        return o->cflags;
    }

  return NULL;
}

const char *
builder_options_get_cxxflags (BuilderOptions *self, BuilderContext *context)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;
      if (o->cxxflags)
        return o->cxxflags;
    }

  return NULL;
}

const char *
builder_options_get_prefix (BuilderOptions *self, BuilderContext *context)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;
      if (o->prefix)
        return o->prefix;
    }

  if (builder_context_get_build_runtime (context))
    return "/usr";

  return "/app";
}

gboolean
builder_options_get_strip (BuilderOptions *self, BuilderContext *context)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;
      if (o->strip)
        return TRUE;
    }

  return FALSE;
}

gboolean
builder_options_get_no_debuginfo (BuilderOptions *self, BuilderContext *context)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;
      if (o->no_debuginfo)
        return TRUE;
    }

  return FALSE;
}

char **
builder_options_get_env (BuilderOptions *self, BuilderContext *context)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;
  int i;
  char **envp = NULL;
  const char *cflags, *cxxflags;

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;

      if (o->env)
        {
          for (i = 0; o->env[i] != NULL; i++)
            {
              const char *line = o->env[i];
              const char *eq = strchr (line, '=');
              const char *value = "";
              g_autofree char *key = NULL;

              if (eq)
                {
                  key = g_strndup (line, eq - line);
                  value = eq + 1;
                }
              else
                {
                  key = g_strdup (key);
                }

              envp = g_environ_setenv (envp, key, value, FALSE);
            }
        }
    }

  envp = builder_context_extend_env (context, envp);

  cflags = builder_options_get_cflags (self, context);
  if (cflags)
    envp = g_environ_setenv (envp, "CFLAGS", cflags, TRUE);

  cxxflags = builder_options_get_cxxflags (self, context);
  if (cxxflags)
    envp = g_environ_setenv (envp, "CXXFLAGS", cxxflags, TRUE);

  return envp;
}

char **
builder_options_get_build_args (BuilderOptions *self,
                                BuilderContext *context,
                                GError **error)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;
  int i;
  g_autoptr(GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

  /* Last argument wins, so reverse the list for per-module to win */
  options = g_list_reverse (options);

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;

      if (o->build_args)
        {
          for (i = 0; o->build_args[i] != NULL; i++)
            g_ptr_array_add (array, g_strdup (o->build_args[i]));
        }
    }

  if (array->len > 0 && builder_context_get_sandboxed (context))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't specify build-args in sandboxed build");
      return NULL;
    }

  g_ptr_array_add (array, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&array), FALSE);
}

char **
builder_options_get_config_opts (BuilderOptions *self,
                                 BuilderContext *context,
                                 char          **base_opts)
{
  g_autoptr(GList) options = get_all_options (self, context);
  GList *l;
  int i;
  g_autoptr(GPtrArray) array = g_ptr_array_new_with_free_func (g_free);

  /* Last argument wins, so reverse the list for per-module to win */
  options = g_list_reverse (options);

  /* Start by adding the base options */
  if (base_opts)
    {
      for (i = 0; base_opts[i] != NULL; i++)
	g_ptr_array_add (array, g_strdup (base_opts[i]));
    }

  for (l = options; l != NULL; l = l->next)
    {
      BuilderOptions *o = l->data;

      if (o->config_opts)
        {
          for (i = 0; o->config_opts[i] != NULL; i++)
            g_ptr_array_add (array, g_strdup (o->config_opts[i]));
        }
    }

  g_ptr_array_add (array, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&array), FALSE);
}

void
builder_options_checksum (BuilderOptions *self,
                          BuilderCache   *cache,
                          BuilderContext *context)
{
  BuilderOptions *arch_options;

  builder_cache_checksum_str (cache, BUILDER_OPTION_CHECKSUM_VERSION);
  builder_cache_checksum_str (cache, self->cflags);
  builder_cache_checksum_str (cache, self->cxxflags);
  builder_cache_checksum_str (cache, self->prefix);
  builder_cache_checksum_strv (cache, self->env);
  builder_cache_checksum_strv (cache, self->build_args);
  builder_cache_checksum_strv (cache, self->config_opts);
  builder_cache_checksum_boolean (cache, self->strip);
  builder_cache_checksum_boolean (cache, self->no_debuginfo);

  arch_options = g_hash_table_lookup (self->arch, builder_context_get_arch (context));
  if (arch_options)
    builder_options_checksum (arch_options, cache, context);
}
