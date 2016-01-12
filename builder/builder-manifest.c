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
#include "xdg-app-utils.h"

#include "libgsystem.h"
#include "libglnx/libglnx.h"

struct BuilderManifest {
  GObject parent;

  char *app_id;
  char *branch;
  char *runtime;
  char *runtime_version;
  char *sdk;
  char **cleanup;
  char **cleanup_commands;
  char **finish_args;
  char *rename_desktop_file;
  char *rename_appdata_file;
  char *rename_icon;
  gboolean copy_icon;
  char *desktop_file_name_prefix;
  char *desktop_file_name_suffix;
  gboolean writable_sdk;
  char *command;
  BuilderOptions *build_options;
  GList *modules;
};

typedef struct {
  GObjectClass parent_class;
} BuilderManifestClass;

static void serializable_iface_init (JsonSerializableIface *serializable_iface);

G_DEFINE_TYPE_WITH_CODE (BuilderManifest, builder_manifest, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (JSON_TYPE_SERIALIZABLE, serializable_iface_init));

enum {
  PROP_0,
  PROP_APP_ID,
  PROP_BRANCH,
  PROP_RUNTIME,
  PROP_RUNTIME_VERSION,
  PROP_SDK,
  PROP_BUILD_OPTIONS,
  PROP_COMMAND,
  PROP_MODULES,
  PROP_CLEANUP,
  PROP_CLEANUP_COMMANDS,
  PROP_WRITABLE_SDK,
  PROP_FINISH_ARGS,
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
  BuilderManifest *self = (BuilderManifest *)object;

  g_free (self->app_id);
  g_free (self->branch);
  g_free (self->runtime);
  g_free (self->runtime_version);
  g_free (self->sdk);
  g_free (self->command);
  g_clear_object (&self->build_options);
  g_list_free_full (self->modules, g_object_unref);
  g_strfreev (self->cleanup);
  g_strfreev (self->cleanup_commands);
  g_strfreev (self->finish_args);
  g_free (self->rename_desktop_file);
  g_free (self->rename_appdata_file);
  g_free (self->rename_icon);
  g_free (self->desktop_file_name_prefix);
  g_free (self->desktop_file_name_suffix);

  G_OBJECT_CLASS (builder_manifest_parent_class)->finalize (object);
}

static void
builder_manifest_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  BuilderManifest *self = BUILDER_MANIFEST(object);

  switch (prop_id)
    {
    case PROP_APP_ID:
      g_value_set_string (value, self->app_id);
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

    case PROP_FINISH_ARGS:
      g_value_set_boxed (value, self->finish_args);
      break;

    case PROP_WRITABLE_SDK:
      g_value_set_boolean (value, self->writable_sdk);
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
builder_manifest_set_property (GObject       *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  BuilderManifest *self = BUILDER_MANIFEST (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_APP_ID:
      g_free (self->app_id);
      self->app_id = g_value_dup_string (value);
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

    case PROP_FINISH_ARGS:
      tmp = self->finish_args;
      self->finish_args = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_WRITABLE_SDK:
      self->writable_sdk = g_value_get_boolean (value);
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
                                   PROP_FINISH_ARGS,
                                   g_param_spec_boxed ("finish-args",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_WRITABLE_SDK,
                                   g_param_spec_boolean ("writable-sdk",
                                                         "",
                                                         "",
                                                         FALSE,
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
    return json_serializable_default_serialize_property (serializable,
                                                         property_name,
                                                         value,
                                                         pspec);
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

              if (JSON_NODE_TYPE (element_node) != JSON_NODE_OBJECT)
                {
                  g_list_free_full (modules, g_object_unref);
                  return FALSE;
                }

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
    return json_serializable_default_deserialize_property (serializable,
                                                           property_name,
                                                           value,
                                                           pspec, property_node);
}

static void
serializable_iface_init (JsonSerializableIface *serializable_iface)
{
  serializable_iface->serialize_property = builder_manifest_serialize_property;
  serializable_iface->deserialize_property = builder_manifest_deserialize_property;
}

const char *
builder_manifest_get_app_id  (BuilderManifest *self)
{
  return self->app_id;
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

gboolean
builder_manifest_init_app_dir (BuilderManifest *self,
                               BuilderContext *context,
                               GError **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autofree char *app_dir_path = g_file_get_path (app_dir);
  g_autoptr(GSubprocess) subp = NULL;

  if (self->app_id == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "app id not specified");
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

  subp =
    g_subprocess_new (G_SUBPROCESS_FLAGS_NONE,
                      error,
                      "xdg-app",
                      "build-init",
                      app_dir_path,
                      self->app_id,
                      self->sdk,
                      self->runtime,
                      builder_manifest_get_runtime_version (self),
                      self->writable_sdk ? "-w" : NULL,
                      NULL);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  return TRUE;
}

/* This gets the checksum of everything that globally affects the build */
void
builder_manifest_checksum (BuilderManifest *self,
                           BuilderCache *cache,
                           BuilderContext *context)
{
  builder_cache_checksum_str (cache, BUILDER_MANIFEST_CHECKSUM_VERSION);
  builder_cache_checksum_str (cache, self->app_id);
  /* No need to include version here, it doesn't affect the build */
  builder_cache_checksum_str (cache, self->runtime);
  builder_cache_checksum_str (cache, builder_manifest_get_runtime_version (self));
  builder_cache_checksum_str (cache, self->sdk);
  if (self->build_options)
    builder_options_checksum (self->build_options, cache, context);
}

void
builder_manifest_checksum_for_cleanup (BuilderManifest *self,
                                       BuilderCache *cache,
                                       BuilderContext *context)
{
  GList *l;

  builder_cache_checksum_str (cache, BUILDER_MANIFEST_CHECKSUM_VERSION);
  builder_cache_checksum_strv (cache, self->cleanup);
  builder_cache_checksum_strv (cache, self->cleanup_commands);
  builder_cache_checksum_str (cache, self->rename_desktop_file);
  builder_cache_checksum_str (cache, self->rename_appdata_file);
  builder_cache_checksum_str (cache, self->rename_icon);
  builder_cache_checksum_boolean (cache, self->copy_icon);
  builder_cache_checksum_str (cache, self->desktop_file_name_prefix);
  builder_cache_checksum_str (cache, self->desktop_file_name_suffix);
  builder_cache_checksum_boolean (cache, self->writable_sdk);

  for (l = self->modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      builder_module_checksum_for_cleanup (m, cache, context);
    }
}

void
builder_manifest_checksum_for_finish (BuilderManifest *self,
                                      BuilderCache *cache,
                                      BuilderContext *context)
{
  builder_cache_checksum_str (cache, BUILDER_MANIFEST_CHECKSUM_VERSION " foo");
  builder_cache_checksum_strv (cache, self->finish_args);
  builder_cache_checksum_str (cache, self->command);
}

gboolean
builder_manifest_download (BuilderManifest *self,
                           gboolean update_vcs,
                           BuilderContext *context,
                           GError **error)
{
  GList *l;

  g_print ("Downloading sources\n");
  for (l = self->modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;

      if (! builder_module_download_sources (m, update_vcs, context, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
builder_manifest_build (BuilderManifest *self,
                        BuilderCache *cache,
                        BuilderContext *context,
                        GError **error)
{
  GList *l;

  builder_context_set_options (context, self->build_options);
  builder_context_set_global_cleanup (context, (const char **)self->cleanup);

  g_print ("Starting build of %s\n", self->app_id ? self->app_id : "app");
  for (l = self->modules; l != NULL; l = l->next)
    {
      BuilderModule *m = l->data;
      g_autoptr(GPtrArray) changes = NULL;

      builder_module_checksum (m, cache, context);

      if (!builder_cache_lookup (cache))
        {
          g_autofree char *body =
            g_strdup_printf ("Built %s\n", builder_module_get_name (m));
          if (!builder_module_build (m, cache, context, error))
            return FALSE;
          if (!builder_cache_commit (cache, body, error))
            return FALSE;
        }
      else
        g_print ("Cache hit for %s, skipping build\n",
                 builder_module_get_name (m));

      changes = builder_cache_get_changes (cache, error);
      if (changes == NULL)
        return FALSE;

      builder_module_set_changes (m, changes);

      builder_module_update (m, context, error);
    }

  return TRUE;
}

static gboolean
command (GFile *app_dir,
         char **env_vars,
         const char *commandline,
         GError **error)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GPtrArray) args = NULL;
  int i;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("xdg-app"));
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

  g_print ("Running: %s\n", commandline);

  launcher = g_subprocess_launcher_new (0);

  subp = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) args->pdata, error);
  g_ptr_array_free (args, TRUE);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  return TRUE;
}

typedef gboolean (*ForeachFileFunc) (BuilderManifest *self,
                                     int            source_parent_fd,
                                     const char    *source_name,
                                     const char    *full_dir,
                                     const char    *rel_dir,
                                     struct stat   *stbuf,
                                     gboolean      *found,
                                     int            depth,
                                     GError       **error);

static gboolean
foreach_file_helper (BuilderManifest *self,
                     ForeachFileFunc func,
                     int            source_parent_fd,
                     const char    *source_name,
                     const char    *full_dir,
                     const char    *rel_dir,
                     gboolean      *found,
                     int            depth,
                     GError       **error)
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
            continue;
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
              ForeachFileFunc func,
              gboolean      *found,
              GFile         *root,
              GError       **error)
{
  return foreach_file_helper (self, func, AT_FDCWD,
                              gs_file_get_path_cached (root),
                              gs_file_get_path_cached (root),
                              "",
                              found, 0,
                              error);
}

static gboolean
rename_icon_cb (BuilderManifest *self,
                int            source_parent_fd,
                const char    *source_name,
                const char    *full_dir,
                const char    *rel_dir,
                struct stat   *stbuf,
                gboolean      *found,
                int            depth,
                GError       **error)
{
  if (S_ISREG (stbuf->st_mode) &&
      depth == 3 &&
      g_str_has_prefix (source_name, self->rename_icon) &&
      source_name[strlen (self->rename_icon)] == '.')
    {
      const char *extension = source_name + strlen (self->rename_icon);
      g_autofree char *new_name = g_strconcat (self->app_id, extension, NULL);
      int res;

      *found = TRUE;

      g_print ("%s icon %s/%s to %s/%s\n", self->copy_icon ? "copying" : "renaming", rel_dir, source_name, rel_dir, new_name);

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
  return strcmp (* (char * const *) p1, * (char * const *) p2);
}

gboolean
builder_manifest_cleanup (BuilderManifest *self,
                          BuilderCache *cache,
                          BuilderContext *context,
                          GError **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autoptr(GFile) app_root = NULL;
  g_autoptr(GHashTable) to_remove_ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GList *l;
  g_autofree char **keys = NULL;
  g_auto(GStrv) env = NULL;
  guint n_keys;
  g_autoptr(GFile) appdata_dir = NULL;
  g_autofree char *appdata_basename = NULL;
  g_autoptr(GFile) appdata_file = NULL;

  int i;

  builder_manifest_checksum_for_cleanup (self, cache, context);
  if (!builder_cache_lookup (cache))
    {
      app_root = g_file_get_child (app_dir, "files");

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

      for (l = self->modules; l != NULL; l = l->next)
        {
          BuilderModule *m = l->data;

          builder_module_cleanup_collect (m, context, to_remove_ht);
        }

      keys = (char **)g_hash_table_get_keys_as_array (to_remove_ht, &n_keys);

      qsort (keys, n_keys, sizeof (char *), cmpstringp);
      /* Iterate in reverse to remove leafs first */
      for (i = n_keys - 1; i >= 0; i--)
        {
          g_autoptr(GError) my_error = NULL;
          g_autoptr(GFile) f = g_file_resolve_relative_path (app_root, keys[i]);
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

      appdata_dir = g_file_resolve_relative_path (app_root, "share/appdata");
      appdata_basename = g_strdup_printf ("%s.appdata.xml", self->app_id);
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
          g_autofree char *desktop_basename = g_strdup_printf ("%s.desktop", self->app_id);
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

              while ( (match = strstr (to_replace, self->rename_desktop_file)) != NULL)
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
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "icon %s not found", self->rename_icon);
              return FALSE;
            }
        }

      if (self->rename_icon ||
          self->desktop_file_name_prefix ||
          self->desktop_file_name_suffix)
        {
          g_autoptr(GFile) applications_dir = g_file_resolve_relative_path (app_root, "share/applications");
          g_autofree char *desktop_basename = g_strdup_printf ("%s.desktop", self->app_id);
          g_autoptr(GFile) desktop = g_file_get_child (applications_dir, desktop_basename);
          g_autoptr(GKeyFile) keyfile = g_key_file_new ();
          g_autofree char *desktop_contents = NULL;
          gsize desktop_size;
          g_auto(GStrv) desktop_keys = NULL;

          if (!g_file_load_contents (desktop, NULL,
                                     &desktop_contents, &desktop_size, NULL, error))
            return FALSE;

          if (!g_key_file_load_from_data (keyfile,
                                          desktop_contents, desktop_size,
                                          G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                          error))
            return FALSE;

          if (self->rename_icon)
            g_key_file_set_string (keyfile,
                                   G_KEY_FILE_DESKTOP_GROUP,
                                   G_KEY_FILE_DESKTOP_KEY_ICON,
                                   self->app_id);

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

      if (!builder_cache_commit (cache, "Cleanup", error))
        return FALSE;
    }
  else
    g_print ("Cache hit for cleanup, skipping\n");

  return TRUE;
}


gboolean
builder_manifest_finish (BuilderManifest *self,
                         BuilderCache *cache,
                         BuilderContext *context,
                         GError **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(GFile) debuginfo_dir = NULL;
  g_autofree char *app_dir_path = g_file_get_path (app_dir);
  g_autofree char *json = NULL;
  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  int i;
  JsonNode *node;
  JsonGenerator *generator;

  builder_manifest_checksum_for_finish (self, cache, context);
  if (!builder_cache_lookup (cache))
    {
      g_print ("Finishing app\n");

      args = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (args, g_strdup ("xdg-app"));
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
      manifest_file = g_file_resolve_relative_path (app_dir, "files/manifest.json");

      if (!g_file_replace_contents (manifest_file, json, strlen (json), NULL, FALSE,
                                    0, NULL, NULL, error))
        return FALSE;


      debuginfo_dir = g_file_resolve_relative_path (app_dir, "files/lib/debug");

      if (g_file_query_exists (debuginfo_dir, NULL))
        {
          g_autoptr(GFile) metadata_file = NULL;
          g_autoptr(GFile) metadata_debuginfo_file = NULL;
          g_autofree char *metadata_contents = NULL;
          g_autofree char *extension_contents = NULL;
          g_autoptr(GFileOutputStream) output = NULL;

          metadata_file = g_file_get_child (app_dir, "metadata");
          metadata_debuginfo_file = g_file_get_child (app_dir, "metadata.debuginfo");

          extension_contents = g_strdup_printf("\n"
                                               "[Extension %s.Debug]\n"
                                               "directory=lib/debug\n",
                                               self->app_id);

          output = g_file_append_to (metadata_file, G_FILE_CREATE_NONE, NULL, error);
          if (output == NULL)
            return FALSE;

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (output), extension_contents, strlen (extension_contents),
                                          NULL, NULL, error))
            return FALSE;

          metadata_contents = g_strdup_printf("[Runtime]\n"
                                              "name=%s.Debug\n", self->app_id);
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
    g_print ("Cache hit for finish, skipping\n");

  return TRUE;
}
