/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 1995-1998 Free Software Foundation, Inc.
 * Copyright © 2014-2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"
#include "flatpak-xml-utils-private.h"

#include "flatpak-run-private.h"
#include "flatpak-utils-private.h"

typedef struct
{
  FlatpakXml *current;
} XmlData;

static FlatpakXml *
flatpak_xml_new (const gchar *element_name)
{
  FlatpakXml *node = g_new0 (FlatpakXml, 1);

  node->element_name = g_strdup (element_name);
  return node;
}

static FlatpakXml *
flatpak_xml_new_text (const gchar *text)
{
  FlatpakXml *node = g_new0 (FlatpakXml, 1);

  node->text = g_strdup (text);
  return node;
}

void
flatpak_xml_add (FlatpakXml *parent, FlatpakXml *node)
{
  node->parent = parent;

  if (parent->first_child == NULL)
    parent->first_child = node;
  else
    parent->last_child->next_sibling = node;
  parent->last_child = node;
}

static void
xml_start_element (GMarkupParseContext *context,
                   const gchar         *element_name,
                   const gchar        **attribute_names,
                   const gchar        **attribute_values,
                   gpointer             user_data,
                   GError             **error)
{
  XmlData *data = user_data;
  FlatpakXml *node;

  node = flatpak_xml_new (element_name);
  node->attribute_names = g_strdupv ((char **) attribute_names);
  node->attribute_values = g_strdupv ((char **) attribute_values);

  flatpak_xml_add (data->current, node);
  data->current = node;
}

static void
xml_end_element (GMarkupParseContext *context,
                 const gchar         *element_name,
                 gpointer             user_data,
                 GError             **error)
{
  XmlData *data = user_data;

  data->current = data->current->parent;
}

static void
xml_text (GMarkupParseContext *context,
          const gchar         *text,
          gsize                text_len,
          gpointer             user_data,
          GError             **error)
{
  XmlData *data = user_data;
  FlatpakXml *node;

  node = flatpak_xml_new (NULL);
  node->text = g_strndup (text, text_len);
  flatpak_xml_add (data->current, node);
}

static void
xml_passthrough (GMarkupParseContext *context,
                 const gchar         *passthrough_text,
                 gsize                text_len,
                 gpointer             user_data,
                 GError             **error)
{
}

static GMarkupParser xml_parser = {
  xml_start_element,
  xml_end_element,
  xml_text,
  xml_passthrough,
  NULL
};

void
flatpak_xml_free (FlatpakXml *node)
{
  FlatpakXml *child;

  if (node == NULL)
    return;

  child = node->first_child;
  while (child != NULL)
    {
      FlatpakXml *next = child->next_sibling;
      flatpak_xml_free (child);
      child = next;
    }

  g_free (node->element_name);
  g_free (node->text);
  g_strfreev (node->attribute_names);
  g_strfreev (node->attribute_values);
  g_free (node);
}


static void
flatpak_xml_to_string (FlatpakXml *node, GString *res)
{
  int i;
  FlatpakXml *child;

  if (node->parent == NULL)
    g_string_append (res, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");

  if (node->element_name)
    {
      if (node->parent != NULL)
        {
          g_string_append (res, "<");
          g_string_append (res, node->element_name);
          if (node->attribute_names)
            {
              for (i = 0; node->attribute_names[i] != NULL; i++)
                {
                  g_string_append_printf (res, " %s=\"%s\"",
                                          node->attribute_names[i],
                                          node->attribute_values[i]);
                }
            }
          if (node->first_child == NULL)
            g_string_append (res, "/>");
          else
            g_string_append (res, ">");
        }

      child = node->first_child;
      while (child != NULL)
        {
          flatpak_xml_to_string (child, res);
          child = child->next_sibling;
        }
      if (node->parent != NULL)
        {
          if (node->first_child != NULL)
            g_string_append_printf (res, "</%s>", node->element_name);
        }
    }
  else if (node->text)
    {
      g_autofree char *escaped = g_markup_escape_text (node->text, -1);
      g_string_append (res, escaped);
    }
}

FlatpakXml *
flatpak_xml_unlink (FlatpakXml *node,
                    FlatpakXml *prev_sibling)
{
  FlatpakXml *parent = node->parent;

  if (parent == NULL)
    return node;

  if (parent->first_child == node)
    parent->first_child = node->next_sibling;

  if (parent->last_child == node)
    parent->last_child = prev_sibling;

  if (prev_sibling)
    prev_sibling->next_sibling = node->next_sibling;

  node->parent = NULL;
  node->next_sibling = NULL;

  return node;
}

FlatpakXml *
flatpak_xml_find (FlatpakXml  *node,
                  const char  *type,
                  FlatpakXml **prev_child_out)
{
  FlatpakXml *child = NULL;
  FlatpakXml *prev_child = NULL;

  child = node->first_child;
  prev_child = NULL;
  while (child != NULL)
    {
      FlatpakXml *next = child->next_sibling;

      if (g_strcmp0 (child->element_name, type) == 0)
        {
          if (prev_child_out)
            *prev_child_out = prev_child;
          return child;
        }

      prev_child = child;
      child = next;
    }

  return NULL;
}


FlatpakXml *
flatpak_xml_parse (GInputStream *in,
                   gboolean      compressed,
                   GCancellable *cancellable,
                   GError      **error)
{
  g_autoptr(GInputStream) real_in = NULL;
  g_autoptr(FlatpakXml) xml_root = NULL;
  XmlData data = { 0 };
  char buffer[32 * 1024];
  gssize len;
  g_autoptr(GMarkupParseContext) ctx = NULL;

  if (compressed)
    {
      g_autoptr(GZlibDecompressor) decompressor = NULL;
      decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      real_in = g_converter_input_stream_new (in, G_CONVERTER (decompressor));
    }
  else
    {
      real_in = g_object_ref (in);
    }

  xml_root = flatpak_xml_new ("root");
  data.current = xml_root;

  ctx = g_markup_parse_context_new (&xml_parser,
                                    G_MARKUP_PREFIX_ERROR_POSITION,
                                    &data,
                                    NULL);

  while ((len = g_input_stream_read (real_in, buffer, sizeof (buffer),
                                     cancellable, error)) > 0)
    {
      if (!g_markup_parse_context_parse (ctx, buffer, len, error))
        return NULL;
    }

  if (len < 0)
    return NULL;

  return g_steal_pointer (&xml_root);
}

FlatpakXml *
flatpak_appstream_xml_new (void)
{
  FlatpakXml *appstream_root = NULL;
  FlatpakXml *appstream_components;

  appstream_root = flatpak_xml_new ("root");
  appstream_components = flatpak_xml_new ("components");
  flatpak_xml_add (appstream_root, appstream_components);
  flatpak_xml_add (appstream_components, flatpak_xml_new_text ("\n  "));

  appstream_components->attribute_names = g_new0 (char *, 3);
  appstream_components->attribute_values = g_new0 (char *, 3);
  appstream_components->attribute_names[0] = g_strdup ("version");
  appstream_components->attribute_values[0] = g_strdup ("0.8");
  appstream_components->attribute_names[1] = g_strdup ("origin");
  appstream_components->attribute_values[1] = g_strdup ("flatpak");

  return appstream_root;
}

gboolean
flatpak_appstream_xml_root_to_data (FlatpakXml *appstream_root,
                                    GBytes    **uncompressed,
                                    GBytes    **compressed,
                                    GError    **error)
{
  g_autoptr(GString) xml = NULL;
  g_autoptr(GZlibCompressor) compressor = NULL;
  g_autoptr(GOutputStream) out2 = NULL;
  g_autoptr(GOutputStream) out = NULL;

  flatpak_xml_add (appstream_root->first_child, flatpak_xml_new_text ("\n"));

  xml = g_string_new ("");
  flatpak_xml_to_string (appstream_root, xml);

  if (compressed)
    {
      compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
      out = g_memory_output_stream_new_resizable ();
      out2 = g_converter_output_stream_new (out, G_CONVERTER (compressor));
      if (!g_output_stream_write_all (out2, xml->str, xml->len,
                                      NULL, NULL, error))
        return FALSE;
      if (!g_output_stream_close (out2, NULL, error))
        return FALSE;
    }

  if (uncompressed)
    *uncompressed = g_string_free_to_bytes (g_steal_pointer (&xml));

  if (compressed)
    *compressed = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));

  return TRUE;
}

void
flatpak_appstream_xml_filter (FlatpakXml *appstream,
                              GRegex *allow_refs,
                              GRegex *deny_refs)
{
  FlatpakXml *components;
  FlatpakXml *component;
  FlatpakXml *prev_component, *old;

  for (components = appstream->first_child;
       components != NULL;
       components = components->next_sibling)
    {
      if (g_strcmp0 (components->element_name, "components") != 0)
        continue;


      prev_component = NULL;
      component = components->first_child;
      while (component != NULL)
        {
          FlatpakXml *bundle;
          gboolean allow = FALSE;

          if (g_strcmp0 (component->element_name, "component") == 0)
            {
              bundle = flatpak_xml_find (component, "bundle", NULL);
              if (bundle && bundle->first_child && bundle->first_child->text)
                allow = flatpak_filters_allow_ref (allow_refs, deny_refs, bundle->first_child->text);
            }

          if (allow)
            {
              prev_component = component;
              component = component->next_sibling;
            }
          else
            {
              old = component;

              /* prev_component is same as before */
              component = component->next_sibling;

              flatpak_xml_unlink (old, prev_component);
              flatpak_xml_free (old);
            }
        }
    }
}

static gboolean
validate_component (FlatpakXml *component,
                    const char *ref,
                    const char *id,
                    char      **tags,
                    const char *runtime,
                    const char *sdk)
{
  FlatpakXml *bundle, *text, *prev, *id_node, *id_text_node, *metadata, *value;
  g_autofree char *id_text = NULL;
  int i;

  if (g_strcmp0 (component->element_name, "component") != 0)
    return FALSE;

  id_node = flatpak_xml_find (component, "id", NULL);
  if (id_node == NULL)
    return FALSE;

  id_text_node = flatpak_xml_find (id_node, NULL, NULL);
  if (id_text_node == NULL || id_text_node->text == NULL)
    return FALSE;

  id_text = g_strstrip (g_strdup (id_text_node->text));

  /* Drop .desktop file suffix (unless the actual app id ends with .desktop) */
  if (g_str_has_suffix (id_text, ".desktop") &&
      !g_str_has_suffix (id, ".desktop"))
    id_text[strlen (id_text) - strlen (".desktop")] = 0;

  if (!g_str_has_prefix (id_text, id))
    {
      g_warning ("Invalid id %s", id_text);
      return FALSE;
    }

  while ((bundle = flatpak_xml_find (component, "bundle", &prev)) != NULL)
    flatpak_xml_free (flatpak_xml_unlink (bundle, prev));

  bundle = flatpak_xml_new ("bundle");
  bundle->attribute_names = g_new0 (char *, 2 * 4);
  bundle->attribute_values = g_new0 (char *, 2 * 4);
  bundle->attribute_names[0] = g_strdup ("type");
  bundle->attribute_values[0] = g_strdup ("flatpak");

  i = 1;
  if (runtime && !g_str_has_prefix (runtime, "runtime/"))
    {
      bundle->attribute_names[i] = g_strdup ("runtime");
      bundle->attribute_values[i] = g_strdup (runtime);
      i++;
    }

  if (sdk)
    {
      bundle->attribute_names[i] = g_strdup ("sdk");
      bundle->attribute_values[i] = g_strdup (sdk);
      i++;
    }

  text = flatpak_xml_new (NULL);
  text->text = g_strdup (ref);
  flatpak_xml_add (bundle, text);

  flatpak_xml_add (component, flatpak_xml_new_text ("  "));
  flatpak_xml_add (component, bundle);
  flatpak_xml_add (component, flatpak_xml_new_text ("\n  "));

  if (tags != NULL && tags[0] != NULL)
    {
      metadata = flatpak_xml_find (component, "metadata", NULL);
      if (metadata == NULL)
        {
          metadata = flatpak_xml_new ("metadata");
          metadata->attribute_names = g_new0 (char *, 1);
          metadata->attribute_values = g_new0 (char *, 1);

          flatpak_xml_add (component, flatpak_xml_new_text ("  "));
          flatpak_xml_add (component, metadata);
          flatpak_xml_add (component, flatpak_xml_new_text ("\n  "));
        }

      value = flatpak_xml_new ("value");
      value->attribute_names = g_new0 (char *, 2);
      value->attribute_values = g_new0 (char *, 2);
      value->attribute_names[0] = g_strdup ("key");
      value->attribute_values[0] = g_strdup ("X-Flatpak-Tags");
      flatpak_xml_add (metadata, flatpak_xml_new_text ("\n       "));
      flatpak_xml_add (metadata, value);
      flatpak_xml_add (metadata, flatpak_xml_new_text ("\n    "));

      text = flatpak_xml_new (NULL);
      text->text = g_strjoinv (",", tags);
      flatpak_xml_add (value, text);
    }

  return TRUE;
}

gboolean
flatpak_appstream_xml_migrate (FlatpakXml *source,
                               FlatpakXml *dest,
                               const char *ref,
                               const char *id,
                               GKeyFile   *metadata)
{
  FlatpakXml *source_components;
  FlatpakXml *dest_components;
  FlatpakXml *component;
  FlatpakXml *prev_component;
  gboolean migrated = FALSE;
  g_auto(GStrv) tags = NULL;
  g_autofree const char *runtime = NULL;
  g_autofree const char *sdk = NULL;
  const char *group;

  if (source->first_child == NULL ||
      source->first_child->next_sibling != NULL ||
      g_strcmp0 (source->first_child->element_name, "components") != 0)
    return FALSE;

  if (g_str_has_prefix (ref, "app/"))
    group = FLATPAK_METADATA_GROUP_APPLICATION;
  else
    group = FLATPAK_METADATA_GROUP_RUNTIME;

  tags = g_key_file_get_string_list (metadata, group, FLATPAK_METADATA_KEY_TAGS,
                                     NULL, NULL);
  runtime = g_key_file_get_string (metadata, group,
                                   FLATPAK_METADATA_KEY_RUNTIME, NULL);
  sdk = g_key_file_get_string (metadata, group, FLATPAK_METADATA_KEY_SDK, NULL);

  source_components = source->first_child;
  dest_components = dest->first_child;

  component = source_components->first_child;
  prev_component = NULL;
  while (component != NULL)
    {
      FlatpakXml *next = component->next_sibling;

      if (validate_component (component, ref, id, tags, runtime, sdk))
        {
          flatpak_xml_add (dest_components,
                           flatpak_xml_unlink (component, prev_component));
          migrated = TRUE;
        }
      else
        {
          prev_component = component;
        }

      component = next;
    }

  return migrated;
}
