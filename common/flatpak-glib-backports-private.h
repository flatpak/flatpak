/*
 * Copyright 2013 Allison Karlitskaya
 * Copyright 2013 Collabora Ltd.
 * Copyright 2016 Canonical Ltd.
 * Copyright 2017-2018 Endless OS Foundation LLC
 * Copyright 2019 Red Hat, Inc
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "libglnx.h"

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

/* Please sort this file by the GLib version where it originated,
 * oldest first. */

G_BEGIN_DECLS

#if !GLIB_CHECK_VERSION (2, 40, 0)
static inline gboolean
g_key_file_save_to_file (GKeyFile    *key_file,
                         const gchar *filename,
                         GError     **error)
{
  gchar *contents;
  gboolean success;
  gsize length;

  g_return_val_if_fail (key_file != NULL, FALSE);
  g_return_val_if_fail (filename != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  contents = g_key_file_to_data (key_file, &length, NULL);
  g_assert (contents != NULL);

  success = g_file_set_contents (filename, contents, length, error);
  g_free (contents);

  return success;
}
#endif

#if !GLIB_CHECK_VERSION (2, 43, 4)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GUnixFDList, g_object_unref)
#endif

#if !GLIB_CHECK_VERSION (2, 54, 0)
static inline gboolean
g_ptr_array_find_with_equal_func (GPtrArray     *haystack,
                                  gconstpointer  needle,
                                  GEqualFunc     equal_func,
                                  guint         *index_)
{
  guint i;

  g_return_val_if_fail (haystack != NULL, FALSE);

  if (equal_func == NULL)
    equal_func = g_direct_equal;

  for (i = 0; i < haystack->len; i++)
    {
      if (equal_func (g_ptr_array_index (haystack, i), needle))
        {
          if (index_ != NULL)
            *index_ = i;
          return TRUE;
        }
    }

  return FALSE;
}

/* We're non-specific about the error behaviour, so this is good enough */
#define G_NUMBER_PARSER_ERROR (G_IO_ERROR)
#define G_NUMBER_PARSER_ERROR_INVALID (G_IO_ERROR_INVALID_ARGUMENT)
#define G_NUMBER_PARSER_ERROR_OUT_OF_BOUNDS (G_IO_ERROR_INVALID_ARGUMENT)
#define GNumberParserError (GIOErrorEnum)

gboolean g_ascii_string_to_unsigned (const gchar *str,
                                     guint        base,
                                     guint64      min,
                                     guint64      max,
                                     guint64     *out_num,
                                     GError     **error);
#endif

#if !GLIB_CHECK_VERSION (2, 56, 0)
typedef void (* GClearHandleFunc) (guint handle_id);

static inline void
g_clear_handle_id (guint            *tag_ptr,
                   GClearHandleFunc  clear_func)
{
  guint _handle_id;

  _handle_id = *tag_ptr;
  if (_handle_id > 0)
    {
      *tag_ptr = 0;
      clear_func (_handle_id);
    }
}

GDateTime *flatpak_g_date_time_new_from_iso8601 (const gchar *text,
                                                 GTimeZone   *default_tz);

static inline GDateTime *
g_date_time_new_from_iso8601 (const gchar *text, GTimeZone *default_tz)
{
  return flatpak_g_date_time_new_from_iso8601 (text, default_tz);
}
#endif

#if !GLIB_CHECK_VERSION (2, 58, 0)
/* This is a reimplementation rather than a backport, and is a little less
 * efficient than the real g_hash_table_steal_extended(), since it can't
 * see into GHashTable internals */
static inline gboolean
g_hash_table_steal_extended (GHashTable    *hash_table,
                             gconstpointer  lookup_key,
                             gpointer      *stolen_key,
                             gpointer      *stolen_value)
{
  if (g_hash_table_lookup_extended (hash_table, lookup_key, stolen_key, stolen_value))
    {
      g_hash_table_steal (hash_table, lookup_key);
      return TRUE;
    }
  else
      return FALSE;
}
#endif

#if !GLIB_CHECK_VERSION (2, 58, 0)
const gchar * const *g_get_language_names_with_category (const gchar *category_name);
#endif

#if !GLIB_CHECK_VERSION (2, 62, 0)
void g_ptr_array_extend (GPtrArray        *array_to_extend,
                         GPtrArray        *array,
                         GCopyFunc         func,
                         gpointer          user_data);
#endif

#if !GLIB_CHECK_VERSION (2, 68, 0)
guint g_string_replace (GString     *string,
                        const gchar *find,
                        const gchar *replace,
                        guint        limit);
#endif

#ifndef G_DBUS_METHOD_INVOCATION_HANDLED    /* GLib < 2.68 */
# define G_DBUS_METHOD_INVOCATION_HANDLED TRUE
# define G_DBUS_METHOD_INVOCATION_UNHANDLED FALSE
#endif

#if !GLIB_CHECK_VERSION (2, 76, 0)
/* All this code is backported directly from 2.84.1 */
static inline gboolean
g_set_str (char       **str_pointer,
           const char  *new_str)
{
  char *copy;

  if (*str_pointer == new_str ||
      (*str_pointer && new_str && strcmp (*str_pointer, new_str) == 0))
    return FALSE;

  copy = g_strdup (new_str);
  g_free (*str_pointer);
  *str_pointer = copy;

  return TRUE;
}
#endif /* GLIB_CHECK_VERSION (2, 76, 0) */

G_END_DECLS
