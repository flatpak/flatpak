/* builder-source-bzr.c
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

#include "builder-source-bzr.h"
#include "builder-utils.h"

struct BuilderSourceBzr {
  BuilderSource parent;

  char *url;
};

typedef struct {
  BuilderSourceClass parent_class;
} BuilderSourceBzrClass;

G_DEFINE_TYPE (BuilderSourceBzr, builder_source_bzr, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_URL,
  LAST_PROP
};

static void
builder_source_bzr_finalize (GObject *object)
{
  BuilderSourceBzr *self = (BuilderSourceBzr *)object;

  g_free (self->url);

  G_OBJECT_CLASS (builder_source_bzr_parent_class)->finalize (object);
}

static void
builder_source_bzr_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  BuilderSourceBzr *self = BUILDER_SOURCE_BZR (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_value_set_string (value, self->url);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_bzr_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  BuilderSourceBzr *self = BUILDER_SOURCE_BZR (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_free (self->url);
      self->url = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

typedef struct
{
  GError *error;
  GError *splice_error;
  GMainLoop *loop;
  int refs;
} BzrData;

static gboolean bzr (GFile *dir,
                     char **output,
                     GError **error,
                     const gchar            *argv1,
                     ...) G_GNUC_NULL_TERMINATED;


static void
bzr_data_exit (BzrData *data)
{
  data->refs--;
  if (data->refs == 0)
    g_main_loop_quit (data->loop);
}

static void
bzr_output_spliced_cb (GObject    *obj,
                       GAsyncResult  *result,
                       gpointer       user_data)
{
  BzrData *data = user_data;

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &data->splice_error);
  bzr_data_exit (data);
}

static void
bzr_exit_cb (GObject    *obj,
             GAsyncResult  *result,
             gpointer       user_data)
{
  BzrData *data = user_data;

  g_subprocess_wait_check_finish (G_SUBPROCESS (obj), result, &data->error);
  bzr_data_exit (data);
}

static gboolean
bzr (GFile        *dir,
     char        **output,
     GError      **error,
     const gchar  *argv1,
     ...)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GPtrArray *args;
  const gchar *arg;
  GInputStream *in;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  va_list ap;
  BzrData data = {0};

  args = g_ptr_array_new ();
  g_ptr_array_add (args, "bzr");
  va_start (ap, argv1);
  g_ptr_array_add (args, (gchar *) argv1);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, (gchar *) arg);
  g_ptr_array_add (args, NULL);
  va_end (ap);

  launcher = g_subprocess_launcher_new (0);

  if (output)
    g_subprocess_launcher_set_flags (launcher, G_SUBPROCESS_FLAGS_STDOUT_PIPE);

  if (dir)
    {
      g_autofree char *path = g_file_get_path (dir);
      g_subprocess_launcher_set_cwd (launcher, path);
    }

  subp = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) args->pdata, error);
  g_ptr_array_free (args, TRUE);

  if (subp == NULL)
    return FALSE;

  loop = g_main_loop_new (NULL, FALSE);

  data.loop = loop;
  data.refs = 1;

  if (output)
    {
      data.refs++;
      in = g_subprocess_get_stdout_pipe (subp);
      out = g_memory_output_stream_new_resizable ();
      g_output_stream_splice_async  (out,
                                     in,
                                     G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                     0,
                                     NULL,
                                     bzr_output_spliced_cb,
                                     &data);
    }

  g_subprocess_wait_async (subp, NULL, bzr_exit_cb, &data);

  g_main_loop_run (loop);

  if (data.error)
    {
      g_propagate_error (error, data.error);
      g_clear_error (&data.splice_error);
      return FALSE;
    }

  if (out)
    {
      if (data.splice_error)
        {
          g_propagate_error (error, data.splice_error);
          return FALSE;
        }

      /* Null terminate */
      g_output_stream_write (out, "\0", 1, NULL, NULL);
      *output = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
    }

  return TRUE;
}

static GFile *
get_mirror_dir (BuilderSourceBzr *self, BuilderContext *context)
{
  g_autoptr(GFile) bzr_dir = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *bzr_dir_path = NULL;

  bzr_dir = g_file_get_child (builder_context_get_state_dir (context),
                              "bzr");

  bzr_dir_path = g_file_get_path (bzr_dir);
  g_mkdir_with_parents (bzr_dir_path, 0755);

  filename = builder_uri_to_filename (self->url);
  return g_file_get_child (bzr_dir, filename);
}

static char *
get_current_commit (BuilderSourceBzr *self, BuilderContext *context, GError **error)
{
  g_autoptr(GFile) mirror_dir = NULL;
  char *output = NULL;

  mirror_dir = get_mirror_dir (self, context);

  if (!bzr (mirror_dir, &output, error,
           "revno", NULL))
    return NULL;

  return output;
}

static gboolean
builder_source_bzr_download (BuilderSource *source,
                             BuilderContext *context,
                             GError **error)
{
  BuilderSourceBzr *self = BUILDER_SOURCE_BZR (source);
  g_autoptr(GFile) mirror_dir = NULL;

  if (self->url == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "URL not specified");
      return FALSE;
    }

  mirror_dir = get_mirror_dir (self, context);

  if (!g_file_query_exists (mirror_dir, NULL))
    {
      g_autofree char *filename = g_file_get_basename (mirror_dir);
      g_autoptr(GFile) parent = g_file_get_parent (mirror_dir);
      g_autofree char *filename_tmp = g_strconcat ("./", filename, ".clone_tmp", NULL);
      g_autoptr(GFile) mirror_dir_tmp = g_file_get_child (parent, filename_tmp);

      if (!bzr (parent, NULL, error,
                "branch", self->url,  filename_tmp, NULL) ||
          !g_file_move (mirror_dir_tmp, mirror_dir, 0, NULL, NULL, NULL, error))
        return FALSE;
    }
  else
    {
      if (!bzr (mirror_dir, NULL, error,
                "pull", NULL))
        return FALSE;
    }

  return TRUE;
}

static gboolean
builder_source_bzr_extract (BuilderSource *source,
                            GFile *dest,
                            BuilderContext *context,
                            GError **error)
{
  BuilderSourceBzr *self = BUILDER_SOURCE_BZR (source);
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *mirror_dir_path = NULL;
  g_autofree char *dest_path = NULL;

  mirror_dir = get_mirror_dir (self, context);

  mirror_dir_path = g_file_get_path (mirror_dir);
  dest_path = g_file_get_path (dest);

  if (!bzr (NULL, NULL, error,
            "branch", "--stacked", mirror_dir_path, dest_path, "--use-existing-dir", NULL))
    return FALSE;

  return TRUE;
}

static void
builder_source_bzr_checksum (BuilderSource  *source,
                             BuilderCache   *cache,
                             BuilderContext *context)
{
  BuilderSourceBzr *self = BUILDER_SOURCE_BZR (source);
  g_autofree char *current_commit;
  g_autoptr(GError) error = NULL;

  builder_cache_checksum_str (cache, self->url);

  current_commit = get_current_commit (self, context, &error);
  if (current_commit)
    builder_cache_checksum_str (cache, current_commit);
  else if (error)
    g_warning ("Failed to get current bzr checksum: %s", error->message);
}

static void
builder_source_bzr_class_init (BuilderSourceBzrClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_bzr_finalize;
  object_class->get_property = builder_source_bzr_get_property;
  object_class->set_property = builder_source_bzr_set_property;

  source_class->download = builder_source_bzr_download;
  source_class->extract = builder_source_bzr_extract;
  source_class->checksum = builder_source_bzr_checksum;

  g_object_class_install_property (object_class,
                                   PROP_URL,
                                   g_param_spec_string ("url",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_source_bzr_init (BuilderSourceBzr *self)
{
}
