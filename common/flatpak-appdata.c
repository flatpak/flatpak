/*
 * Copyright © 2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "flatpak-appdata-private.h"
#include "flatpak-utils-private.h"
#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <stdio.h>

typedef struct
{
  char *id;
  GHashTable *names;
  GHashTable *comments;
  char       *version;
  char       *license;
  char       *content_rating_type;
  GHashTable *content_rating;  /* (element-type interned-utf8 interned-utf8) */
} Component;

typedef struct
{
  GPtrArray *components;
  GString   *text;
  gboolean   in_text;
  gboolean   in_component;
  gboolean   in_content_rating;
  char      *lang;
  guint64    timestamp;
  const char *id;  /* interned */
} ParserData;

static void
component_free (gpointer data)
{
  Component *component = data;

  g_hash_table_unref (component->names);
  g_hash_table_unref (component->comments);
  g_free (component->id);
  g_free (component->version);
  g_free (component->license);
  g_free (component->content_rating_type);
  g_clear_pointer (&component->content_rating, g_hash_table_unref);

  g_free (component);
}

static Component *
component_new (void)
{
  Component *component = g_new0 (Component, 1);

  component->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  component->comments = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  return component;
}

static void
parser_data_free (ParserData *data)
{
  g_ptr_array_unref (data->components);
  g_string_free (data->text, TRUE);
  g_free (data->lang);

  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ParserData, parser_data_free)

static ParserData *
parser_data_new (void)
{
  ParserData *data = g_new0 (ParserData, 1);

  data->components = g_ptr_array_new_with_free_func (component_free);
  data->text = g_string_new ("");

  return data;
}

static void
start_element (GMarkupParseContext *context,
               const char          *element_name,
               const char         **attribute_names,
               const char         **attribute_values,
               gpointer             user_data,
               GError             **error)
{
  ParserData *data = user_data;

  g_assert (data->text->len == 0);
  g_assert (data->lang == NULL);

  if (g_str_equal (element_name, "component"))
    {
      g_ptr_array_add (data->components, component_new ());
    }
  else if (g_str_equal (element_name, "id"))
    {
      data->in_text = TRUE;
    }
  else if (g_str_equal (element_name, "name") ||
           g_str_equal (element_name, "summary"))
    {
      const char *lang = NULL;

      if (g_markup_collect_attributes (element_name,
                                       attribute_names,
                                       attribute_values,
                                       error,
                                       G_MARKUP_COLLECT_STRING | G_MARKUP_COLLECT_OPTIONAL, "xml:lang", &lang,
                                       G_MARKUP_COLLECT_INVALID))
        {
          if (lang)
            data->lang = g_strdup (lang);
          else
            data->lang = g_strdup ("C");
          data->in_text = TRUE;
        }
    }
  else if (g_str_equal (element_name, "project_license"))
    {
      data->in_text = TRUE;
    }
  else if (g_str_equal (element_name, "release"))
    {
      const char *timestamp;
      const char *date;
      const char *version;
      Component *component = NULL;

      g_assert (data->components->len > 0);

      component = g_ptr_array_index (data->components, data->components->len - 1);

      if (g_markup_collect_attributes (element_name,
                                       attribute_names,
                                       attribute_values,
                                       error,
                                       G_MARKUP_COLLECT_STRING, "version", &version,
                                       G_MARKUP_COLLECT_STRING | G_MARKUP_COLLECT_OPTIONAL, "timestamp", &timestamp,
                                       G_MARKUP_COLLECT_STRING | G_MARKUP_COLLECT_OPTIONAL, "date", &date,
                                       G_MARKUP_COLLECT_STRING | G_MARKUP_COLLECT_OPTIONAL, "date_eol", NULL,
                                       G_MARKUP_COLLECT_STRING | G_MARKUP_COLLECT_OPTIONAL, "urgency", NULL,
                                       G_MARKUP_COLLECT_STRING | G_MARKUP_COLLECT_OPTIONAL, "type", NULL,
                                       G_MARKUP_COLLECT_INVALID))
        {
          guint64 ts = 0;

          if (timestamp)
            ts = g_ascii_strtoull (timestamp, NULL, 10);
          else if (date)
            {
              g_autoptr(GTimeZone) tz = g_time_zone_new_utc ();
              g_autoptr(GDateTime) dt = g_date_time_new_from_iso8601 (date, tz);
              if (!dt)
                {
                  int d, m, y;

                  if (sscanf (date, "%u-%u-%u", &d, &m, &y) == 3)
                    dt = g_date_time_new_utc (d, m, y, 0, 0, 0);
                }
              if (dt)
                ts = g_date_time_to_unix (dt);
            }
          else
            g_warning (_("Ignoring release element without timestamp or date"));

          if (ts > data->timestamp)
            {
              data->timestamp = ts;
              g_free (component->version);
              component->version = g_strdup (version);
            }
        }
    }
  else if (g_str_equal (element_name, "content_rating"))
    {
      /* https://www.freedesktop.org/software/appstream/docs/chap-Metadata.html#tag-content_rating */
      Component *component = NULL;

      g_assert (data->components->len > 0);

      component = g_ptr_array_index (data->components, data->components->len - 1);

      if (component->content_rating == NULL)
        {
          const gchar *type = NULL;

          if (g_markup_collect_attributes (element_name,
                                                 attribute_names,
                                                 attribute_values,
                                                 error,
                                                 G_MARKUP_COLLECT_STRING, "type", &type,
                                                 G_MARKUP_COLLECT_INVALID))
            {
              component->content_rating_type = g_strdup (type);
              component->content_rating = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
              data->in_content_rating = TRUE;
            }
          else
            {
              g_warning (_("Ignoring content rating missing type attribute"));
            }
        }
      else
        {
          g_warning (_("Ignoring duplicate content rating"));
        }
    }
  else if (data->in_content_rating && g_str_equal (element_name, "content_attribute"))
    {
      /* https://www.freedesktop.org/software/appstream/docs/chap-Metadata.html#tag-content_rating */
      Component *component = NULL;
      const gchar *id = NULL;

      g_assert (data->components->len > 0);

      component = g_ptr_array_index (data->components, data->components->len - 1);
      g_assert (component->content_rating != NULL);

      if (g_markup_collect_attributes (element_name,
                                       attribute_names,
                                       attribute_values,
                                       error,
                                       G_MARKUP_COLLECT_STRING, "id", &id,
                                       G_MARKUP_COLLECT_INVALID))
        {
          /* FIXME: We use interned strings here (for keys and, below, for
           * values) as the set of OARS keys and values is small and bounded.
           * In future (once we can depend on GLib 2.58), we could switch to
           * using #GRefString to avoid expanding the interned string hash
           * table. */
          data->id = g_intern_string (id);
          data->in_text = TRUE;
        }
      else
        {
          g_warning (_("Ignoring content attribute missing id attribute"));
        }
    }
}

static void
end_element (GMarkupParseContext *context,
             const char          *element_name,
             gpointer             user_data,
             GError             **error)
{
  ParserData *data = user_data;
  g_autofree char *text = NULL;
  Component *component = NULL;
  const char *parent = NULL;
  const GSList *elements;

  elements = g_markup_parse_context_get_element_stack (context);
  if (elements->next)
    parent = (const char *) elements->next->data;

  g_assert (data->components->len > 0);

  component = g_ptr_array_index (data->components, data->components->len - 1);

  if (data->in_text)
    {
      text = g_strdup (data->text->str);
      g_string_truncate (data->text, 0);
      data->in_text = FALSE;
    }

  /* avoid picking up <id> elements from e.g. <provides> */
  if (g_str_equal (element_name, "id") &&
      g_str_equal (parent, "component"))
    {
      component->id = g_steal_pointer (&text);
    }
  else if (g_str_equal (element_name, "name"))
    {
      g_hash_table_insert (component->names, g_steal_pointer (&data->lang), g_steal_pointer (&text));
    }
  else if (g_str_equal (element_name, "summary"))
    {
      g_hash_table_insert (component->comments, g_steal_pointer (&data->lang), g_steal_pointer (&text));
    }
  else if (g_str_equal (element_name, "project_license"))
    {
      component->license = g_steal_pointer (&text);
    }
  else if (g_str_equal (element_name, "content_rating"))
    {
      data->in_content_rating = FALSE;
    }
  else if (data->in_content_rating && g_str_equal (element_name, "content_attribute"))
    {
      /* https://www.freedesktop.org/software/appstream/docs/chap-Metadata.html#tag-content_rating */
      g_assert (component->content_rating != NULL);
      g_hash_table_insert (component->content_rating, (gpointer) data->id, (gpointer) g_intern_string (text));
    }
}

static void
text (GMarkupParseContext *context,
      const char          *text,
      gsize                text_len,
      gpointer             user_data,
      GError             **error)
{
  ParserData *data = user_data;

  if (data->in_text)
    g_string_append_len (data->text, text, text_len);
}

gboolean
flatpak_parse_appdata (const char  *appdata_xml,
                       const char  *app_id,
                       GHashTable **names,
                       GHashTable **comments,
                       char       **version,
                       char       **license,
                       char       **content_rating_type,
                       GHashTable **content_rating)
{
  g_autoptr(GMarkupParseContext) context = NULL;
  GMarkupParser parser = {
    start_element,
    end_element,
    text,
    NULL,
    NULL
  };
  g_autoptr(ParserData) data = parser_data_new ();
  g_autoptr(GError) error = NULL;
  int i;
  g_autofree char *legacy_id = NULL;

  context = g_markup_parse_context_new (&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, data, NULL);

  if (!g_markup_parse_context_parse (context, appdata_xml, -1, &error))
    {
      g_warning (_("Failed to parse appdata: %s"), error->message);
      return FALSE;
    }

  legacy_id = g_strconcat (app_id, ".desktop", NULL);

  for (i = 0; i < data->components->len; i++)
    {
      Component *component = g_ptr_array_index (data->components, i);

      if (g_str_equal (component->id, app_id) ||
          g_str_equal (component->id, legacy_id))
        {
          *names = g_hash_table_ref (component->names);
          *comments = g_hash_table_ref (component->comments);
          *version = g_steal_pointer (&component->version);
          *license = g_steal_pointer (&component->license);
          if (content_rating_type != NULL)
            *content_rating_type = g_steal_pointer (&component->content_rating_type);
          if (content_rating != NULL)
            *content_rating = g_steal_pointer (&component->content_rating);
          return TRUE;
        }
    }

  g_warning (_("No matching appdata for %s"), app_id);
  return FALSE;
}
