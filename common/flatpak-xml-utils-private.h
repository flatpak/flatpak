/*
 * Copyright © 2014 Red Hat, Inc
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
 */

#pragma once

#include "libglnx.h"

typedef struct FlatpakXml FlatpakXml;

struct FlatpakXml
{
  gchar      *element_name; /* NULL == text */
  char      **attribute_names;
  char      **attribute_values;
  char       *text;
  FlatpakXml *parent;
  FlatpakXml *first_child;
  FlatpakXml *last_child;
  FlatpakXml *next_sibling;
};

void       flatpak_xml_add (FlatpakXml *parent,
                            FlatpakXml *node);
void       flatpak_xml_free (FlatpakXml *node);
FlatpakXml *flatpak_xml_parse (GInputStream * in,
                               gboolean compressed,
                               GCancellable *cancellable,
                               GError      **error);
FlatpakXml *flatpak_xml_unlink (FlatpakXml *node,
                                FlatpakXml *prev_sibling);
FlatpakXml *flatpak_xml_find (FlatpakXml  *node,
                              const char  *type,
                              FlatpakXml **prev_child_out);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakXml, flatpak_xml_free);

FlatpakXml *flatpak_appstream_xml_new (void);
gboolean   flatpak_appstream_xml_migrate (FlatpakXml *source,
                                          FlatpakXml *dest,
                                          const char *ref,
                                          const char *id,
                                          GKeyFile   *metadata);
gboolean flatpak_appstream_xml_root_to_data (FlatpakXml *appstream_root,
                                             GBytes    **uncompressed,
                                             GBytes    **compressed,
                                             GError    **error);
void flatpak_appstream_xml_filter (FlatpakXml *appstream,
                                   GRegex *allow_refs,
                                   GRegex *deny_refs);
