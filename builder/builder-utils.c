/* builder-utils.c
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

#include <libelf.h>
#include <gelf.h>
#include <sys/mman.h>

#include <string.h>

#include "xdg-app-utils.h"
#include "builder-utils.h"

char *
builder_uri_to_filename (const char *uri)
{
  GString *s;
  const char *p;

  s = g_string_new ("");

  for (p = uri; *p != 0; p++)
    {
      if (*p == '/' || *p == ':')
        {
          while (p[1] == '/' || p[1] == ':')
            p++;
          g_string_append_c (s, '_');
        }
      else
        g_string_append_c (s, *p);
    }

  return g_string_free (s, FALSE);
}

const char *
inplace_basename (const char *path)
{
  const char *last_slash;

  last_slash = strrchr (path, '/');
  if (last_slash)
    path = last_slash + 1;

  return path;
}


/* Adds all matches of path to prefix. There can be multiple, because
 * e.g matching "a/b/c" against "/a" matches both "a/b" and "a/b/c"
 *
 * If pattern starts with a slash, then match on the entire
 * path, otherwise just the basename.
 */
void
xdg_app_collect_matches_for_path_pattern (const char *path,
                                          const char *pattern,
                                          const char *add_prefix,
                                          GHashTable *to_remove_ht)
{
  const char *rest;

  if (pattern[0] != '/')
    {
      rest = xdg_app_path_match_prefix (pattern, inplace_basename (path));
      if (rest != NULL)
        g_hash_table_insert (to_remove_ht, g_strconcat (add_prefix ? add_prefix : "", path, NULL), GINT_TO_POINTER (1));
    }
  else
    {
      /* Absolute pathname match. This can actually match multiple
       * files, as a prefix match should remove all files below that
       * (in this module) */

      rest = xdg_app_path_match_prefix (pattern, path);
      while (rest != NULL)
        {
          const char *slash;
          g_autofree char *prefix = g_strndup (path, rest-path);
          g_hash_table_insert (to_remove_ht, g_strconcat (add_prefix ? add_prefix : "", prefix, NULL), GINT_TO_POINTER (1));
          while (*rest == '/')
            rest++;
          if (*rest == 0)
            break;
          slash = strchr (rest, '/');
          rest = slash ? slash : rest + strlen (rest);
        }
    }


}

gboolean
xdg_app_matches_path_pattern (const char *path,
                              const char *pattern)
{
  if (pattern[0] != '/')
    path = inplace_basename (path);

  return xdg_app_path_match_prefix (pattern, path) != NULL;
}

gboolean
strip (GError **error,
       ...)
{
  gboolean res;
  va_list ap;

  va_start (ap, error);
  res = xdg_app_spawn (NULL, NULL, error, "strip", ap);
  va_end (ap);

  return res;
}

gboolean
eu_strip (GError **error,
       ...)
{
  gboolean res;
  va_list ap;

  va_start (ap, error);
  res = xdg_app_spawn (NULL, NULL, error, "eu-strip", ap);
  va_end (ap);

  return res;
}

static gboolean elf_has_symtab (Elf *elf)
{
  Elf_Scn *scn;
  GElf_Shdr shdr;

  scn = NULL;
  while ((scn = elf_nextscn(elf, scn)) != NULL)
    {
      if (gelf_getshdr (scn, &shdr) == NULL)
        continue;

      if (shdr.sh_type != SHT_SYMTAB)
        continue;

      return TRUE;
    }

  return FALSE;
}

gboolean is_elf_file (const char *path,
                      gboolean *is_shared,
                      gboolean *is_stripped)
{
  g_autofree char *filename = g_path_get_basename (path);
  struct stat stbuf;

  if (lstat (path, &stbuf) == -1)
    return FALSE;

  if (!S_ISREG (stbuf.st_mode))
    return FALSE;

  if ((strstr (filename, ".so.") != NULL ||
       g_str_has_suffix (filename, ".so")) ||
      (stbuf.st_mode & 0111) != 0)
    {
      glnx_fd_close int fd = -1;

      fd = open (path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
      if (fd >= 0)
        {
          Elf *elf;
          GElf_Ehdr ehdr;
          gboolean res = FALSE;

          if (elf_version (EV_CURRENT) == EV_NONE )
            return FALSE;

          elf = elf_begin (fd, ELF_C_READ, NULL);
          if (elf == NULL)
            return FALSE;

          if (elf_kind (elf) == ELF_K_ELF &&
              gelf_getehdr (elf, &ehdr))
            {
              if (is_shared)
                *is_shared = ehdr.e_type == ET_DYN;
              if (is_stripped)
                *is_stripped = !elf_has_symtab (elf);

              res = TRUE;
            }

          elf_end (elf);
          return res;
        }
    }

  return FALSE;
}

gboolean directory_is_empty (const char *path)
{
  GDir *dir;
  gboolean empty;

  dir = g_dir_open (path, 0, NULL);
  if (g_dir_read_name (dir) == NULL)
    empty = TRUE;
  else
    empty = FALSE;

  g_dir_close (dir);

  return empty;
}

static gboolean
migrate_locale_dir (GFile *source_dir,
                    GFile *separate_dir,
                    const char *subdir,
                    GError **error)
{
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  GFileInfo *next;
  GError *temp_error = NULL;

  dir_enum = g_file_enumerate_children (source_dir, "standard::name,standard::type",
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, NULL);
  if (!dir_enum)
    return TRUE;

  while ((next = g_file_enumerator_next_file (dir_enum, NULL, &temp_error)))
    {
      g_autoptr(GFileInfo) child_info = next;
      g_autoptr(GFile) child = NULL;
      g_autoptr(GFile) locale_subdir = NULL;

      child = g_file_get_child (source_dir, g_file_info_get_name (child_info));

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          g_autoptr(GFile) child = NULL;
          const char *name = g_file_info_get_name (child_info);
          g_autofree char *language = g_strdup (name);
          g_autofree char *relative = NULL;
          g_autofree char *target = NULL;
          char *c;

          c = strchr (language, '@');
          if (c != NULL)
            *c = 0;
          c = strchr (language, '_');
          if (c != NULL)
            *c = 0;

          /* We ship english and C locales always */
          if (strcmp (language, "C") == 0 ||
              strcmp (language, "en") == 0)
            continue;

          child = g_file_get_child (source_dir, g_file_info_get_name (child_info));

          relative = g_build_filename (language, subdir, name, NULL);
          locale_subdir = g_file_resolve_relative_path (separate_dir, relative);
          if (!gs_file_ensure_directory (locale_subdir, TRUE,
                                         NULL, error))
            return FALSE;

          if (!xdg_app_cp_a (child, locale_subdir,
                             XDG_APP_CP_FLAGS_MERGE | XDG_APP_CP_FLAGS_MOVE,
                             NULL, error))
            return FALSE;

          target = g_build_filename ("../../share/runtime/locale", relative, NULL);

          if (!g_file_make_symbolic_link (child, target,
                                          NULL, error))
            return FALSE;

        }
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  return TRUE;
}

gboolean
builder_migrate_locale_dirs (GFile *root_dir,
                             GError **error)
{
  g_autoptr(GFile) separate_dir = NULL;
  g_autoptr(GFile) lib_locale_dir = NULL;
  g_autoptr(GFile) share_locale_dir = NULL;

  lib_locale_dir = g_file_resolve_relative_path (root_dir, "lib/locale");
  share_locale_dir = g_file_resolve_relative_path (root_dir, "share/locale");
  separate_dir = g_file_resolve_relative_path (root_dir, "share/runtime/locale");

  if (!migrate_locale_dir (lib_locale_dir, separate_dir, "lib", error))
    return FALSE;

  if (!migrate_locale_dir (share_locale_dir, separate_dir, "share", error))
    return FALSE;

  return TRUE;
}
