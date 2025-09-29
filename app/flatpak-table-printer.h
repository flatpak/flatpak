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
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#ifndef __FLATPAK_TABLE_PRINTER_H__
#define __FLATPAK_TABLE_PRINTER_H__

#include <gio/gio.h>

#include "flatpak-builtins-utils.h"

typedef struct FlatpakTablePrinter FlatpakTablePrinter;

FlatpakTablePrinter *flatpak_table_printer_new (void);
void                flatpak_table_printer_free (FlatpakTablePrinter *printer);
void                flatpak_table_printer_set_column_title (FlatpakTablePrinter *printer,
                                                            int                  column,
                                                            const char          *title);
void                flatpak_table_printer_set_columns (FlatpakTablePrinter *printer,
                                                       Column              *columns,
                                                       gboolean             defaults);
void                flatpak_table_printer_add_column (FlatpakTablePrinter *printer,
                                                      const char          *text);
void                flatpak_table_printer_take_column (FlatpakTablePrinter *printer,
                                                       char                *text);
void                flatpak_table_printer_add_aligned_column (FlatpakTablePrinter *printer,
                                                              const char          *text,
                                                              int                  align);
void                flatpak_table_printer_add_decimal_column (FlatpakTablePrinter *printer,
                                                              const char          *text);
void                flatpak_table_printer_add_column_len (FlatpakTablePrinter *printer,
                                                          const char          *text,
                                                          gsize                len);
void                flatpak_table_printer_add_span (FlatpakTablePrinter *printer,
                                                    const char          *text);
void                flatpak_table_printer_append_with_comma (FlatpakTablePrinter *printer,
                                                             const char          *text);
void                flatpak_table_printer_append_with_comma_printf (FlatpakTablePrinter *printer,
                                                                    const char          *format,
                                                                    ...) G_GNUC_PRINTF (2, 3);
void                flatpak_table_printer_set_key (FlatpakTablePrinter *printer,
                                                   const char          *key);
void                flatpak_table_printer_finish_row (FlatpakTablePrinter *printer);
void                flatpak_table_printer_sort (FlatpakTablePrinter *printer,
                                                GCompareFunc         cmp);
int                 flatpak_table_printer_lookup_row (FlatpakTablePrinter *printer, const char *key);
void                flatpak_table_printer_print (FlatpakTablePrinter *printer);
void                flatpak_table_printer_print_json (FlatpakTablePrinter *printer);
void                flatpak_table_printer_print_full (FlatpakTablePrinter *printer,
                                                      int                  skip,
                                                      int                  columns,
                                                      int                 *table_height,
                                                      int                 *table_width);
int                 flatpak_table_printer_get_current_row (FlatpakTablePrinter *printer);
void                flatpak_table_printer_set_cell (FlatpakTablePrinter *printer,
                                                    int                  row,
                                                    int                  col,
                                                    const char          *cell);
void                flatpak_table_printer_append_cell (FlatpakTablePrinter *printer,
                                                       int                  row,
                                                       int                  col,
                                                       const char          *cell);
void                flatpak_table_printer_append_cell_with_comma (FlatpakTablePrinter *printer,
                                                                  int                  row,
                                                                  int                  col,
                                                                  const char          *cell);
void                flatpak_table_printer_append_cell_with_comma_unique (FlatpakTablePrinter *printer,
                                                                         int                  row,
                                                                         int                  col,
                                                                         const char          *cell);
void                flatpak_table_printer_set_decimal_cell (FlatpakTablePrinter *printer,
                                                            int                  row,
                                                            int                  col,
                                                            const char          *cell);
void               flatpak_table_printer_set_column_expand (FlatpakTablePrinter *printer,
                                                            int                  col,
                                                            gboolean             expand);
void               flatpak_table_printer_set_column_ellipsize (FlatpakTablePrinter *printer,
                                                               int                  col,
                                                               FlatpakEllipsizeMode mode);
void               flatpak_table_printer_set_column_skip_unique (FlatpakTablePrinter *printer,
                                                                 int                  column,
                                                                 gboolean             skip_unique);
void               flatpak_table_printer_set_column_skip_unique_string (FlatpakTablePrinter *printer,
                                                                        int                  column,
                                                                        const char          *str);


G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakTablePrinter, flatpak_table_printer_free)

#endif /* __FLATPAK_TABLE_PRINTER_H__ */
