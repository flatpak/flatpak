/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright 1995-1998 Free Software Foundation, Inc.
 * Copyright 2004 Hidetoshi Tajima
 * Copyright 2004-2010 Christian Persch
 * Copyright 2004-2019 Red Hat, Inc
 * Copyright 2016-2018 Canonical Ltd.
 * Copyright 2019 Endless OS Foundation LLC
 * Copyright 2019 Emmanuel Fleury
 * Copyright 2021 Joshua Lee
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

#include "config.h"
#include "flatpak-glib-backports-private.h"

#include <glib/gi18n-lib.h>

/* Please sort this file by the GLib version where it originated,
 * oldest first. */

#if !GLIB_CHECK_VERSION (2, 56, 0)
/* All this code is backported directly from glib */

static void
g_date_time_get_week_number (GDateTime *datetime,
                             gint      *week_number,
                             gint      *day_of_week,
                             gint      *day_of_year)
{
  gint a, b, c, d, e, f, g, n, s, month, day, year;

  g_date_time_get_ymd (datetime, &year, &month, &day);

  if (month <= 2)
    {
      a = g_date_time_get_year (datetime) - 1;
      b = (a / 4) - (a / 100) + (a / 400);
      c = ((a - 1) / 4) - ((a - 1) / 100) + ((a - 1) / 400);
      s = b - c;
      e = 0;
      f = day - 1 + (31 * (month - 1));
    }
  else
    {
      a = year;
      b = (a / 4) - (a / 100) + (a / 400);
      c = ((a - 1) / 4) - ((a - 1) / 100) + ((a - 1) / 400);
      s = b - c;
      e = s + 1;
      f = day + (((153 * (month - 3)) + 2) / 5) + 58 + s;
    }

  g = (a + b) % 7;
  d = (f + g - e) % 7;
  n = f + 3 - d;

  if (week_number)
    {
      if (n < 0)
        *week_number = 53 - ((g - s) / 5);
      else if (n > 364 + s)
        *week_number = 1;
      else
        *week_number = (n / 7) + 1;
    }

  if (day_of_week)
    *day_of_week = d + 1;

  if (day_of_year)
    *day_of_year = f + 1;
}

#define GREGORIAN_LEAP(y)    ((((y) % 4) == 0) && (!((((y) % 100) == 0) && (((y) % 400) != 0))))

/* Parse integers in the form d (week days), dd (hours etc), ddd (ordinal days) or dddd (years) */
static gboolean
get_iso8601_int (const gchar *text, gsize length, gint *value)
{
  gint i, v = 0;

  if (length < 1 || length > 4)
    return FALSE;

  for (i = 0; i < length; i++)
    {
      const gchar c = text[i];
      if (c < '0' || c > '9')
        return FALSE;
      v = v * 10 + (c - '0');
    }

  *value = v;
  return TRUE;
}

/* Parse seconds in the form ss or ss.sss (variable length decimal) */
static gboolean
get_iso8601_seconds (const gchar *text, gsize length, gdouble *value)
{
  gint i;
  gdouble divisor = 1, v = 0;

  if (length < 2)
    return FALSE;

  for (i = 0; i < 2; i++)
    {
      const gchar c = text[i];
      if (c < '0' || c > '9')
        return FALSE;
      v = v * 10 + (c - '0');
    }

  if (length > 2 && !(text[i] == '.' || text[i] == ','))
    return FALSE;
  i++;
  if (i == length)
    return FALSE;

  for (; i < length; i++)
    {
      const gchar c = text[i];
      if (c < '0' || c > '9')
        return FALSE;
      v = v * 10 + (c - '0');
      divisor *= 10;
    }

  *value = v / divisor;
  return TRUE;
}

static GDateTime *
g_date_time_new_ordinal (GTimeZone *tz, gint year, gint ordinal_day, gint hour, gint minute, gdouble seconds)
{
  GDateTime *dt, *dt2;

  if (ordinal_day < 1 || ordinal_day > (GREGORIAN_LEAP (year) ? 366 : 365))
    return NULL;

  dt = g_date_time_new (tz, year, 1, 1, hour, minute, seconds);
  dt2 = g_date_time_add_days (dt, ordinal_day - 1);
  g_date_time_unref (dt);

  return dt2;
}

static GDateTime *
g_date_time_new_week (GTimeZone *tz, gint year, gint week, gint week_day, gint hour, gint minute, gdouble seconds)
{
  gint64 p;
  gint max_week, jan4_week_day, ordinal_day;
  GDateTime *dt;

  p = (year * 365 + (year / 4) - (year / 100) + (year / 400)) % 7;
  max_week = p == 4 ? 53 : 52;

  if (week < 1 || week > max_week || week_day < 1 || week_day > 7)
    return NULL;

  dt = g_date_time_new (tz, year, 1, 4, 0, 0, 0);
  g_date_time_get_week_number (dt, NULL, &jan4_week_day, NULL);
  g_date_time_unref (dt);

  ordinal_day = (week * 7) + week_day - (jan4_week_day + 3);
  if (ordinal_day < 0)
    {
      year--;
      ordinal_day += GREGORIAN_LEAP (year) ? 366 : 365;
    }
  else if (ordinal_day > (GREGORIAN_LEAP (year) ? 366 : 365))
    {
      ordinal_day -= (GREGORIAN_LEAP (year) ? 366 : 365);
      year++;
    }

  return g_date_time_new_ordinal (tz, year, ordinal_day, hour, minute, seconds);
}

static GDateTime *
parse_iso8601_date (const gchar *text, gsize length,
                    gint hour, gint minute, gdouble seconds, GTimeZone *tz)
{
  /* YYYY-MM-DD */
  if (length == 10 && text[4] == '-' && text[7] == '-')
    {
      int year, month, day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 5, 2, &month) ||
          !get_iso8601_int (text + 8, 2, &day))
        return NULL;
      return g_date_time_new (tz, year, month, day, hour, minute, seconds);
    }
  /* YYYY-DDD */
  else if (length == 8 && text[4] == '-')
    {
      gint year, ordinal_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 5, 3, &ordinal_day))
        return NULL;
      return g_date_time_new_ordinal (tz, year, ordinal_day, hour, minute, seconds);
    }
  /* YYYY-Www-D */
  else if (length == 10 && text[4] == '-' && text[5] == 'W' && text[8] == '-')
    {
      gint year, week, week_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 6, 2, &week) ||
          !get_iso8601_int (text + 9, 1, &week_day))
        return NULL;
      return g_date_time_new_week (tz, year, week, week_day, hour, minute, seconds);
    }
  /* YYYYWwwD */
  else if (length == 8 && text[4] == 'W')
    {
      gint year, week, week_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 5, 2, &week) ||
          !get_iso8601_int (text + 7, 1, &week_day))
        return NULL;
      return g_date_time_new_week (tz, year, week, week_day, hour, minute, seconds);
    }
  /* YYYYMMDD */
  else if (length == 8)
    {
      int year, month, day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 4, 2, &month) ||
          !get_iso8601_int (text + 6, 2, &day))
        return NULL;
      return g_date_time_new (tz, year, month, day, hour, minute, seconds);
    }
  /* YYYYDDD */
  else if (length == 7)
    {
      gint year, ordinal_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 4, 3, &ordinal_day))
        return NULL;
      return g_date_time_new_ordinal (tz, year, ordinal_day, hour, minute, seconds);
    }
  else
    return FALSE;
}

static GTimeZone *
parse_iso8601_timezone (const gchar *text, gsize length, gssize *tz_offset)
{
  gint i, tz_length, offset_sign = 1, offset_hours, offset_minutes;
  GTimeZone *tz;

  /* UTC uses Z suffix  */
  if (length > 0 && text[length - 1] == 'Z')
    {
      *tz_offset = length - 1;
      return g_time_zone_new_utc ();
    }

  /* Look for '+' or '-' of offset */
  for (i = length - 1; i >= 0; i--)
    if (text[i] == '+' || text[i] == '-')
      {
        offset_sign = text[i] == '-' ? -1 : 1;
        break;
      }
  if (i < 0)
    return NULL;
  tz_length = length - i;

  /* +hh:mm or -hh:mm */
  if (tz_length == 6 && text[i + 3] == ':')
    {
      if (!get_iso8601_int (text + i + 1, 2, &offset_hours) ||
          !get_iso8601_int (text + i + 4, 2, &offset_minutes))
        return NULL;
    }
  /* +hhmm or -hhmm */
  else if (tz_length == 5)
    {
      if (!get_iso8601_int (text + i + 1, 2, &offset_hours) ||
          !get_iso8601_int (text + i + 3, 2, &offset_minutes))
        return NULL;
    }
  /* +hh or -hh */
  else if (tz_length == 3)
    {
      if (!get_iso8601_int (text + i + 1, 2, &offset_hours))
        return NULL;
      offset_minutes = 0;
    }
  else
    return NULL;

  *tz_offset = i;
  tz = g_time_zone_new (text + i);

  /* Double-check that the GTimeZone matches our interpretation of the timezone.
   * Failure would indicate a bug either here of in the GTimeZone code. */
  g_assert (g_time_zone_get_offset (tz, 0) == offset_sign * (offset_hours * 3600 + offset_minutes * 60));

  return tz;
}

static gboolean
parse_iso8601_time (const gchar *text, gsize length,
                    gint *hour, gint *minute, gdouble *seconds, GTimeZone **tz)
{
  gssize tz_offset = -1;

  /* Check for timezone suffix */
  *tz = parse_iso8601_timezone (text, length, &tz_offset);
  if (tz_offset >= 0)
    length = tz_offset;

  /* hh:mm:ss(.sss) */
  if (length >= 8 && text[2] == ':' && text[5] == ':')
    {
      return get_iso8601_int (text, 2, hour) &&
             get_iso8601_int (text + 3, 2, minute) &&
             get_iso8601_seconds (text + 6, length - 6, seconds);
    }
  /* hhmmss(.sss) */
  else if (length >= 6)
    {
      return get_iso8601_int (text, 2, hour) &&
             get_iso8601_int (text + 2, 2, minute) &&
             get_iso8601_seconds (text + 4, length - 4, seconds);
    }
  else
    return FALSE;
}


GDateTime *
flatpak_g_date_time_new_from_iso8601 (const gchar *text, GTimeZone *default_tz)
{
  gint length, date_length = -1;
  gint hour = 0, minute = 0;
  gdouble seconds = 0.0;
  GTimeZone *tz = NULL;
  GDateTime *datetime = NULL;

  g_return_val_if_fail (text != NULL, NULL);

  /* Count length of string and find date / time separator ('T', 't', or ' ') */
  for (length = 0; text[length] != '\0'; length++)
    {
      if (date_length < 0 && (text[length] == 'T' || text[length] == 't' || text[length] == ' '))
        date_length = length;
    }

  if (date_length < 0)
    return NULL;

  if (!parse_iso8601_time (text + date_length + 1, length - (date_length + 1),
                           &hour, &minute, &seconds, &tz))
    goto out;
  if (tz == NULL && default_tz == NULL)
    return NULL;

  datetime = parse_iso8601_date (text, date_length, hour, minute, seconds, tz ? tz : default_tz);

out:
  if (tz != NULL)
    g_time_zone_unref (tz);
  return datetime;
}
#endif

#if !GLIB_CHECK_VERSION (2, 58, 0)
/* All this code is backported directly from glib 2.66 */

typedef struct _GLanguageNamesCache GLanguageNamesCache;

struct _GLanguageNamesCache {
  gchar *languages;
  gchar **language_names;
};

static void
language_names_cache_free (gpointer data)
{
  GLanguageNamesCache *cache = data;
  g_free (cache->languages);
  g_strfreev (cache->language_names);
  g_free (cache);
}

/* read an alias file for the locales */
static void
read_aliases (const gchar *file,
              GHashTable  *alias_table)
{
  FILE *fp;
  char buf[256];

  fp = fopen (file,"r");
  if (!fp)
    return;
  while (fgets (buf, 256, fp))
    {
      char *p, *q;

      g_strstrip (buf);

      /* Line is a comment */
      if ((buf[0] == '#') || (buf[0] == '\0'))
        continue;

      /* Reads first column */
      for (p = buf, q = NULL; *p; p++) {
        if ((*p == '\t') || (*p == ' ') || (*p == ':')) {
          *p = '\0';
          q = p+1;
          while ((*q == '\t') || (*q == ' ')) {
            q++;
          }
          break;
        }
      }
      /* The line only had one column */
      if (!q || *q == '\0')
        continue;

      /* Read second column */
      for (p = q; *p; p++) {
        if ((*p == '\t') || (*p == ' ')) {
          *p = '\0';
          break;
        }
      }

      /* Add to alias table if necessary */
      if (!g_hash_table_lookup (alias_table, buf)) {
        g_hash_table_insert (alias_table, g_strdup (buf), g_strdup (q));
      }
    }
  fclose (fp);
}

static char *
unalias_lang (char *lang)
{
  static GHashTable *alias_table = NULL;
  char *p;
  int i;

  if (g_once_init_enter (&alias_table))
    {
      GHashTable *table = g_hash_table_new (g_str_hash, g_str_equal);
      read_aliases ("/usr/share/locale/locale.alias", table);
      g_once_init_leave (&alias_table, table);
    }

  i = 0;
  while ((p = g_hash_table_lookup (alias_table, lang)) && (strcmp (p, lang) != 0))
    {
      lang = p;
      if (i++ == 30)
        {
          static gboolean said_before = FALSE;
          if (!said_before)
            g_warning ("Too many alias levels for a locale, "
                       "may indicate a loop");
          said_before = TRUE;
          return lang;
        }
    }
  return lang;
}

/* Mask for components of locale spec. The ordering here is from
 * least significant to most significant
 */
enum
{
  COMPONENT_CODESET =   1 << 0,
  COMPONENT_TERRITORY = 1 << 1,
  COMPONENT_MODIFIER =  1 << 2
};

/* Break an X/Open style locale specification into components
 */
static guint
explode_locale (const gchar *locale,
                gchar      **language,
                gchar      **territory,
                gchar      **codeset,
                gchar      **modifier)
{
  const gchar *uscore_pos;
  const gchar *at_pos;
  const gchar *dot_pos;

  guint mask = 0;

  uscore_pos = strchr (locale, '_');
  dot_pos = strchr (uscore_pos ? uscore_pos : locale, '.');
  at_pos = strchr (dot_pos ? dot_pos : (uscore_pos ? uscore_pos : locale), '@');

  if (at_pos)
    {
      mask |= COMPONENT_MODIFIER;
      *modifier = g_strdup (at_pos);
    }
  else
    at_pos = locale + strlen (locale);

  if (dot_pos)
    {
      mask |= COMPONENT_CODESET;
      *codeset = g_strndup (dot_pos, at_pos - dot_pos);
    }
  else
    dot_pos = at_pos;

  if (uscore_pos)
    {
      mask |= COMPONENT_TERRITORY;
      *territory = g_strndup (uscore_pos, dot_pos - uscore_pos);
    }
  else
    uscore_pos = dot_pos;

  *language = g_strndup (locale, uscore_pos - locale);

  return mask;
}

/*
 * Compute all interesting variants for a given locale name -
 * by stripping off different components of the value.
 *
 * For simplicity, we assume that the locale is in
 * X/Open format: language[_territory][.codeset][@modifier]
 *
 * TODO: Extend this to handle the CEN format (see the GNUlibc docs)
 *       as well. We could just copy the code from glibc wholesale
 *       but it is big, ugly, and complicated, so I'm reluctant
 *       to do so when this should handle 99% of the time...
 */
static void
append_locale_variants (GPtrArray *array,
                        const gchar *locale)
{
  gchar *language = NULL;
  gchar *territory = NULL;
  gchar *codeset = NULL;
  gchar *modifier = NULL;

  guint mask;
  guint i, j;

  g_return_if_fail (locale != NULL);

  mask = explode_locale (locale, &language, &territory, &codeset, &modifier);

  /* Iterate through all possible combinations, from least attractive
   * to most attractive.
   */
  for (j = 0; j <= mask; ++j)
    {
      i = mask - j;

      if ((i & ~mask) == 0)
        {
          gchar *val = g_strconcat (language,
                                    (i & COMPONENT_TERRITORY) ? territory : "",
                                    (i & COMPONENT_CODESET) ? codeset : "",
                                    (i & COMPONENT_MODIFIER) ? modifier : "",
                                    NULL);
          g_ptr_array_add (array, val);
        }
    }

  g_free (language);
  if (mask & COMPONENT_CODESET)
    g_free (codeset);
  if (mask & COMPONENT_TERRITORY)
    g_free (territory);
  if (mask & COMPONENT_MODIFIER)
    g_free (modifier);
}

/* The following is (partly) taken from the gettext package.
   Copyright (C) 1995, 1996, 1997, 1998 Free Software Foundation, Inc.  */

static const gchar *
guess_category_value (const gchar *category_name)
{
  const gchar *retval;

  /* The highest priority value is the 'LANGUAGE' environment
     variable.  This is a GNU extension.  */
  retval = g_getenv ("LANGUAGE");
  if ((retval != NULL) && (retval[0] != '\0'))
    return retval;

  /* 'LANGUAGE' is not set.  So we have to proceed with the POSIX
     methods of looking to 'LC_ALL', 'LC_xxx', and 'LANG'.  On some
     systems this can be done by the 'setlocale' function itself.  */

  /* Setting of LC_ALL overwrites all other.  */
  retval = g_getenv ("LC_ALL");
  if ((retval != NULL) && (retval[0] != '\0'))
    return retval;

  /* Next comes the name of the desired category.  */
  retval = g_getenv (category_name);
  if ((retval != NULL) && (retval[0] != '\0'))
    return retval;

  /* Last possibility is the LANG environment variable.  */
  retval = g_getenv ("LANG");
  if ((retval != NULL) && (retval[0] != '\0'))
    return retval;

#ifdef G_PLATFORM_WIN32
  /* g_win32_getlocale() first checks for LC_ALL, LC_MESSAGES and
   * LANG, which we already did above. Oh well. The main point of
   * calling g_win32_getlocale() is to get the thread's locale as used
   * by Windows and the Microsoft C runtime (in the "English_United
   * States" format) translated into the Unixish format.
   */
  {
    char *locale = g_win32_getlocale ();
    retval = g_intern_string (locale);
    g_free (locale);
    return retval;
  }
#endif

  return NULL;
}

const gchar * const *
g_get_language_names_with_category (const gchar *category_name)
{
  static GPrivate cache_private = G_PRIVATE_INIT ((void (*)(gpointer)) g_hash_table_unref);
  GHashTable *cache = g_private_get (&cache_private);
  const gchar *languages;
  GLanguageNamesCache *name_cache;

  g_return_val_if_fail (category_name != NULL, NULL);

  if (!cache)
    {
      cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     g_free, language_names_cache_free);
      g_private_set (&cache_private, cache);
    }

  languages = guess_category_value (category_name);
  if (!languages)
    languages = "C";

  name_cache = (GLanguageNamesCache *) g_hash_table_lookup (cache, category_name);
  if (!(name_cache && name_cache->languages &&
        strcmp (name_cache->languages, languages) == 0))
    {
      GPtrArray *array;
      gchar **alist, **a;

      g_hash_table_remove (cache, category_name);

      array = g_ptr_array_sized_new (8);

      alist = g_strsplit (languages, ":", 0);
      for (a = alist; *a; a++)
        append_locale_variants (array, unalias_lang (*a));
      g_strfreev (alist);
      g_ptr_array_add (array, g_strdup ("C"));
      g_ptr_array_add (array, NULL);

      name_cache = g_new0 (GLanguageNamesCache, 1);
      name_cache->languages = g_strdup (languages);
      name_cache->language_names = (gchar **) g_ptr_array_free (array, FALSE);
      g_hash_table_insert (cache, g_strdup (category_name), name_cache);
    }

  return (const gchar * const *) name_cache->language_names;
}
#endif

#if !GLIB_CHECK_VERSION (2, 62, 0)
/* This is a reimplementation, not a backport: the version in GLib makes
 * use of GPtrArray internals */
void
g_ptr_array_extend (GPtrArray  *array_to_extend,
                    GPtrArray  *array,
                    GCopyFunc   func,
                    gpointer    user_data)
{
  for (gsize i = 0; i < array->len; i++)
    {
      if (func)
        g_ptr_array_add (array_to_extend, func (g_ptr_array_index (array, i), user_data));
      else
        g_ptr_array_add (array_to_extend, g_ptr_array_index (array, i));
    }
}
#endif

#if !GLIB_CHECK_VERSION (2, 68, 0)
/* All this code is backported directly from GLib 2.76.2 */
guint
g_string_replace (GString     *string,
                  const gchar *find,
                  const gchar *replace,
                  guint        limit)
{
  gsize f_len, r_len, pos;
  gchar *cur, *next;
  guint n = 0;

  g_return_val_if_fail (string != NULL, 0);
  g_return_val_if_fail (find != NULL, 0);
  g_return_val_if_fail (replace != NULL, 0);

  f_len = strlen (find);
  r_len = strlen (replace);
  cur = string->str;

  while ((next = strstr (cur, find)) != NULL)
    {
      pos = next - string->str;
      g_string_erase (string, pos, f_len);
      g_string_insert (string, pos, replace);
      cur = string->str + pos + r_len;
      n++;
      /* Only match the empty string once at any given position, to
       * avoid infinite loops */
      if (f_len == 0)
        {
          if (cur[0] == '\0')
            break;
          else
            cur++;
        }
      if (n == limit)
        break;
    }

  return n;
}

#endif /* GLIB_CHECK_VERSION (2, 68, 0) */
