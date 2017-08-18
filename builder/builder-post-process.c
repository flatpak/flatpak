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
invalidate_old_python_compiled (const char *path,
                                const char *rel_path,
                                GError **error)
{
  struct stat stbuf;
  g_autofree char *pyc = NULL;
  g_autofree char *pyo = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *py3dir = NULL;
  g_autofree char *pyfilename = NULL;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };


  /* This is a python file, not a .py[oc]. If it changed (mtime != 0) then
   * this needs to invalidate any old (mtime == 0) .py[oc] files that could refer to it.
   */

  if (lstat (path, &stbuf) != 0)
    {
      g_warning ("Can't stat %s", rel_path);
      return TRUE;
    }

  if (stbuf.st_mtime == OSTREE_TIMESTAMP)
    return TRUE; /* Previously handled .py */

  pyc = g_strconcat (path, "c", NULL);
  if (lstat (pyc, &stbuf) == 0 &&
      stbuf.st_mtime == OSTREE_TIMESTAMP)
    {
      g_print ("Removing stale file %sc", rel_path);
      if (unlink (pyc) != 0)
        g_warning ("Unable to delete %s", pyc);
    }

  pyo = g_strconcat (path, "o", NULL);
  if (lstat (pyo, &stbuf) == 0 &&
      stbuf.st_mtime == OSTREE_TIMESTAMP)
    {
      g_print ("Removing stale file %so", rel_path);
      if (unlink (pyo) != 0)
        g_warning ("Unable to delete %s", pyo);
    }

  /* Handle python3 which is in a __pycache__ subdir */

  pyfilename = g_path_get_basename (path);
  pyfilename[strlen (pyfilename) - 2] = 0; /* skip "py" */
  dir = g_path_get_dirname (path);
  py3dir = g_build_filename (dir, "__pycache__", NULL);

  if (glnx_dirfd_iterator_init_at (AT_FDCWD, py3dir, FALSE, &dfd_iter, NULL))
    {
      struct dirent *dent;
      while (glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, NULL, NULL) &&
             dent != NULL)
        {
          if (!(g_str_has_suffix (dent->d_name, ".pyc") ||
                g_str_has_suffix (dent->d_name, ".pyo")))
            continue;

          if (!g_str_has_prefix (dent->d_name, pyfilename))
            continue;

          if (fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == 0 &&
              stbuf.st_mtime == OSTREE_TIMESTAMP)
            {
              g_print ("Removing stale file %s/__pycache__/%s", rel_path, dent->d_name);
              if (unlinkat (dfd_iter.fd, dent->d_name, 0))
                g_warning ("Unable to delete %s", dent->d_name);
            }
        }
    }

  return TRUE;
}

static gboolean
fixup_python_time_stamp (const char *path,
                         const char *rel_path,
                         GError **error)
{
  glnx_fd_close int fd = -1;
  g_auto(GLnxTmpfile) tmpf = { 0 };
  guint8 buffer[8];
  ssize_t res;
  guint32 pyc_mtime;
  g_autofree char *py_path = NULL;
  struct stat stbuf;
  gboolean remove_pyc = FALSE;
  g_autofree char *path_basename = g_path_get_basename (path);
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *dir_basename = g_path_get_basename (dir);

  fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd == -1)
    {
      g_warning ("Can't open %s", rel_path);
      return TRUE;
    }

  res = pread (fd, buffer, 8, 0);
  if (res != 8)
    {
      g_warning ("Short read for %s", rel_path);
      return TRUE;
    }

  if (buffer[2] != 0x0d || buffer[3] != 0x0a)
    {
      g_debug ("Not matching python magic: %s", rel_path);
      return TRUE;
    }

  pyc_mtime =
    (buffer[4] << 8*0) |
    (buffer[5] << 8*1) |
    (buffer[6] << 8*2) |
    (buffer[7] << 8*3);

  if (strcmp (dir_basename, "__pycache__") == 0)
    {
      /* Python3 */
      g_autofree char *base = g_strdup (path_basename);
      g_autofree char *real_dir = g_path_get_dirname (dir);
      g_autofree char *py_basename = NULL;
      char *dot;

      dot = strrchr (base, '.');
      if (dot == NULL)
        return TRUE;
      *dot = 0;

      dot = strrchr (base, '.');
      if (dot == NULL)
        return TRUE;
      *dot = 0;

      py_basename = g_strconcat (base, ".py", NULL);
      py_path = g_build_filename (real_dir, py_basename, NULL);
    }
  else
    {
      /* Python2 */
      py_path = g_strndup (path, strlen (path) - 1);
    }

  /* Here we found a .pyc (or .pyo) file and a possible .py file that apply for it.
   * There are several possible cases wrt their mtimes:
   *
   * py not existing: pyc is stale, remove it
   * pyc mtime == 0: (.pyc is from an old commited module)
   *     py mtime == 0: Do nothing, already correct
   *     py mtime != 0: The py changed in this module, remove pyc
   * pyc mtime != 0: (.pyc changed this module, or was never rewritten in base layer)
   *     py mtime == 0: Shouldn't happen in flatpak-builder, but could be an un-rewritten ctime lower layer, assume it matches and update timestamp
   *     py mtime != pyc mtime: new pyc doesn't match last py written in this module, remove it
   *     py mtime == pyc mtime: These match, but the py will be set to mtime 0 by ostree, so update timestamp in pyc.
   */

  if (lstat (py_path, &stbuf) != 0)
    {
      /* pyc file without .py file, this happens for binary-only deployments.
       *  Accept it as-is. */
      return TRUE;
    }
  else if (pyc_mtime == OSTREE_TIMESTAMP)
    {
      if (stbuf.st_mtime == OSTREE_TIMESTAMP)
        return TRUE; /* Previously handled pyc */

      remove_pyc = TRUE;
    }
  else /* pyc_mtime != 0 */
    {
      if (pyc_mtime != stbuf.st_mtime && stbuf.st_mtime != OSTREE_TIMESTAMP)
        remove_pyc = TRUE;
      /* else change mtime */
    }

  if (remove_pyc)
    {
      g_print ("Removing stale python bytecode file %s\n", rel_path);
      if (unlink (path) != 0)
        g_warning ("Unable to delete %s", rel_path);
      return TRUE;
    }

  if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, dir,
                                      O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                                      &tmpf,
                                      error))
    return FALSE;

  if (glnx_regfile_copy_bytes (fd, tmpf.fd, (off_t)-1) < 0)
    return glnx_throw_errno_prefix (error, "copyfile");

  /* Change to mtime 0 which is what ostree uses for checkouts */
  buffer[4] = OSTREE_TIMESTAMP;
  buffer[5] = buffer[6] = buffer[7] = 0;

  res = pwrite (tmpf.fd, buffer, 8, 0);
  if (res != 8)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!glnx_link_tmpfile_at (&tmpf,
                             GLNX_LINK_TMPFILE_REPLACE,
                             AT_FDCWD,
                             path,
                             error))
    return FALSE;

  g_print ("Fixed up header mtime for %s\n", rel_path);

  /* The mtime will be zeroed on cache commit. We don't want to do that now, because multiple
     files could reference one .py file and we need the mtimes to match for them all */

  return TRUE;
}

static gboolean
builder_post_process_python_time_stamp (GFile *app_dir,
                                        GPtrArray *changed,
                                        GError **error)
{
  int i;

  for (i = 0; i < changed->len; i++)
    {
      const char *rel_path = (char *) g_ptr_array_index (changed, i);
      g_autoptr(GFile) file = NULL;
      g_autofree char *path = NULL;
      struct stat stbuf;

      if (!(g_str_has_suffix (rel_path, ".py") ||
            g_str_has_suffix (rel_path, ".pyc") ||
            g_str_has_suffix (rel_path, ".pyo")))
        continue;

      file = g_file_resolve_relative_path (app_dir, rel_path);
      path = g_file_get_path (file);

      if (lstat (path, &stbuf) == -1)
        continue;

      if (!S_ISREG (stbuf.st_mode))
        continue;

      if (g_str_has_suffix (rel_path, ".py"))
        {
          if (!invalidate_old_python_compiled (path, rel_path, error))
            return FALSE;
        }
      else
        {
          if (!fixup_python_time_stamp (path, rel_path, error))
            return FALSE;
        }
    }

  return TRUE;
}


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

      if (is_stripped)
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

      if (is_stripped)
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
                          (void)g_file_delete (dst, NULL, NULL);

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
          if (!flatpak_break_hardlink (file, error))
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
  g_autoptr(GPtrArray) changed = NULL;

  if (!builder_cache_get_outstanding_changes (cache, &changed, error))
    return FALSE;

  if (flags & BUILDER_POST_PROCESS_FLAGS_PYTHON_TIMESTAMPS)
    {
      if (!builder_post_process_python_time_stamp (app_dir, changed,error))
        return FALSE;
    }

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
