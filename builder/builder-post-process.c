/* builder-post-process.c
 *
 * Copyright (C) 2017 Red Hat, Inc
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
#include "builder-post-process.h"

static gboolean
builder_post_process_strip (GFile *app_dir,
                            GPtrArray *changed,
                            GError        **error)
{
  int i;

  for (i = 0; i < changed->len; i++)
    {
      const char *rel_path = (char *) g_ptr_array_index (changed, i);
      g_autoptr(GFile) file = g_file_resolve_relative_path (app_dir, rel_path);
      g_autofree char *path = g_file_get_path (file);
      gboolean is_shared, is_stripped;

      if (!is_elf_file (path, &is_shared, &is_stripped))
        continue;

      g_print ("stripping: %s\n", rel_path);
      if (is_shared)
        {
          if (!strip (error, "--remove-section=.comment", "--remove-section=.note", "--strip-unneeded", path, NULL))
            return FALSE;
        }
      else
        {
          if (!strip (error, "--remove-section=.comment", "--remove-section=.note", path, NULL))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
builder_post_process_debuginfo (GFile          *app_dir,
                                GPtrArray      *changed,
                                BuilderContext *context,
                                GError        **error)
{
  g_autofree char *app_dir_path = g_file_get_path (app_dir);
  int j;

  for (j = 0; j < changed->len; j++)
    {
      const char *rel_path = (char *) g_ptr_array_index (changed, j);
      g_autoptr(GFile) file = g_file_resolve_relative_path (app_dir, rel_path);
      g_autofree char *path = g_file_get_path (file);
      g_autofree char *debug_path = NULL;
      g_autofree char *real_debug_path = NULL;
      g_autofree char *rel_path_dir = g_path_get_dirname (rel_path);
      g_autofree char *filename = g_path_get_basename (rel_path);
      g_autofree char *filename_debug = g_strconcat (filename, ".debug", NULL);
      g_autofree char *debug_dir = NULL;
      g_autofree char *source_dir_path = NULL;
      g_autoptr(GFile) source_dir = NULL;
      g_autofree char *real_debug_dir = NULL;
      gboolean is_shared, is_stripped;

      if (!is_elf_file (path, &is_shared, &is_stripped))
        continue;

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
              return FALSE;
            }

          source_dir = g_file_new_for_path (source_dir_path);
          if (g_mkdir_with_parents (source_dir_path, 0755) != 0)
            {
              glnx_set_error_from_errno (error);
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
                        return FALSE;

                      file_type = g_file_query_file_type (src, 0, NULL);
                      if (file_type == G_FILE_TYPE_DIRECTORY)
                        {
                          if (!flatpak_mkdir_p (dst, NULL, error))
                            return FALSE;
                        }
                      else if (file_type == G_FILE_TYPE_REGULAR)
                        {
                          /* Make sure the target is gone, because g_file_copy does
                             truncation on hardlinked destinations */
                          g_file_delete (dst, NULL, NULL);

                          if (!g_file_copy (src, dst,
                                            G_FILE_COPY_OVERWRITE,
                                            NULL, NULL, NULL, error))
                            return FALSE;
                        }
                    }
                }
            }

          g_print ("stripping %s to %s\n", path, debug_path);

          /* Some files are hardlinked and eu-strip modifies in-place,
             which breaks rofiles-fuse. Unlink them */
          if (!flatpak_unbreak_hardlink (file, error))
            return FALSE;

          if (!eu_strip (error, "--remove-comment", "--reloc-debug-sections",
                         "-f", debug_path,
                         "-F", real_debug_path,
                         path, NULL))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
builder_post_process (BuilderPostProcessFlags flags,
                      GFile *app_dir,
                      BuilderCache   *cache,
                      BuilderContext *context,
                      GError        **error)
{
  g_autofree char *app_dir_path = g_file_get_path (app_dir);
  g_autoptr(GPtrArray) changed = NULL;

  if (!builder_cache_get_outstanding_changes (cache, &changed, error))
    return FALSE;

  if (flags & BUILDER_POST_PROCESS_FLAGS_STRIP)
    {
      if (!builder_post_process_strip (app_dir, changed, error))
        return FALSE;
    }
  else if (flags & BUILDER_POST_PROCESS_FLAGS_DEBUGINFO)
    {
      if (!builder_post_process_debuginfo (app_dir, changed, context, error))
        return FALSE;
    }

  return TRUE;
}
