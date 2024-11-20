/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2024 GNOME Foundation, Inc.
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
 *       Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *       Hubert Figuière <hub@figuiere.net>
 */

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include "libglnx.h"

#include "flatpak-usb-private.h"

void
flatpak_usb_rule_free (FlatpakUsbRule *usb_rule)
{
  g_clear_pointer (&usb_rule, g_free);
}

void
flatpak_usb_query_free (FlatpakUsbQuery *usb_query)
{
  if (!usb_query)
    return;

  g_clear_pointer (&usb_query->rules, g_ptr_array_unref);
  g_clear_pointer (&usb_query, g_free);
}

static gboolean
validate_hex_uint16 (const char *value,
                     size_t      expected_length,
                     uint16_t   *out_value)
{
  size_t len;
  char *end;
  long n;

  g_return_val_if_fail (value != NULL, FALSE);
  g_return_val_if_fail (expected_length > 0 && expected_length <= 4, FALSE);

  len = strlen (value);
  if (len != expected_length)
    return FALSE;

  n = strtol (value, &end, 16);

  if (end - value != len)
    return FALSE;

  if (n < 0 || n > UINT16_MAX)
    return FALSE;

  if (out_value)
    *out_value = n;

  return TRUE;
}

static gboolean
parse_all_usb_rule (FlatpakUsbRule  *dest,
                    GStrv            data,
                    GError         **error)
{
  if (g_strv_length (data) != 1)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB device query 'all' must not have data"));
      return FALSE;
    }

  dest->rule_type = FLATPAK_USB_RULE_TYPE_ALL;
  return TRUE;
}

static gboolean
parse_cls_usb_rule (FlatpakUsbRule  *dest,
                    GStrv            data,
                    GError         **error)
{
  const char *subclass;
  const char *class;

  if (g_strv_length (data) < 3)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB query rule 'cls' must be in the form CLASS:SUBCLASS or CLASS:*"));
      return FALSE;
    }

  class = data[1];
  subclass = data[2];

  if (!validate_hex_uint16 (class, 2, &dest->d.device_class.class))
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, _("Invalid USB class"));
      return FALSE;
    }

  if (g_strcmp0 (subclass, "*") == 0)
    {
      dest->d.device_class.type = FLATPAK_USB_RULE_CLASS_TYPE_CLASS_ONLY;
    }
  else if (validate_hex_uint16 (subclass, 2, &dest->d.device_class.subclass))
    {
      dest->d.device_class.type = FLATPAK_USB_RULE_CLASS_TYPE_CLASS_SUBCLASS;
    }
  else
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, _("Invalid USB subclass"));
      return FALSE;
    }

  dest->rule_type = FLATPAK_USB_RULE_TYPE_CLASS;
  return TRUE;
}

static gboolean
parse_dev_usb_rule (FlatpakUsbRule  *dest,
                    GStrv            data,
                    GError         **error)
{
  if (g_strv_length (data) != 2)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB query rule 'dev' must have a valid 4-digit hexadecimal product id"));
      return FALSE;
    }

  if (!validate_hex_uint16 (data[1], 4, &dest->d.product.id))
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB query rule 'dev' must have a valid 4-digit hexadecimal product id"));
      return FALSE;
    }

  dest->rule_type = FLATPAK_USB_RULE_TYPE_DEVICE;
  return TRUE;
}

static gboolean
parse_vnd_usb_rule (FlatpakUsbRule  *dest,
                    GStrv            data,
                    GError         **error)
{
  if (g_strv_length (data) != 2)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB query rule 'vnd' must have a valid 4-digit hexadecimal vendor id"));
      return FALSE;
    }

  if (!validate_hex_uint16 (data[1], 4, &dest->d.product.id))
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB query rule 'vnd' must have a valid 4-digit hexadecimal vendor id"));
      return FALSE;
    }

  dest->rule_type = FLATPAK_USB_RULE_TYPE_VENDOR;
  return TRUE;
}

static const struct {
  const char *name;
  gboolean (*parse) (FlatpakUsbRule  *dest,
                     GStrv            data,
                     GError         **error);
} rule_parsers[] = {
  { "all", parse_all_usb_rule },
  { "cls", parse_cls_usb_rule },
  { "dev", parse_dev_usb_rule },
  { "vnd", parse_vnd_usb_rule },
};

gboolean
flatpak_usb_parse_usb_rule (const char      *data,
			FlatpakUsbRule **out_usb_rule,
			GError         **error)
{
  g_autoptr(FlatpakUsbRule) usb_rule = NULL;
  g_auto(GStrv) split = NULL;
  gboolean parsed = FALSE;

  split = g_strsplit (data, ":", 0);

  if (!split || g_strv_length (split) > 3)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB device queries must be in the form TYPE:DATA"));
      return FALSE;
    }

  usb_rule = g_new0 (FlatpakUsbRule, 1);

  for (size_t i = 0; i < G_N_ELEMENTS (rule_parsers); i++)
    {
      if (g_strcmp0 (rule_parsers[i].name, split[0]))
	continue;

      if (!rule_parsers[i].parse (usb_rule, split, error))
	return FALSE;

      parsed = TRUE;
    }

  if (!parsed)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("Unknown USB query rule %s"), split[0]);
      return FALSE;
    }

  if (out_usb_rule)
    *out_usb_rule = g_steal_pointer (&usb_rule);

  return TRUE;
}

gboolean
flatpak_usb_parse_usb (const char       *data,
		   FlatpakUsbQuery **out_usb_query,
		   GError          **error)
{
  g_autoptr(FlatpakUsbQuery) usb_query = NULL;
  g_autoptr(GHashTable) rule_types = NULL;
  g_auto(GStrv) split = NULL;

  split = g_strsplit (data, "+", 0);

  if (!*split)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, _("Empty USB query"));
      return FALSE;
    }

  usb_query = flatpak_usb_query_new ();

  for (size_t i = 0; split[i] != NULL; i++)
    {
      g_autoptr(FlatpakUsbRule) usb_rule = NULL;
      const char *rule = split[i];

      if (!flatpak_usb_parse_usb_rule (rule, &usb_rule, error))
        return FALSE;

      g_ptr_array_add (usb_query->rules, g_steal_pointer (&usb_rule));
    }

  g_assert (usb_query->rules->len > 0);

  rule_types = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (size_t i = 0; i < usb_query->rules->len; i++)
    {
      FlatpakUsbRule *usb_rule = g_ptr_array_index (usb_query->rules, i);
      if (!g_hash_table_add (rule_types, GINT_TO_POINTER (usb_rule->rule_type)))
        {
          g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                       _("Multiple USB query rules of the same type is not supported"));
          return FALSE;
        }
    }

  if (g_hash_table_contains (rule_types, GINT_TO_POINTER (FLATPAK_USB_RULE_TYPE_ALL)) &&
      g_hash_table_size (rule_types) > 1)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("'all' must not contain extra query rules"));
      return FALSE;
    }

  if (g_hash_table_contains (rule_types, GINT_TO_POINTER (FLATPAK_USB_RULE_TYPE_DEVICE)) &&
      !g_hash_table_contains (rule_types, GINT_TO_POINTER (FLATPAK_USB_RULE_TYPE_VENDOR)))
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   _("USB queries with 'dev' must also specify vendors"));
      return FALSE;
    }

  if (out_usb_query)
    *out_usb_query = g_steal_pointer (&usb_query);

  return TRUE;
}

gboolean
flatpak_usb_parse_usb_list (const char     *buffer,
                            GHashTable     *enumerable,
                            GHashTable     *hidden,
                            GError        **error)
{
  char *aux = NULL;
  g_autoptr(FlatpakUsbQuery) usb_query = NULL;
  g_autoptr(GInputStream) stream = NULL;
  g_autoptr(GDataInputStream) buffered = NULL;
  g_autoptr(GError) local_error = NULL;

  stream = g_memory_input_stream_new_from_data (buffer, -1, NULL);
  if (!stream)
    return FALSE;

  buffered = g_data_input_stream_new (G_INPUT_STREAM (stream));

  while ((aux = g_data_input_stream_read_line (buffered, NULL, NULL, &local_error)))
    {
      g_autofree char *line = g_steal_pointer (&aux);
      g_auto(GStrv) split = NULL;
      gboolean blocking = FALSE;

      if (line[0] == '#')
        continue;

      split = g_strsplit (line, ";", 0);
      if (!split || !*split)
        continue;

      for (size_t i = 0; split[i] != NULL; i++)
        {
          const char *item = split[i];

          blocking = item[0] == '!';
          if (blocking)
            item++;

          if (flatpak_usb_parse_usb (item, &usb_query, NULL))
            {
              GString *string = g_string_new (NULL);
              flatpak_usb_query_print (usb_query, string);

              if (blocking)
	        {
                  g_hash_table_insert (hidden,
                                       g_string_free (string, FALSE),
                                       g_steal_pointer (&usb_query));
                }
              else
                {
                  g_hash_table_insert (enumerable,
                                       g_string_free (string, FALSE),
                                       g_steal_pointer (&usb_query));
                }
            }
        }
    }

  g_input_stream_close (G_INPUT_STREAM (buffered), NULL, error);
  g_input_stream_close (G_INPUT_STREAM (stream), NULL, error);

  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

void
flatpak_usb_rule_print (FlatpakUsbRule *usb_rule,
                        GString        *string)
{
  g_return_if_fail (usb_rule != NULL);
  g_assert (string != NULL);

  switch (usb_rule->rule_type)
    {
    case FLATPAK_USB_RULE_TYPE_ALL:
      g_string_append (string, "all");
      break;

    case FLATPAK_USB_RULE_TYPE_CLASS:
      g_string_append (string, "cls:");
      if (usb_rule->d.device_class.type == FLATPAK_USB_RULE_CLASS_TYPE_CLASS_ONLY)
        g_string_append_printf (string, "%02x:*", usb_rule->d.device_class.class);
      else if (usb_rule->d.device_class.type == FLATPAK_USB_RULE_CLASS_TYPE_CLASS_SUBCLASS)
        g_string_append_printf (string, "%02x:%02x", usb_rule->d.device_class.class, usb_rule->d.device_class.subclass);
      else
        g_assert_not_reached ();
      break;

    case FLATPAK_USB_RULE_TYPE_DEVICE:
      g_string_append_printf (string, "dev:%04x", usb_rule->d.product.id);
      break;

    case FLATPAK_USB_RULE_TYPE_VENDOR:
      g_string_append_printf (string, "vnd:%04x", usb_rule->d.vendor.id);
      break;

    default:
      g_assert_not_reached ();
    }
}

FlatpakUsbQuery *
flatpak_usb_query_new (void)
{
  g_autoptr(FlatpakUsbQuery) usb_query = NULL;

  usb_query = g_new0 (FlatpakUsbQuery, 1);
  usb_query->rules = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_usb_rule_free);

  return g_steal_pointer (&usb_query);
}

FlatpakUsbQuery *
flatpak_usb_query_copy (const FlatpakUsbQuery *query)
{
  FlatpakUsbQuery *copy = flatpak_usb_query_new ();

  for (size_t i = 0; i < query->rules->len; i++)
    {
      FlatpakUsbRule *usb_rule = g_ptr_array_index (query->rules, i);
      g_ptr_array_add (copy->rules, g_memdup2 (usb_rule, sizeof (FlatpakUsbRule)));
    }
  return copy;
}

void
flatpak_usb_query_print (const FlatpakUsbQuery *usb_query,
                         GString               *string)
{
  g_assert (usb_query != NULL && usb_query->rules != NULL);
  g_assert (string != NULL);

  for (size_t i = 0; i < usb_query->rules->len; i++)
    {
      FlatpakUsbRule *usb_rule = g_ptr_array_index (usb_query->rules, i);

      if (i > 0)
        g_string_append_c (string, '+');

      flatpak_usb_rule_print (usb_rule, string);
    }
}
