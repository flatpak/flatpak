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

/* Returns end of matching path prefix, or NULL if no match */
static const char *
path_prefix_match (const char *pattern,
                   const char *string)
{
  char c, test;
  const char *tmp;

  while (TRUE)
    {
      switch (c = *pattern++)
        {
        case 0:
          if (*string == '/' || *string == 0)
            return string;
          return NULL;

        case '?':
          if (*string == '/' || *string == 0)
            return NULL;
          string++;
          break;

        case '*':
          c = *pattern;

          while (c == '*')
            c = *++pattern;

          /* special case * at end */
          if (c == 0)
            {
              char *tmp = strchr (string, '/');
              if (tmp != NULL)
                return tmp;
              return string + strlen (string);
            }
          else if (c == '/')
            {
              string = strchr (string, '/');
              if (string == NULL)
                return NULL;
              break;
            }

          while ((test = *string) != 0)
            {
              tmp = path_prefix_match (pattern, string);
              if (tmp != NULL)
                return tmp;
              if (test == '/')
                break;
              string++;
            }
          return NULL;

        default:
          if (c != *string)
            return NULL;
          string++;
          break;
        }
    }
  return NULL; /* Should not be reached */
}

/* If pattern starts with a slash, then match on the entire
 * path, otherwise just the basename.
 * Note: Return value points into path.
 */
static const char *
path_prefix_match_full (const char *pattern,
                        const char *path,
                        char **prefix_out)
{
  const char *rest;
  const char *last_slash;

  if (pattern[0] == '/')
    {
      /* Absolute path match */
      rest = path_prefix_match (pattern+1, path);
    }
  else
    {
      /* Basename match */
      last_slash = strrchr (path, '/');
      if (last_slash && prefix_out)
        {
          *prefix_out = g_strndup (path, last_slash - path);
          path = last_slash + 1;
        }
      rest = path_prefix_match (pattern, path);
    }

  return rest;
}

/* Adds all matches of path to prefix. There can be multiple, because
   e.g matching "a/b/c" against "/a" matches both "a/b" and "a/b/c" */
void
xdg_app_collect_matches_for_path_pattern (const char *path,
                                          const char *pattern,
                                          GHashTable *to_remove_ht)
{
  const char *rest;
  g_autofree char *dir = NULL;

  rest = path_prefix_match_full (pattern, path, &dir);

  while (rest != NULL)
    {
      const char *slash;
      g_autofree char *prefix = g_strndup (path, rest-path);
      g_autofree char *to_remove = NULL;
      if (dir)
        to_remove = g_strconcat (dir, "/", prefix, NULL);
      else
        to_remove = g_strdup (prefix);
      g_hash_table_insert (to_remove_ht, g_steal_pointer (&to_remove), GINT_TO_POINTER (1));
      while (*rest == '/')
        rest++;
      if (*rest == 0)
        break;
      slash = strchr (rest, '/');
      rest = slash ? slash : rest + strlen (rest);
    }
}

gboolean
xdg_app_matches_path_pattern (const char *path,
                              const char *pattern)
{
  return path_prefix_match_full (pattern, path, NULL) != NULL;
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
