/* builder-source-shell.c
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
#include "builder-source-shell.h"

struct BuilderSourceShell {
  BuilderSource parent;

  char **commands;
};

typedef struct {
  BuilderSourceClass parent_class;
} BuilderSourceShellClass;

G_DEFINE_TYPE (BuilderSourceShell, builder_source_shell, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_COMMANDS,
  LAST_PROP
};

static void
builder_source_shell_finalize (GObject *object)
{
  BuilderSourceShell *self = (BuilderSourceShell *)object;

  g_strfreev (self->commands);

  G_OBJECT_CLASS (builder_source_shell_parent_class)->finalize (object);
}

static void
builder_source_shell_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (object);

  switch (prop_id)
    {
    case PROP_COMMANDS:
      g_value_set_boxed (value, self->commands);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_shell_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (object);
  gchar **tmp;

  switch (prop_id)
    {
    case PROP_COMMANDS:
      tmp = self->commands;
      self->commands = g_strdupv (g_value_get_boxed (value));
      g_strfreev (tmp);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static gboolean
builder_source_shell_download (BuilderSource *source,
                                gboolean update_vcs,
                                BuilderContext *context,
                                GError **error)
{
  return TRUE;
}

static gboolean
run_script (BuilderContext *context,
            GFile *source_dir,
            const gchar *script,
            GError **error)
{
  GFile *app_dir = builder_context_get_app_dir (context);
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  g_autoptr(GPtrArray) args = NULL;
  g_autofree char *commandline = NULL;
  g_autofree char *source_dir_path = g_file_get_path (source_dir);
  g_autofree char *source_dir_path_canonical = NULL;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("xdg-app"));
  g_ptr_array_add (args, g_strdup ("build"));

  source_dir_path_canonical = canonicalize_file_name (source_dir_path);

  g_ptr_array_add (args, g_strdup ("--nofilesystem=host"));
  g_ptr_array_add (args, g_strdup_printf ("--filesystem=%s", source_dir_path_canonical));

  g_ptr_array_add (args, g_file_get_path (app_dir));
  g_ptr_array_add (args, g_strdup ("/bin/sh"));
  g_ptr_array_add (args, g_strdup ("-c"));
  g_ptr_array_add (args, g_strdup (script));
  g_ptr_array_add (args, NULL);

  commandline = g_strjoinv (" ", (char **) args->pdata);
  g_print ("Running: %s\n", commandline);

  launcher = g_subprocess_launcher_new (0);

  g_subprocess_launcher_set_cwd (launcher, source_dir_path_canonical);

  subp = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) args->pdata, error);
  g_ptr_array_free (args, TRUE);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  return TRUE;
}


static gboolean
builder_source_shell_extract (BuilderSource *source,
                               GFile *dest,
                               BuilderContext *context,
                               GError **error)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (source);
  int i;

  if (self->commands)
    {
      for (i = 0; self->commands[i] != NULL; i++)
        {
          if (!run_script (context,
                           dest, self->commands[i], error))
            return FALSE;
        }
    }


  return TRUE;
}

static void
builder_source_shell_checksum (BuilderSource  *source,
                                BuilderCache   *cache,
                                BuilderContext *context)
{
  BuilderSourceShell *self = BUILDER_SOURCE_SHELL (source);

  builder_cache_checksum_strv (cache, self->commands);
}

static void
builder_source_shell_class_init (BuilderSourceShellClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_shell_finalize;
  object_class->get_property = builder_source_shell_get_property;
  object_class->set_property = builder_source_shell_set_property;

  source_class->download = builder_source_shell_download;
  source_class->extract = builder_source_shell_extract;
  source_class->checksum = builder_source_shell_checksum;

  g_object_class_install_property (object_class,
                                   PROP_COMMANDS,
                                   g_param_spec_boxed ("commands",
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
}

static void
builder_source_shell_init (BuilderSourceShell *self)
{
}
