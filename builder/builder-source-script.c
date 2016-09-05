/* builder-source-script.c
 *
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This script is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This script is distributed in the hope that it will be useful, but
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
#include "builder-source-script.h"

struct BuilderSourceScript
{
  BuilderSource parent;

  char        **commands;
  char         *dest_filename;
};

typedef struct
{
  BuilderSourceClass parent_class;
} BuilderSourceScriptClass;

G_DEFINE_TYPE (BuilderSourceScript, builder_source_script, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_COMMANDS,
  PROP_DEST_FILENAME,
  LAST_PROP
};

static void
builder_source_script_finalize (GObject *object)
{
  BuilderSourceScript *self = (BuilderSourceScript *) object;

  g_strfreev (self->commands);
  g_free (self->dest_filename);

  G_OBJECT_CLASS (builder_source_script_parent_class)->finalize (object);
}

static void
builder_source_script_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  BuilderSourceScript *self = BUILDER_SOURCE_SCRIPT (object);

  switch (prop_id)
    {
    case PROP_COMMANDS:
      g_value_set_boxed (value, self->commands);
      break;

    case PROP_DEST_FILENAME:
      g_value_set_string (value, self->dest_filename);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_script_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  BuilderSourceScript *self = BUILDER_SOURCE_SCRIPT (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_COMMANDS:
      tmp = self->commands;
      self->commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    case PROP_DEST_FILENAME:
      g_free (self->dest_filename);
      self->dest_filename = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static gboolean
builder_source_script_download (BuilderSource  *source,
                                gboolean        update_vcs,
                                BuilderContext *context,
                                GError        **error)
{
  return TRUE;
}

static gboolean
builder_source_script_extract (BuilderSource  *source,
                               GFile          *dest,
                               BuilderOptions *build_options,
                               BuilderContext *context,
                               GError        **error)
{
  BuilderSourceScript *self = BUILDER_SOURCE_SCRIPT (source);

  g_autoptr(GFile) dest_script = NULL;
  const char *dest_filename;
  g_autoptr(GString) script = NULL;
  int i;
  guint32 perms;

  script = g_string_new ("#!/bin/sh\n");

  if (self->commands)
    {
      for (i = 0; self->commands[i] != NULL; i++)
        {
          g_string_append (script, self->commands[i]);
          g_string_append_c (script, '\n');
        }
    }

  if (self->dest_filename)
    dest_filename = self->dest_filename;
  else
    dest_filename = "autogen.sh";

  dest_script = g_file_get_child (dest, dest_filename);

  if (!g_file_replace_contents (dest_script,
                                script->str,
                                script->len,
                                NULL,
                                FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL, NULL, error))
    return FALSE;

  perms = 0755;
  if (!g_file_set_attribute (dest_script,
                             G_FILE_ATTRIBUTE_UNIX_MODE,
                             G_FILE_ATTRIBUTE_TYPE_UINT32,
                             &perms,
                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                             NULL, error))
    return FALSE;

  return TRUE;
}

static void
builder_source_script_checksum (BuilderSource  *source,
                                BuilderCache   *cache,
                                BuilderContext *context)
{
  BuilderSourceScript *self = BUILDER_SOURCE_SCRIPT (source);

  builder_cache_checksum_strv (cache, self->commands);
  builder_cache_checksum_str (cache, self->dest_filename);
}

static void
builder_source_script_class_init (BuilderSourceScriptClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_script_finalize;
  object_class->get_property = builder_source_script_get_property;
  object_class->set_property = builder_source_script_set_property;

  source_class->download = builder_source_script_download;
  source_class->extract = builder_source_script_extract;
  source_class->checksum = builder_source_script_checksum;

  g_object_class_install_property (object_class,
                                   PROP_COMMANDS,
                                   g_param_spec_boxed ("commands",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
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
builder_source_script_init (BuilderSourceScript *self)
{
}
