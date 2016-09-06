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

#include <stdlib.h>
#include <libelf.h>
#include <gelf.h>
#include <dwarf.h>
#include <sys/mman.h>

#include <string.h>

#include <glib-unix.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#include "flatpak-utils.h"
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
        {
          g_string_append_c (s, *p);
        }
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
flatpak_collect_matches_for_path_pattern (const char *path,
                                          const char *pattern,
                                          const char *add_prefix,
                                          GHashTable *to_remove_ht)
{
  const char *rest;

  if (pattern[0] != '/')
    {
      rest = flatpak_path_match_prefix (pattern, inplace_basename (path));
      if (rest != NULL)
        g_hash_table_insert (to_remove_ht, g_strconcat (add_prefix ? add_prefix : "", path, NULL), GINT_TO_POINTER (1));
    }
  else
    {
      /* Absolute pathname match. This can actually match multiple
       * files, as a prefix match should remove all files below that
       * (in this module) */

      rest = flatpak_path_match_prefix (pattern, path);
      while (rest != NULL)
        {
          const char *slash;
          g_autofree char *prefix = g_strndup (path, rest - path);
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
flatpak_matches_path_pattern (const char *path,
                              const char *pattern)
{
  if (pattern[0] != '/')
    path = inplace_basename (path);

  return flatpak_path_match_prefix (pattern, path) != NULL;
}

gboolean
strip (GError **error,
       ...)
{
  gboolean res;
  va_list ap;

  va_start (ap, error);
  res = flatpak_spawn (NULL, NULL, error, "strip", ap);
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
  res = flatpak_spawn (NULL, NULL, error, "eu-strip", ap);
  va_end (ap);

  return res;
}

static gboolean
elf_has_symtab (Elf *elf)
{
  Elf_Scn *scn;
  GElf_Shdr shdr;

  scn = NULL;
  while ((scn = elf_nextscn (elf, scn)) != NULL)
    {
      if (gelf_getshdr (scn, &shdr) == NULL)
        continue;

      if (shdr.sh_type != SHT_SYMTAB)
        continue;

      return TRUE;
    }

  return FALSE;
}

gboolean
is_elf_file (const char *path,
             gboolean   *is_shared,
             gboolean   *is_stripped)
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

      fd = open (path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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

gboolean
directory_is_empty (const char *path)
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
migrate_locale_dir (GFile      *source_dir,
                    GFile      *separate_dir,
                    const char *subdir,
                    GError    **error)
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
          c = strchr (language, '.');
          if (c != NULL)
            *c = 0;

          /* We ship english and C locales always */
          if (strcmp (language, "C") == 0 ||
              strcmp (language, "en") == 0)
            continue;

          child = g_file_get_child (source_dir, g_file_info_get_name (child_info));

          relative = g_build_filename (language, subdir, name, NULL);
          locale_subdir = g_file_resolve_relative_path (separate_dir, relative);
          if (!flatpak_mkdir_p (locale_subdir, NULL, error))
            return FALSE;

          if (!flatpak_cp_a (child, locale_subdir,
                             FLATPAK_CP_FLAGS_MERGE | FLATPAK_CP_FLAGS_MOVE,
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
builder_migrate_locale_dirs (GFile   *root_dir,
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


/*
 * This code is based on debugedit.c from rpm, which has this copyright:
 *
 *
 * Copyright (C) 2001, 2002, 2003, 2005, 2007, 2009, 2010, 2011 Red Hat, Inc.
 * Written by Alexander Larsson <alexl@redhat.com>, 2002
 * Based on code by Jakub Jelinek <jakub@redhat.com>, 2001.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */


#define DW_TAG_partial_unit 0x3c
#define DW_FORM_sec_offset 0x17
#define DW_FORM_exprloc 0x18
#define DW_FORM_flag_present 0x19
#define DW_FORM_ref_sig8 0x20

/* keep uptodate with changes to debug_sections */
#define DEBUG_INFO 0
#define DEBUG_ABBREV 1
#define DEBUG_LINE 2
#define DEBUG_ARANGES 3
#define DEBUG_PUBNAMES 4
#define DEBUG_PUBTYPES 5
#define DEBUG_MACINFO 6
#define DEBUG_LOC 7
#define DEBUG_STR 8
#define DEBUG_FRAME 9
#define DEBUG_RANGES 10
#define DEBUG_TYPES 11
#define DEBUG_MACRO 12
#define DEBUG_GDB_SCRIPT 13
#define NUM_DEBUG_SECTIONS 14

static const char * debug_section_names[] = {
  ".debug_info",
  ".debug_abbrev",
  ".debug_line",
  ".debug_aranges",
  ".debug_pubnames",
  ".debug_pubtypes",
  ".debug_macinfo",
  ".debug_loc",
  ".debug_str",
  ".debug_frame",
  ".debug_ranges",
  ".debug_types",
  ".debug_macro",
  ".debug_gdb_scripts",
};


typedef struct
{
  unsigned char *data;
  Elf_Data      *elf_data;
  size_t         size;
  int            sec, relsec;
} debug_section_t;

typedef struct
{
  Elf            *elf;
  GElf_Ehdr       ehdr;
  Elf_Scn       **scns;
  const char     *filename;
  int             lastscn;
  debug_section_t debug_sections[NUM_DEBUG_SECTIONS];
  GElf_Shdr      *shdr;
} DebuginfoData;

typedef struct
{
  unsigned char *ptr;
  uint32_t       addend;
} REL;

#define read_uleb128(ptr) ({            \
    unsigned int ret = 0;                 \
    unsigned int c;                       \
    int shift = 0;                        \
    do                                    \
    {                                   \
      c = *ptr++;                       \
      ret |= (c & 0x7f) << shift;       \
      shift += 7;                       \
    } while (c & 0x80);                 \
                                        \
    if (shift >= 35)                      \
      ret = UINT_MAX;                     \
    ret;                                  \
  })

static uint16_t (*do_read_16)(unsigned char *ptr);
static uint32_t (*do_read_32) (unsigned char *ptr);

static int ptr_size;
static int cu_version;

static inline uint16_t
buf_read_ule16 (unsigned char *data)
{
  return data[0] | (data[1] << 8);
}

static inline uint16_t
buf_read_ube16 (unsigned char *data)
{
  return data[1] | (data[0] << 8);
}

static inline uint32_t
buf_read_ule32 (unsigned char *data)
{
  return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

static inline uint32_t
buf_read_ube32 (unsigned char *data)
{
  return data[3] | (data[2] << 8) | (data[1] << 16) | (data[0] << 24);
}

#define read_1(ptr) *ptr++

#define read_16(ptr) ({                                 \
    uint16_t ret = do_read_16 (ptr);                      \
    ptr += 2;                                             \
    ret;                                                  \
  })

#define read_32(ptr) ({                                 \
    uint32_t ret = do_read_32 (ptr);                      \
    ptr += 4;                                             \
    ret;                                                  \
  })

REL *relptr, *relend;
int reltype;

#define do_read_32_relocated(ptr) ({                    \
    uint32_t dret = do_read_32 (ptr);                     \
    if (relptr)                                           \
    {                                                   \
      while (relptr < relend && relptr->ptr < ptr)      \
        ++relptr;                                       \
      if (relptr < relend && relptr->ptr == ptr)        \
      {                                               \
        if (reltype == SHT_REL)                       \
          dret += relptr->addend;                     \
        else                                          \
          dret = relptr->addend;                      \
      }                                               \
    }                                                   \
    dret;                                                 \
  })

#define read_32_relocated(ptr) ({                       \
    uint32_t ret = do_read_32_relocated (ptr);            \
    ptr += 4;                                             \
    ret;                                                  \
  })

struct abbrev_attr
{
  unsigned int attr;
  unsigned int form;
};

struct abbrev_tag
{
  unsigned int       tag;
  int                nattr;
  struct abbrev_attr attr[0];
};

static GHashTable *
read_abbrev (DebuginfoData *data, unsigned char *ptr)
{
  GHashTable *h;
  unsigned int attr, entry, form;
  struct abbrev_tag *t;
  int size;

  h = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                             NULL, g_free);

  while ((attr = read_uleb128 (ptr)) != 0)
    {
      size = 10;
      entry = attr;
      t = g_malloc (sizeof (*t) + size * sizeof (struct abbrev_attr));
      t->tag = read_uleb128 (ptr);
      t->nattr = 0;
      ++ptr; /* skip children flag.  */
      while ((attr = read_uleb128 (ptr)) != 0)
        {
          if (t->nattr == size)
            {
              size += 10;
              t = g_realloc (t, sizeof (*t) + size * sizeof (struct abbrev_attr));
            }
          form = read_uleb128 (ptr);
          if (form == 2 || (form > DW_FORM_flag_present && form != DW_FORM_ref_sig8))
            g_warning ("%s: Unknown DWARF DW_FORM_%d", data->filename, form);

          t->attr[t->nattr].attr = attr;
          t->attr[t->nattr++].form = form;
        }
      if (read_uleb128 (ptr) != 0)
        g_warning ("%s: DWARF abbreviation does not end with 2 zeros", data->filename);
      g_hash_table_insert (h, GINT_TO_POINTER (entry), t);
    }

  return h;
}

#define IS_DIR_SEPARATOR(c) ((c) == '/')

static char *
canonicalize_path (const char *s, char *d)
{
  char *rv = d;
  char *droot;

  if (IS_DIR_SEPARATOR (*s))
    {
      *d++ = *s++;
      if (IS_DIR_SEPARATOR (*s) && !IS_DIR_SEPARATOR (s[1]))
        /* Special case for "//foo" meaning a Posix namespace  escape.  */
        *d++ = *s++;
      while (IS_DIR_SEPARATOR (*s))
        s++;
    }
  droot = d;

  while (*s)
    {
      /* At this point, we're always at the beginning of a path segment.  */

      if (s[0] == '.' && (s[1] == 0 || IS_DIR_SEPARATOR (s[1])))
        {
          s++;
          if (*s)
            while (IS_DIR_SEPARATOR (*s))
              ++s;
        }
      else if (s[0] == '.' && s[1] == '.' &&
               (s[2] == 0 || IS_DIR_SEPARATOR (s[2])))
        {
          char *pre = d - 1; /* includes slash */
          while (droot < pre && IS_DIR_SEPARATOR (*pre))
            pre--;
          if (droot <= pre && !IS_DIR_SEPARATOR (*pre))
            {
              while (droot < pre && !IS_DIR_SEPARATOR (*pre))
                pre--;
              /* pre now points to the slash */
              if (droot < pre)
                pre++;
              if (pre + 3 == d && pre[0] == '.' && pre[1] == '.')
                {
                  *d++ = *s++;
                  *d++ = *s++;
                }
              else
                {
                  d = pre;
                  s += 2;
                  if (*s)
                    while (IS_DIR_SEPARATOR (*s))
                      s++;
                }
            }
          else
            {
              *d++ = *s++;
              *d++ = *s++;
            }
        }
      else
        {
          while (*s && !IS_DIR_SEPARATOR (*s))
            *d++ = *s++;
        }

      if (IS_DIR_SEPARATOR (*s))
        {
          *d++ = *s++;
          while (IS_DIR_SEPARATOR (*s))
            s++;
        }
    }
  while (droot < d && IS_DIR_SEPARATOR (d[-1]))
    --d;
  if (d == rv)
    *d++ = '.';
  *d = 0;

  return rv;
}

static gboolean
handle_dwarf2_line (DebuginfoData *data, uint32_t off, char *comp_dir, GHashTable *files, GError **error)
{
  unsigned char *ptr = data->debug_sections[DEBUG_LINE].data, *dir;
  unsigned char **dirt;
  unsigned char *endsec = ptr + data->debug_sections[DEBUG_LINE].size;
  unsigned char *endcu, *endprol;
  unsigned char opcode_base;
  uint32_t value, dirt_cnt;
  size_t comp_dir_len = !comp_dir ? 0 : strlen (comp_dir);


  /* XXX: RhBug:929365, should we error out instead of ignoring? */
  if (ptr == NULL)
    return TRUE;

  ptr += off;

  endcu = ptr + 4;
  endcu += read_32 (ptr);
  if (endcu == ptr + 0xffffffff)
    return flatpak_fail (error, "%s: 64-bit DWARF not supported", data->filename);

  if (endcu > endsec)
    return flatpak_fail (error, "%s: .debug_line CU does not fit into section", data->filename);

  value = read_16 (ptr);
  if (value != 2 && value != 3 && value != 4)
    return flatpak_fail (error, "%s: DWARF version %d unhandled", data->filename, value);

  endprol = ptr + 4;
  endprol += read_32 (ptr);
  if (endprol > endcu)
    return flatpak_fail (error, "%s: .debug_line CU prologue does not fit into CU", data->filename);

  opcode_base = ptr[4 + (value >= 4)];
  ptr = dir = ptr + 4 + (value >= 4) + opcode_base;

  /* dir table: */
  value = 1;
  while (*ptr != 0)
    {
      ptr = (unsigned char *) strchr ((char *) ptr, 0) + 1;
      ++value;
    }

  dirt = (unsigned char **) alloca (value * sizeof (unsigned char *));
  dirt[0] = (unsigned char *) ".";
  dirt_cnt = 1;
  ptr = dir;
  while (*ptr != 0)
    {
      dirt[dirt_cnt++] = ptr;
      ptr = (unsigned char *) strchr ((char *) ptr, 0) + 1;
    }
  ptr++;

  /* file table: */
  while (*ptr != 0)
    {
      char *s, *file;
      size_t file_len, dir_len;

      file = (char *) ptr;
      ptr = (unsigned char *) strchr ((char *) ptr, 0) + 1;
      value = read_uleb128 (ptr);

      if (value >= dirt_cnt)
        return flatpak_fail (error, "%s: Wrong directory table index %u",  data->filename, value);

      file_len = strlen (file);
      dir_len = strlen ((char *) dirt[value]);
      s = g_malloc (comp_dir_len + 1 + file_len + 1 + dir_len + 1);
      if (*file == '/')
        {
          memcpy (s, file, file_len + 1);
        }
      else if (*dirt[value] == '/')
        {
          memcpy (s, dirt[value], dir_len);
          s[dir_len] = '/';
          memcpy (s + dir_len + 1, file, file_len + 1);
        }
      else
        {
          char *p = s;
          if (comp_dir_len != 0)
            {
              memcpy (s, comp_dir, comp_dir_len);
              s[comp_dir_len] = '/';
              p += comp_dir_len + 1;
            }
          memcpy (p, dirt[value], dir_len);
          p[dir_len] = '/';
          memcpy (p + dir_len + 1, file, file_len + 1);
        }
      canonicalize_path (s, s);

      if (s)
        g_hash_table_insert (files, s, NULL);

      (void) read_uleb128 (ptr);
      (void) read_uleb128 (ptr);
    }
  ++ptr;

  return TRUE;
}

static unsigned char *
handle_attributes (DebuginfoData *data, unsigned char *ptr, struct abbrev_tag *t, GHashTable *files, GError **error)
{
  int i;
  uint32_t list_offs;
  int found_list_offs;
  g_autofree char *comp_dir = NULL;

  comp_dir = NULL;
  list_offs = 0;
  found_list_offs = 0;
  for (i = 0; i < t->nattr; ++i)
    {
      uint32_t form = t->attr[i].form;
      size_t len = 0;

      while (1)
        {
          if (t->attr[i].attr == DW_AT_stmt_list)
            {
              if (form == DW_FORM_data4 ||
                  form == DW_FORM_sec_offset)
                {
                  list_offs = do_read_32_relocated (ptr);
                  found_list_offs = 1;
                }
            }

          if (t->attr[i].attr == DW_AT_comp_dir)
            {
              if (form == DW_FORM_string)
                {
                  g_free (comp_dir);
                  comp_dir = g_strdup ((char *) ptr);
                }
              else if (form == DW_FORM_strp &&
                       data->debug_sections[DEBUG_STR].data)
                {
                  char *dir;

                  dir = (char *) data->debug_sections[DEBUG_STR].data
                        + do_read_32_relocated (ptr);

                  g_free (comp_dir);
                  comp_dir = g_strdup (dir);
                }
            }
          else if ((t->tag == DW_TAG_compile_unit ||
                    t->tag == DW_TAG_partial_unit) &&
                   t->attr[i].attr == DW_AT_name &&
                   form == DW_FORM_strp &&
                   data->debug_sections[DEBUG_STR].data)
            {
              char *name;

              name = (char *) data->debug_sections[DEBUG_STR].data
                     + do_read_32_relocated (ptr);
              if (*name == '/' && comp_dir == NULL)
                {
                  char *enddir = strrchr (name, '/');

                  if (enddir != name)
                    {
                      comp_dir = g_malloc (enddir - name + 1);
                      memcpy (comp_dir, name, enddir - name);
                      comp_dir[enddir - name] = '\0';
                    }
                  else
                    {
                      comp_dir = g_strdup ("/");
                    }
                }

            }

          switch (form)
            {
            case DW_FORM_ref_addr:
              if (cu_version == 2)
                ptr += ptr_size;
              else
                ptr += 4;
              break;

            case DW_FORM_flag_present:
              break;

            case DW_FORM_addr:
              ptr += ptr_size;
              break;

            case DW_FORM_ref1:
            case DW_FORM_flag:
            case DW_FORM_data1:
              ++ptr;
              break;

            case DW_FORM_ref2:
            case DW_FORM_data2:
              ptr += 2;
              break;

            case DW_FORM_ref4:
            case DW_FORM_data4:
            case DW_FORM_sec_offset:
              ptr += 4;
              break;

            case DW_FORM_ref8:
            case DW_FORM_data8:
            case DW_FORM_ref_sig8:
              ptr += 8;
              break;

            case DW_FORM_sdata:
            case DW_FORM_ref_udata:
            case DW_FORM_udata:
              (void) read_uleb128 (ptr);
              break;

            case DW_FORM_strp:
              ptr += 4;
              break;

            case DW_FORM_string:
              ptr = (unsigned char *) strchr ((char *) ptr, '\0') + 1;
              break;

            case DW_FORM_indirect:
              form = read_uleb128 (ptr);
              continue;

            case DW_FORM_block1:
              len = *ptr++;
              break;

            case DW_FORM_block2:
              len = read_16 (ptr);
              form = DW_FORM_block1;
              break;

            case DW_FORM_block4:
              len = read_32 (ptr);
              form = DW_FORM_block1;
              break;

            case DW_FORM_block:
            case DW_FORM_exprloc:
              len = read_uleb128 (ptr);
              form = DW_FORM_block1;
              g_assert (len < UINT_MAX);
              break;

            default:
              g_warning ("%s: Unknown DWARF DW_FORM_%d", data->filename, form);
              return NULL;
            }

          if (form == DW_FORM_block1)
            ptr += len;

          break;
        }
    }

  /* Ensure the CU current directory will exist even if only empty.  Source
     filenames possibly located in its parent directories refer relatively to
     it and the debugger (GDB) cannot safely optimize out the missing
     CU current dir subdirectories.  */
  if (comp_dir)
    g_hash_table_insert (files, g_strdup (comp_dir), NULL);

  if (found_list_offs &&
      !handle_dwarf2_line (data, list_offs, comp_dir, files, error))
    return NULL;

  return ptr;
}

static int
rel_cmp (const void *a, const void *b)
{
  REL *rela = (REL *) a, *relb = (REL *) b;

  if (rela->ptr < relb->ptr)
    return -1;

  if (rela->ptr > relb->ptr)
    return 1;

  return 0;
}

static gboolean
handle_dwarf2_section (DebuginfoData *data, GHashTable *files, GError **error)
{
  Elf_Data *e_data;
  int i;
  debug_section_t *debug_sections;

  ptr_size = 0;

  if (data->ehdr.e_ident[EI_DATA] == ELFDATA2LSB)
    {
      do_read_16 = buf_read_ule16;
      do_read_32 = buf_read_ule32;
    }
  else if (data->ehdr.e_ident[EI_DATA] == ELFDATA2MSB)
    {
      do_read_16 = buf_read_ube16;
      do_read_32 = buf_read_ube32;
    }
  else
    {
      return flatpak_fail (0, 0, "%s: Wrong ELF data encoding", data->filename);
    }

  debug_sections = data->debug_sections;

  if (debug_sections[DEBUG_INFO].data != NULL)
    {
      unsigned char *ptr, *endcu, *endsec;
      uint32_t value;
      struct abbrev_tag *t;
      g_autofree REL *relbuf = NULL;

      if (debug_sections[DEBUG_INFO].relsec)
        {
          Elf_Scn *scn;
          int ndx, maxndx;
          GElf_Rel rel;
          GElf_Rela rela;
          GElf_Sym sym;
          GElf_Addr base = data->shdr[debug_sections[DEBUG_INFO].sec].sh_addr;
          Elf_Data *symdata = NULL;
          int rtype;

          i = debug_sections[DEBUG_INFO].relsec;
          scn = data->scns[i];
          e_data = elf_getdata (scn, NULL);
          g_assert (e_data != NULL && e_data->d_buf != NULL);
          g_assert (elf_getdata (scn, e_data) == NULL);
          g_assert (e_data->d_off == 0);
          g_assert (e_data->d_size == data->shdr[i].sh_size);
          maxndx = data->shdr[i].sh_size / data->shdr[i].sh_entsize;
          relbuf = g_malloc (maxndx * sizeof (REL));
          reltype = data->shdr[i].sh_type;

          symdata = elf_getdata (data->scns[data->shdr[i].sh_link], NULL);
          g_assert (symdata != NULL && symdata->d_buf != NULL);
          g_assert (elf_getdata (data->scns[data->shdr[i].sh_link], symdata) == NULL);
          g_assert (symdata->d_off == 0);
          g_assert (symdata->d_size == data->shdr[data->shdr[i].sh_link].sh_size);

          for (ndx = 0, relend = relbuf; ndx < maxndx; ++ndx)
            {
              if (data->shdr[i].sh_type == SHT_REL)
                {
                  gelf_getrel (e_data, ndx, &rel);
                  rela.r_offset = rel.r_offset;
                  rela.r_info = rel.r_info;
                  rela.r_addend = 0;
                }
              else
                {
                  gelf_getrela (e_data, ndx, &rela);
                }
              gelf_getsym (symdata, ELF64_R_SYM (rela.r_info), &sym);
              /* Relocations against section symbols are uninteresting
                 in REL.  */
              if (data->shdr[i].sh_type == SHT_REL && sym.st_value == 0)
                continue;
              /* Only consider relocations against .debug_str, .debug_line
                 and .debug_abbrev.  */
              if (sym.st_shndx != debug_sections[DEBUG_STR].sec &&
                  sym.st_shndx != debug_sections[DEBUG_LINE].sec &&
                  sym.st_shndx != debug_sections[DEBUG_ABBREV].sec)
                continue;
              rela.r_addend += sym.st_value;
              rtype = ELF64_R_TYPE (rela.r_info);
              switch (data->ehdr.e_machine)
                {
                case EM_SPARC:
                case EM_SPARC32PLUS:
                case EM_SPARCV9:
                  if (rtype != R_SPARC_32 && rtype != R_SPARC_UA32)
                    goto fail;
                  break;

                case EM_386:
                  if (rtype != R_386_32)
                    goto fail;
                  break;

                case EM_PPC:
                case EM_PPC64:
                  if (rtype != R_PPC_ADDR32 && rtype != R_PPC_UADDR32)
                    goto fail;
                  break;

                case EM_S390:
                  if (rtype != R_390_32)
                    goto fail;
                  break;

                case EM_IA_64:
                  if (rtype != R_IA64_SECREL32LSB)
                    goto fail;
                  break;

                case EM_X86_64:
                  if (rtype != R_X86_64_32)
                    goto fail;
                  break;

                case EM_ALPHA:
                  if (rtype != R_ALPHA_REFLONG)
                    goto fail;
                  break;

#if defined(EM_AARCH64) && defined(R_AARCH64_ABS32)
                case EM_AARCH64:
                  if (rtype != R_AARCH64_ABS32)
                    goto fail;
                  break;

#endif
                case EM_68K:
                  if (rtype != R_68K_32)
                    goto fail;
                  break;

                default:
fail:
                  return flatpak_fail (error, "%s: Unhandled relocation %d in .debug_info section",
                                       data->filename, rtype);
                }
              relend->ptr = debug_sections[DEBUG_INFO].data
                            + (rela.r_offset - base);
              relend->addend = rela.r_addend;
              ++relend;
            }
          if (relbuf == relend)
            {
              g_free (relbuf);
              relbuf = NULL;
              relend = NULL;
            }
          else
            {
              qsort (relbuf, relend - relbuf, sizeof (REL), rel_cmp);
            }
        }

      ptr = debug_sections[DEBUG_INFO].data;
      relptr = relbuf;
      endsec = ptr + debug_sections[DEBUG_INFO].size;
      while (ptr != NULL && ptr < endsec)
        {
          g_autoptr(GHashTable) abbrev = NULL;

          if (ptr + 11 > endsec)
            return flatpak_fail (error, "%s: .debug_info CU header too small", data->filename);

          endcu = ptr + 4;
          endcu += read_32 (ptr);
          if (endcu == ptr + 0xffffffff)
            return flatpak_fail (error, "%s: 64-bit DWARF not supported", data->filename);

          if (endcu > endsec)
            return flatpak_fail (error, "%s: .debug_info too small", data->filename);

          cu_version = read_16 (ptr);
          if (cu_version != 2 && cu_version != 3 && cu_version != 4)
            return flatpak_fail (error, "%s: DWARF version %d unhandled", data->filename, cu_version);

          value = read_32_relocated (ptr);
          if (value >= debug_sections[DEBUG_ABBREV].size)
            {
              if (debug_sections[DEBUG_ABBREV].data == NULL)
                return flatpak_fail (error, "%s: .debug_abbrev not present", data->filename);
              else
                return flatpak_fail (error, "%s: DWARF CU abbrev offset too large", data->filename);
            }

          if (ptr_size == 0)
            {
              ptr_size = read_1 (ptr);
              if (ptr_size != 4 && ptr_size != 8)
                return flatpak_fail (error, "%s: Invalid DWARF pointer size %d", data->filename, ptr_size);
            }
          else if (read_1 (ptr) != ptr_size)
            {
              return flatpak_fail (error, "%s: DWARF pointer size differs between CUs", data->filename);
            }

          abbrev = read_abbrev (data,
                                debug_sections[DEBUG_ABBREV].data + value);

          while (ptr < endcu)
            {
              guint entry = read_uleb128 (ptr);
              if (entry == 0)
                continue;
              t = g_hash_table_lookup (abbrev, GINT_TO_POINTER (entry));
              if (t == NULL)
                {
                  g_warning ("%s: Could not find DWARF abbreviation %d", data->filename, entry);
                }
              else
                {
                  ptr = handle_attributes (data, ptr, t, files, error);
                  if (ptr == NULL)
                    return FALSE;
                }
            }
        }
    }

  return TRUE;
}

static const char *
strptr (Elf_Scn **scns, GElf_Shdr *shdr, int sec, off_t offset)
{
  Elf_Scn *scn;
  Elf_Data *data;

  scn = scns[sec];
  if (offset >= 0 && (GElf_Addr) offset < shdr[sec].sh_size)
    {
      data = NULL;
      while ((data = elf_rawdata (scn, data)) != NULL)
        {
          if (data->d_buf &&
              offset >= data->d_off &&
              offset < data->d_off + data->d_size)
            return (const char *) data->d_buf + (offset - data->d_off);
        }
    }

  return NULL;
}

char **
builder_get_debuginfo_file_references (const char *filename, GError **error)
{
  Elf *elf = NULL;
  GElf_Ehdr ehdr;
  int i, j;
  glnx_fd_close int fd = -1;
  DebuginfoData data = { 0 };
  g_autofree GElf_Shdr *shdr = NULL;
  g_autofree Elf_Scn **scns = NULL;
  debug_section_t *debug_sections;

  g_autoptr(GHashTable) files = NULL;
  char **res;

  fd = open (filename, O_RDONLY);
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  elf = elf_begin (fd, ELF_C_RDWR_MMAP, NULL);
  if (elf == NULL)
    {
      flatpak_fail (error, "cannot open ELF file: %s", elf_errmsg (-1));
      return NULL;
    }

  if (elf_kind (elf) != ELF_K_ELF)
    {
      flatpak_fail (error, "\"%s\" is not an ELF file", filename);
      return NULL;
    }

  if (gelf_getehdr (elf, &ehdr) == NULL)
    {
      flatpak_fail (error, "cannot get the ELF header: %s", elf_errmsg (-1));
      return NULL;
    }

  if (ehdr.e_type != ET_DYN && ehdr.e_type != ET_EXEC && ehdr.e_type != ET_REL)
    {
      flatpak_fail (error, "\"%s\" is not a shared library", filename);
      return NULL;
    }

  elf_flagelf (elf, ELF_C_SET, ELF_F_LAYOUT);

  shdr = g_new0 (GElf_Shdr, ehdr.e_shnum);
  scns =  g_new0 (Elf_Scn *, ehdr.e_shnum);

  for (i = 0; i < ehdr.e_shnum; ++i)
    {
      scns[i] = elf_getscn (elf, i);
      gelf_getshdr (scns[i], &shdr[i]);
    }

  data.elf = elf;
  data.ehdr = ehdr;
  data.shdr = shdr;
  data.scns = scns;
  data.filename = filename;

  /* Locate all debug sections */
  debug_sections = data.debug_sections;
  for (i = 1; i < ehdr.e_shnum; ++i)
    {
      if (!(shdr[i].sh_flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR)) && shdr[i].sh_size)
        {
          const char *name = strptr (scns, shdr, ehdr.e_shstrndx, shdr[i].sh_name);

          if (g_str_has_prefix (name, ".debug_"))
            {
              for (j = 0; j < NUM_DEBUG_SECTIONS; ++j)
                {
                  if (strcmp (name, debug_section_names[j]) == 0)
                    {
                      Elf_Scn *scn = scns[i];
                      Elf_Data *e_data;

                      if (debug_sections[j].data)
                        g_warning ("%s: Found two copies of %s section", filename, name);

                      e_data = elf_rawdata (scn, NULL);
                      g_assert (e_data != NULL && e_data->d_buf != NULL);
                      g_assert (elf_rawdata (scn, e_data) == NULL);
                      g_assert (e_data->d_off == 0);
                      g_assert (e_data->d_size == shdr[i].sh_size);
                      debug_sections[j].data = e_data->d_buf;
                      debug_sections[j].elf_data = e_data;
                      debug_sections[j].size = e_data->d_size;
                      debug_sections[j].sec = i;
                      break;
                    }
                }

              if (j == NUM_DEBUG_SECTIONS)
                g_warning ("%s: Unknown debugging section %s", filename, name);
            }
          else if (ehdr.e_type == ET_REL &&
                   ((shdr[i].sh_type == SHT_REL && g_str_has_prefix (name, ".rel.debug_")) ||
                    (shdr[i].sh_type == SHT_RELA && g_str_has_prefix (name, ".rela.debug_"))))
            {
              for (j = 0; j < NUM_DEBUG_SECTIONS; ++j)
                if (strcmp (name + sizeof (".rel") - 1
                            + (shdr[i].sh_type == SHT_RELA),
                            debug_section_names[j]) == 0)
                  {
                    debug_sections[j].relsec = i;
                    break;
                  }
            }
        }
    }

  files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (!handle_dwarf2_section (&data, files, error))
    return NULL;

  if (elf_end (elf) < 0)
    g_warning ("elf_end failed: %s\n", elf_errmsg (elf_errno ()));

  res = (char **) g_hash_table_get_keys_as_array (files, NULL);
  g_hash_table_steal_all (files);
  return res;
}

typedef struct {
  GDBusConnection *connection;
  GMainLoop *loop;
  GError    *splice_error;
  guint32 client_pid;
  guint32 exit_status;
  int refs;
} HostCommandCallData;

static void
host_command_call_exit (HostCommandCallData *data)
{
  data->refs--;
  if (data->refs == 0)
    g_main_loop_quit (data->loop);
}

static void
output_spliced_cb (GObject      *obj,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  HostCommandCallData *data = user_data;

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &data->splice_error);
  host_command_call_exit (data);
}

static void
host_command_exited_cb (GDBusConnection *connection,
                        const gchar     *sender_name,
                        const gchar     *object_path,
                        const gchar     *interface_name,
                        const gchar     *signal_name,
                        GVariant        *parameters,
                        gpointer         user_data)
{
  guint32 client_pid, exit_status;
  HostCommandCallData *data = (HostCommandCallData *)user_data;

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(uu)")))
    return;

  g_variant_get (parameters, "(uu)", &client_pid, &exit_status);

  if (client_pid == data->client_pid)
    {
      g_print ("host_command_exited_cb %d %d\n", client_pid, exit_status);
      data->exit_status = exit_status;
      host_command_call_exit (data);
    }
}

static gboolean
sigterm_handler (gpointer user_data)
{
  HostCommandCallData *data = (HostCommandCallData *)user_data;

  g_dbus_connection_call_sync (data->connection,
                               "org.freedesktop.Flatpak",
                               "/org/freedesktop/Flatpak/Development",
                               "org.freedesktop.Flatpak.Development",
                               "HostCommandSignal",
                               g_variant_new ("(uub)", data->client_pid, SIGTERM, TRUE),
                               NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1,
                               NULL, NULL);

  kill (getpid (), SIGTERM);
  return TRUE;
}

static gboolean
sigint_handler (gpointer user_data)
{
  HostCommandCallData *data = (HostCommandCallData *)user_data;

  g_dbus_connection_call_sync (data->connection,
                               "org.freedesktop.Flatpak",
                               "/org/freedesktop/Flatpak/Development",
                               "org.freedesktop.Flatpak.Development",
                               "HostCommandSignal",
                               g_variant_new ("(uub)", data->client_pid, SIGINT, TRUE),
                               NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1,
                               NULL, NULL);

  kill (getpid (), SIGTERM);
  return TRUE;
}

gboolean
builder_host_spawnv (GFile                *dir,
                     char                **output,
                     GError              **error,
                     const gchar * const  *argv)
{
  guint32 client_pid;
  GVariantBuilder *fd_builder = g_variant_builder_new (G_VARIANT_TYPE("a{uh}"));
  GVariantBuilder *env_builder = g_variant_builder_new (G_VARIANT_TYPE("a{ss}"));
  g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new ();
  gint stdout_handle, stdin_handle, stderr_handle;
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_auto(GStrv) env_vars = NULL;
  guint subscription;
  HostCommandCallData data = { NULL };
  guint sigterm_id = 0, sigint_id = 0;
  g_autofree gchar *commandline = NULL;
  g_autoptr(GOutputStream) out = NULL;
  int pipefd[2];
  int i;

  commandline = g_strjoinv (" ", (gchar **) argv);
  g_debug ("Running '%s' on host", commandline);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (connection == NULL)
    return FALSE;

  loop = g_main_loop_new (NULL, FALSE);
  data.connection = connection;
  data.loop = loop;
  data.refs = 1;

  subscription = g_dbus_connection_signal_subscribe (connection,
                                                     NULL,
                                                     "org.freedesktop.Flatpak.Development",
                                                     "HostCommandExited",
                                                     "/org/freedesktop/Flatpak/Development",
                                                     NULL,
                                                     G_DBUS_SIGNAL_FLAGS_NONE,
                                                     host_command_exited_cb,
                                                     &data, NULL);

  stdin_handle = g_unix_fd_list_append (fd_list, 0, error);
  if (stdin_handle == -1)
    return FALSE;

  if (output)
    {
      g_autoptr(GInputStream) in = NULL;

      if (pipe2 (pipefd, O_CLOEXEC) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      data.refs++;
      in = g_unix_input_stream_new (pipefd[0], TRUE);
      out = g_memory_output_stream_new_resizable ();
      g_output_stream_splice_async (out,
                                    in,
                                    G_OUTPUT_STREAM_SPLICE_NONE,
                                    0,
                                    NULL,
                                    output_spliced_cb,
                                    &data);
      stdout_handle = g_unix_fd_list_append (fd_list, pipefd[1], error);
      close (pipefd[1]);
      if (stdout_handle == -1)
        return FALSE;
    }
  else
    {
      stdout_handle = g_unix_fd_list_append (fd_list, 1, error);
      if (stdout_handle == -1)
        return FALSE;
    }

  stderr_handle = g_unix_fd_list_append (fd_list, 2, error);
  if (stderr_handle == -1)
    return FALSE;

  g_variant_builder_add (fd_builder, "{uh}", 0, stdin_handle);
  g_variant_builder_add (fd_builder, "{uh}", 1, stdout_handle);
  g_variant_builder_add (fd_builder, "{uh}", 2, stderr_handle);

  env_vars = g_listenv ();
  for (i = 0; env_vars[i] != NULL; i++)
    {
      const char *env_var = env_vars[i];
      g_variant_builder_add (env_builder, "{ss}", env_var, g_getenv (env_var));
    }

  sigterm_id = g_unix_signal_add (SIGTERM, sigterm_handler, &data);
  sigint_id = g_unix_signal_add (SIGINT, sigint_handler, &data);

  ret = g_dbus_connection_call_with_unix_fd_list_sync (connection,
                                                       "org.freedesktop.Flatpak",
                                                       "/org/freedesktop/Flatpak/Development",
                                                       "org.freedesktop.Flatpak.Development",
                                                       "HostCommand",
                                                       g_variant_new ("(^ay^aay@a{uh}@a{ss}u)",
                                                                      dir ? flatpak_file_get_path_cached (dir) : "",
                                                                      argv,
                                                                      g_variant_builder_end (fd_builder),
                                                                      g_variant_builder_end (env_builder),
                                                                      FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV),
                                                       G_VARIANT_TYPE ("(u)"),
                                                       G_DBUS_CALL_FLAGS_NONE, -1,
                                                       fd_list, NULL,
                                                       NULL, error);

  if (ret == NULL)
    return FALSE;


  g_variant_get (ret, "(u)", &client_pid);
  data.client_pid = client_pid;

  g_main_loop_run (loop);

  g_source_remove (sigterm_id);
  g_source_remove (sigint_id);
  g_dbus_connection_signal_unsubscribe (connection, subscription);

  if (!g_spawn_check_exit_status (data.exit_status, error))
    return FALSE;

  if (out)
    {
      if (data.splice_error)
        {
          g_propagate_error (error, data.splice_error);
          return FALSE;
        }

      /* Null terminate */
      g_output_stream_write (out, "\0", 1, NULL, NULL);
      g_output_stream_close (out, NULL, NULL);
      *output = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
    }

  return TRUE;
}

/* Similar to flatpak_spawnv, except uses the session helper HostCommand operation
   if in a sandbox */
gboolean
builder_maybe_host_spawnv (GFile                *dir,
                           char                **output,
                           GError              **error,
                           const gchar * const  *argv)
{
  if (flatpak_is_in_sandbox ())
    return builder_host_spawnv (dir, output, error, argv);

  return flatpak_spawnv (dir, output, error, argv);
}
