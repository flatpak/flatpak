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

#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>


typedef struct
{
  char    *text;
  int      align;
  gboolean span;
} Cell;

static void
free_cell (gpointer data)
{
  Cell *cell = data;

  g_free (cell->text);
  g_free (cell);
}

struct FlatpakTablePrinter
{
  GPtrArray *titles;
  GPtrArray *rows;
  GPtrArray *current;
  int        n_columns;
};

FlatpakTablePrinter *
flatpak_table_printer_new (void)
{
  FlatpakTablePrinter *printer = g_new0 (FlatpakTablePrinter, 1);

  printer->titles = g_ptr_array_new_with_free_func (g_free);
  printer->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  printer->current = g_ptr_array_new_with_free_func (free_cell);

  return printer;
}

void
flatpak_table_printer_free (FlatpakTablePrinter *printer)
{
  g_ptr_array_free (printer->titles, TRUE);
  g_ptr_array_free (printer->rows, TRUE);
  g_ptr_array_free (printer->current, TRUE);
  g_free (printer);
}

void
flatpak_table_printer_set_column_title (FlatpakTablePrinter *printer,
                                        int                  column,
                                        const char          *text)
{
  g_ptr_array_insert (printer->titles, column, g_strdup (text));
}

void
flatpak_table_printer_add_aligned_column (FlatpakTablePrinter *printer,
                                          const char          *text,
                                          int                  align)
{
  Cell *cell = g_new0 (Cell, 1);

  cell->text = text ? g_strdup (text) : g_strdup ("");
  cell->align = align;
  g_ptr_array_add (printer->current, cell);
}

void
flatpak_table_printer_add_span (FlatpakTablePrinter *printer,
                                const char          *text)
{
  Cell *cell = g_new0 (Cell, 1);

  cell->text = text ? g_strdup (text) : g_strdup ("");
  cell->align = -1;
  cell->span = TRUE;
  g_ptr_array_add (printer->current, cell);
}

static const char *
find_decimal_point (const char *text)
{
  struct lconv *locale_data;

  locale_data = localeconv ();
  return strstr (text, locale_data->decimal_point);
}

void
flatpak_table_printer_add_decimal_column (FlatpakTablePrinter *printer,
                                          const char          *text)
{
  const char *decimal;
  int align = -1;

  decimal = find_decimal_point (text);
  if (decimal)
    align = decimal - text;

  flatpak_table_printer_add_aligned_column (printer, text, align);
}

void
flatpak_table_printer_add_column (FlatpakTablePrinter *printer,
                                  const char          *text)
{
  flatpak_table_printer_add_aligned_column (printer, text, -1);
}

void
flatpak_table_printer_add_column_len (FlatpakTablePrinter *printer,
                                      const char          *text,
                                      gsize                len)
{
  Cell *cell = g_new0 (Cell, 1);

  cell->text = text ? g_strndup (text, len) : g_strdup ("");
  cell->align = -1;
  g_ptr_array_add (printer->current, cell);
}

void
flatpak_table_printer_append_with_comma (FlatpakTablePrinter *printer,
                                         const char          *text)
{
  Cell *cell;
  char *new;

  g_assert (printer->current->len > 0);

  cell = g_ptr_array_index (printer->current, printer->current->len - 1);

  if (cell->text[0] != 0)
    new = g_strconcat (cell->text, ",", text, NULL);
  else
    new = g_strdup (text);

  g_free (cell->text);
  cell->text = new;
}

void
flatpak_table_printer_append_with_comma_printf (FlatpakTablePrinter *printer,
                                                const char          *format,
                                                ...)
{
  va_list var_args;
  g_autofree char *s = NULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  va_start (var_args, format);
  s = g_strdup_vprintf (format, var_args);
  va_end (var_args);
#pragma GCC diagnostic pop

  flatpak_table_printer_append_with_comma (printer, s);
}

void
flatpak_table_printer_finish_row (FlatpakTablePrinter *printer)
{
  if (printer->current->len == 0)
    return; /* Ignore empty rows */

  printer->n_columns = MAX (printer->n_columns, printer->current->len);
  g_ptr_array_add (printer->rows, printer->current);
  printer->current = g_ptr_array_new_with_free_func (free_cell);
}

void
flatpak_table_printer_print (FlatpakTablePrinter *printer)
{
  g_autofree int *widths = NULL;
  g_autofree int *lwidths = NULL;
  g_autofree int *rwidths = NULL;
  int i, j;

  if (printer->current->len != 0)
    flatpak_table_printer_finish_row (printer);

  widths = g_new0 (int, printer->n_columns);
  lwidths = g_new0 (int, printer->n_columns);
  rwidths = g_new0 (int, printer->n_columns);

  for (i = 0; i < printer->titles->len && i < printer->n_columns; i++)
    {
      char *title = g_ptr_array_index (printer->titles, i);

      if (title)
        widths[i] = MAX (widths[i], strlen (title));
    }

  for (i = 0; i < printer->rows->len; i++)
    {
      GPtrArray *row = g_ptr_array_index (printer->rows, i);

      for (j = 0; j < row->len; j++)
        {
          Cell *cell = g_ptr_array_index (row, j);
          int width;

          if (cell->span)
            width = 0;
          else
            width = strlen (cell->text);
          widths[j] = MAX (widths[j], width);
          if (cell->align >= 0)
            {
              lwidths[j] = MAX (lwidths[j], cell->align);
              rwidths[j] = MAX (rwidths[j], width - cell->align);
            }
        }
    }

  if (flatpak_fancy_output () && printer->titles->len > 0)
    {
      g_print (FLATPAK_ANSI_BOLD_ON);
      for (i = 0; i < printer->titles->len && i < printer->n_columns; i++)
        {
          char *title = g_ptr_array_index (printer->titles, i);

          g_print ("%s%-*s", (i == 0) ? "" : " ", widths[i], title);
        }
      g_print (FLATPAK_ANSI_BOLD_OFF);
      g_print ("\n");
    }

  for (i = 0; i < printer->rows->len; i++)
    {
      GPtrArray *row = g_ptr_array_index (printer->rows, i);

      for (j = 0; j < row->len; j++)
        {
          Cell *cell = g_ptr_array_index (row, j);
          if (flatpak_fancy_output ())
            {
              if (cell->span)
                g_print ("%s%s", (j == 0) ? "" : " ", cell->text);
              else if (cell->align < 0)
                g_print ("%s%-*s", (j == 0) ? "" : " ", widths[j], cell->text);
              else
                g_print ("%s%*s%-*s", (j == 0) ? "" : " ", lwidths[j] - cell->align, "", widths[j] - (lwidths[j] - cell->align), cell->text);
            }
          else
            g_print ("%s%s", cell->text, (j < row->len - 1) ? "\t" : "");
        }
      g_print ("\n");
    }
}
