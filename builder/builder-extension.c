/* builder-extension.c
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

#include <glib/gi18n.h>
#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-utils.h"
#include "flatpak-run.h"
#include "builder-utils.h"
#include "builder-extension.h"

struct BuilderExtension
{
  GObject         parent;

  char           *name;
  char           *directory;
  gboolean        bundle;
  gboolean        autodelete;
  gboolean        no_autodownload;
  gboolean        subdirectories;
  char           *add_ld_path;
  char           *download_if;
  char           *enable_if;
  char           *merge_dirs;
  char           *subdirectory_suffix;
  char           *version;
  char           *versions;
};

typedef struct
{
  GObjectClass parent_class;
} BuilderExtensionClass;

G_DEFINE_TYPE (BuilderExtension, builder_extension, G_TYPE_OBJECT);

enum {
  PROP_0,
  PROP_DIRECTORY,
  PROP_BUNDLE,
  PROP_AUTODELETE,
  PROP_ADD_LD_PATH,
  PROP_DOWNLOAD_IF,
  PROP_ENABLE_IF,
  PROP_MERGE_DIRS,
  PROP_NO_AUTODOWNLOAD,
  PROP_SUBDIRECTORIES,
  PROP_SUBDIRECTORY_SUFFIX,
  PROP_VERSION,
  PROP_VERSIONS,
  LAST_PROP
};

static void
builder_extension_finalize (GObject *object)
{
  BuilderExtension *self = (BuilderExtension *) object;

  g_free (self->name);
  g_free (self->directory);
  g_free (self->add_ld_path);
  g_free (self->download_if);
  g_free (self->enable_if);
  g_free (self->merge_dirs);
  g_free (self->subdirectory_suffix);
  g_free (self->version);
  g_free (self->versions);

  G_OBJECT_CLASS (builder_extension_parent_class)->finalize (object);
}

static void
builder_extension_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  BuilderExtension *self = BUILDER_EXTENSION (object);

  switch (prop_id)
    {
    case PROP_DIRECTORY:
      g_value_set_string (value, self->directory);
      break;

    case PROP_BUNDLE:
      g_value_set_boolean (value, self->bundle);
      break;

    case PROP_AUTODELETE:
      g_value_set_boolean (value, self->autodelete);
      break;

    case PROP_NO_AUTODOWNLOAD:
      g_value_set_boolean (value, self->autodelete);
      break;

    case PROP_SUBDIRECTORIES:
      g_value_set_boolean (value, self->autodelete);
      break;

    case PROP_ADD_LD_PATH:
      g_value_set_string (value, self->add_ld_path);
      break;

    case PROP_DOWNLOAD_IF:
      g_value_set_string (value, self->download_if);
      break;

    case PROP_ENABLE_IF:
      g_value_set_string (value, self->enable_if);
      break;

    case PROP_MERGE_DIRS:
      g_value_set_string (value, self->merge_dirs);
      break;

    case PROP_SUBDIRECTORY_SUFFIX:
      g_value_set_string (value, self->subdirectory_suffix);
      break;

    case PROP_VERSION:
      g_value_set_string (value, self->version);
      break;

    case PROP_VERSIONS:
      g_value_set_string (value, self->versions);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_extension_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  BuilderExtension *self = BUILDER_EXTENSION (object);

  switch (prop_id)
    {
    case PROP_DIRECTORY:
      g_clear_pointer (&self->directory, g_free);
      self->directory = g_value_dup_string (value);
      break;

    case PROP_BUNDLE:
      self->bundle = g_value_get_boolean (value);
      break;

    case PROP_AUTODELETE:
      self->autodelete = g_value_get_boolean (value);
      break;

    case PROP_NO_AUTODOWNLOAD:
      self->no_autodownload = g_value_get_boolean (value);
      break;

    case PROP_SUBDIRECTORIES:
      self->subdirectories = g_value_get_boolean (value);
      break;

    case PROP_ADD_LD_PATH:
      g_clear_pointer (&self->add_ld_path, g_free);
      self->add_ld_path = g_value_dup_string (value);
      break;

    case PROP_DOWNLOAD_IF:
      g_clear_pointer (&self->download_if, g_free);
      self->download_if = g_value_dup_string (value);
      break;

    case PROP_ENABLE_IF:
      g_clear_pointer (&self->enable_if, g_free);
      self->enable_if = g_value_dup_string (value);
      break;

    case PROP_MERGE_DIRS:
      g_clear_pointer (&self->merge_dirs, g_free);
      self->merge_dirs = g_value_dup_string (value);
      break;

    case PROP_SUBDIRECTORY_SUFFIX:
      g_clear_pointer (&self->subdirectory_suffix, g_free);
      self->subdirectory_suffix = g_value_dup_string (value);
      break;

    case PROP_VERSION:
      g_clear_pointer (&self->version, g_free);
      self->version = g_value_dup_string (value);
      break;

    case PROP_VERSIONS:
      g_clear_pointer (&self->versions, g_free);
      self->versions = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_extension_class_init (BuilderExtensionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = builder_extension_finalize;
  object_class->get_property = builder_extension_get_property;
  object_class->set_property = builder_extension_set_property;

  g_object_class_install_property (object_class,
                                   PROP_DIRECTORY,
                                   g_param_spec_string ("directory",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BUNDLE,
                                   g_param_spec_boolean ("bundle",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_AUTODELETE,
                                   g_param_spec_boolean ("autodelete",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_NO_AUTODOWNLOAD,
                                   g_param_spec_boolean ("no-autodownload",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SUBDIRECTORIES,
                                   g_param_spec_boolean ("subdirectories",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ADD_LD_PATH,
                                   g_param_spec_string ("add-ld-path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DOWNLOAD_IF,
                                   g_param_spec_string ("download-if",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ENABLE_IF,
                                   g_param_spec_string ("enable-if",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_MERGE_DIRS,
                                   g_param_spec_string ("merge-dirs",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SUBDIRECTORY_SUFFIX,
                                   g_param_spec_string ("subdirectory-suffix",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_VERSION,
                                   g_param_spec_string ("version",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_VERSIONS,
                                   g_param_spec_string ("versions",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_extension_init (BuilderExtension *self)
{
}

void
builder_extension_set_name (BuilderExtension *self,
                            const char *name)
{
  g_free (self->name);
  self->name = g_strdup (name);
}

const char *
builder_extension_get_name (BuilderExtension *self)
{
  return self->name;
}


gboolean
builder_extension_is_bundled (BuilderExtension *self)
{
  return self->bundle;
}

const char *
builder_extension_get_directory (BuilderExtension *self)
{
  return self->directory;
}

static void
add_arg (BuilderExtension  *self,
         GPtrArray *args,
         const char *key,
         const char *value)
{
  if (value == NULL)
    return;
  g_ptr_array_add (args,
                   g_strdup_printf ("--extension=%s=%s=%s", self->name, key, value));
}

static void
add_argb (BuilderExtension  *self,
          GPtrArray *args,
          const char *key,
          gboolean val)
{
  if (val)
    add_arg (self, args, key, "true");
}

void
builder_extension_add_finish_args (BuilderExtension  *self,
                                   GPtrArray *args)
{
  if (self->directory == NULL)
    {
      g_warning ("No directory specified for extension '%s'", self->name);
      return;
    }

  add_arg (self, args, FLATPAK_METADATA_KEY_DIRECTORY, self->directory);
  add_argb (self, args, FLATPAK_METADATA_KEY_AUTODELETE, self->autodelete);
  add_argb (self, args, FLATPAK_METADATA_KEY_NO_AUTODOWNLOAD, self->no_autodownload);
  add_argb (self, args, FLATPAK_METADATA_KEY_SUBDIRECTORIES, self->subdirectories);
  add_arg (self, args, FLATPAK_METADATA_KEY_ADD_LD_PATH, self->add_ld_path);
  add_arg (self, args, FLATPAK_METADATA_KEY_DOWNLOAD_IF, self->download_if);
  add_arg (self, args, FLATPAK_METADATA_KEY_ENABLE_IF, self->enable_if);
  add_arg (self, args, FLATPAK_METADATA_KEY_MERGE_DIRS, self->merge_dirs);
  add_arg (self, args, FLATPAK_METADATA_KEY_SUBDIRECTORY_SUFFIX, self->subdirectory_suffix);
  add_arg (self, args, FLATPAK_METADATA_KEY_VERSION, self->version);
  add_arg (self, args, FLATPAK_METADATA_KEY_VERSIONS, self->versions);
}

void
builder_extension_checksum (BuilderExtension  *self,
                            BuilderCache   *cache,
                            BuilderContext *context)
{
  builder_cache_checksum_str (cache, BUILDER_EXTENSION_CHECKSUM_VERSION);
  builder_cache_checksum_str (cache, self->name);
  builder_cache_checksum_str (cache, self->directory);
  builder_cache_checksum_boolean (cache, self->bundle);
  builder_cache_checksum_boolean (cache, self->autodelete);
  builder_cache_checksum_boolean (cache, self->no_autodownload);
  builder_cache_checksum_boolean (cache, self->subdirectories);
  builder_cache_checksum_str (cache, self->add_ld_path);
  builder_cache_checksum_str (cache, self->download_if);
  builder_cache_checksum_str (cache, self->enable_if);
  builder_cache_checksum_str (cache, self->merge_dirs);
  builder_cache_checksum_str (cache, self->subdirectory_suffix);
  builder_cache_checksum_str (cache, self->version);
  builder_cache_checksum_str (cache, self->versions);
}
