/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <json-glib/json-gobject.h>
#include "flatpak-table-printer.h"
#include "flatpak-tty-utils-private.h"
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

typedef struct
{
  GPtrArray *cells;
  char      *key;
} Row;

static void
free_row (gpointer data)
{
  Row *row = data;

  g_ptr_array_free (row->cells, TRUE);
  g_free (row->key);
  g_free (row);
}

typedef struct
{
  char                *title;
  gboolean             expand;
  FlatpakEllipsizeMode ellipsize;
  gboolean             skip_unique;
  char                *skip_unique_str;
  gboolean             skip;
} TableColumn;

static void
free_column (gpointer data)
{
  TableColumn *column = data;

  g_free (column->title);
  g_free (column->skip_unique_str);
  g_free (column);
}

struct FlatpakTablePrinter
{
  GPtrArray *columns;
  GPtrArray *rows;
  GHashTable *rows_ht;
  char      *key;
  GPtrArray *current;
  int        n_columns;
};

FlatpakTablePrinter *
flatpak_table_printer_new (void)
{
  FlatpakTablePrinter *printer = g_new0 (FlatpakTablePrinter, 1);

  printer->columns = g_ptr_array_new_with_free_func (free_column);
  printer->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) free_row);
  printer->rows_ht = g_hash_table_new (g_str_hash, g_str_equal);
  printer->current = g_ptr_array_new_with_free_func (free_cell);

  return printer;
}

void
flatpak_table_printer_free (FlatpakTablePrinter *printer)
{
  g_ptr_array_free (printer->columns, TRUE);
  g_ptr_array_free (printer->rows, TRUE);
  g_hash_table_destroy (printer->rows_ht);
  g_ptr_array_free (printer->current, TRUE);
  g_free (printer->key);
  g_free (printer);
}

static TableColumn *
peek_table_column (FlatpakTablePrinter *printer,
                   int                  column)
{
  if (column < printer->columns->len)
    return g_ptr_array_index (printer->columns, column);

  return NULL;
}

static TableColumn *
get_table_column (FlatpakTablePrinter *printer,
                  int                  column)
{
  TableColumn *col = NULL;

  if (column < printer->columns->len)
    col = g_ptr_array_index (printer->columns, column);

  if (col == NULL)
    {
      col = g_new0 (TableColumn, 1);
      g_ptr_array_insert (printer->columns, column, col);
    }

  return col;
}

void
flatpak_table_printer_set_column_title (FlatpakTablePrinter *printer,
                                        int                  column,
                                        const char          *text)
{
  TableColumn *col = get_table_column (printer, column);

  col->title = g_strdup (text);
}

void
flatpak_table_printer_set_columns (FlatpakTablePrinter *printer,
                                   Column              *columns,
                                   gboolean             defaults)
{
  int i;

  for (i = 0; columns[i].name; i++)
    {
      flatpak_table_printer_set_column_title (printer, i, _(columns[i].title));
      flatpak_table_printer_set_column_expand (printer, i, columns[i].expand);
      flatpak_table_printer_set_column_ellipsize (printer, i, columns[i].ellipsize);
      if (defaults && columns[i].skip_unique_if_default)
        flatpak_table_printer_set_column_skip_unique (printer, i, TRUE);
    }
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
flatpak_table_printer_take_column (FlatpakTablePrinter *printer,
                                   char                *text)
{
  flatpak_table_printer_add_aligned_column (printer, text, -1);
  g_free (text);
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

  va_start (var_args, format);
  s = g_strdup_vprintf (format, var_args);
  va_end (var_args);

  flatpak_table_printer_append_with_comma (printer, s);
}

void
flatpak_table_printer_set_key (FlatpakTablePrinter *printer, const char *key)
{
  printer->key = g_strdup (key);
}

static gint
cmp_row (gconstpointer _row_a,
         gconstpointer _row_b,
         gpointer      user_data)
{
  const Row *row_a = *(const Row **) _row_a;
  const Row *row_b = *(const Row **) _row_b;
  GCompareFunc cmp = user_data;

  if (row_a == row_b || (row_a->key == NULL && row_b->key == NULL))
    return 0;
  if (row_a->key == NULL)
    return -1;
  if (row_b->key == NULL)
    return 1;

  return cmp (row_a->key, row_b->key);
}

void
flatpak_table_printer_sort (FlatpakTablePrinter *printer, GCompareFunc cmp)
{
  g_ptr_array_sort_with_data (printer->rows, cmp_row, cmp);
}

int
flatpak_table_printer_lookup_row (FlatpakTablePrinter *printer, const char *key)
{
  gpointer value;

  if (g_hash_table_lookup_extended (printer->rows_ht, key, NULL, &value))
    return GPOINTER_TO_INT(value);

  return -1;
}

void
flatpak_table_printer_finish_row (FlatpakTablePrinter *printer)
{
  Row *row;
  int row_nr = flatpak_table_printer_get_current_row (printer);

  if (printer->current->len == 0)
    return; /* Ignore empty rows */

  printer->n_columns = MAX (printer->n_columns, printer->current->len);
  row = g_new0 (Row, 1);
  row->cells = g_steal_pointer (&printer->current);
  row->key = g_steal_pointer (&printer->key);
  g_ptr_array_add (printer->rows, row);
  if (row->key)
    g_hash_table_insert (printer->rows_ht, row->key, GINT_TO_POINTER (row_nr));
  printer->current = g_ptr_array_new_with_free_func (free_cell);
}

/* Return how many terminal rows we produced (with wrapping to columns)
 * while skipping 'skip' many of them. 'skip' is updated to reflect
 * how many we skipped.
 */
static int
print_row (GString *row_s, gboolean bold, int *skip, int columns)
{
  int rows;
  const char *p, *end;
  int n_chars;

  g_strchomp (row_s->str);
  n_chars = cell_width (row_s->str);
  if (n_chars > 0)
    rows = (n_chars + columns - 1) / columns;
  else
    rows = 1;

  p = row_s->str;
  end = row_s->str + strlen (row_s->str);
  while (*skip > 0 && p <= end)
    {
      (*skip)--;
      p = cell_advance (p, columns);
    }

  if (p < end || p == row_s->str)
    {
      if (bold)
        g_print (FLATPAK_ANSI_BOLD_ON "%s" FLATPAK_ANSI_BOLD_OFF, p);
      else
        g_print ("%s", p);
    }
  g_string_truncate (row_s, 0);

  return rows;
}

static void
string_add_spaces (GString *str, int count)
{
  while (count-- > 0)
    g_string_append_c (str, ' ');
}

static gboolean
column_is_unique (FlatpakTablePrinter *printer, int col)
{
  TableColumn *column = get_table_column (printer, col);
  char *first_row = column->skip_unique_str;
  int i;

  for (i = 0; i < printer->rows->len; i++)
    {
      Row *row = g_ptr_array_index (printer->rows, i);
      if (col >= row->cells->len)
        continue;

      Cell *cell = g_ptr_array_index (row->cells, col);

      if (i == 0 && first_row == NULL)
        first_row = cell->text;
      else
        {
          if (g_strcmp0 (first_row, cell->text) != 0)
            return FALSE;
        }
    }

  return TRUE;
}

/*
 * This variant of flatpak_table_printer_print() takes a window width
 * and returns the number of rows that are generated by printing the
 * table to that width. It also takes a number of (terminal) rows
 * to skip at the beginning of the table.
 *
 * Care is taken to do the right thing if the skipping ends
 * in the middle of a wrapped table row.
 *
 * Note that unlike flatpak_table_printer_print(), this function does
 * not add a newline after the last table row.
 */
void
flatpak_table_printer_print_full (FlatpakTablePrinter *printer,
                                  int                  skip,
                                  int                  columns,
                                  int                 *table_height,
                                  int                 *table_width)
{
  g_autofree int *widths = NULL;
  g_autofree int *lwidths = NULL;
  g_autofree int *rwidths = NULL;
  g_autofree int *shrinks = NULL;
  g_autoptr(GString) row_s = g_string_new ("");
  int i, j;
  int rows = 0;
  int total_skip = skip;
  int width;
  int expand_columns;
  int shrink_columns;
  gboolean has_title;
  int expand_by, expand_extra;

  if (printer->current->len != 0)
    flatpak_table_printer_finish_row (printer);

  widths = g_new0 (int, printer->n_columns);
  lwidths = g_new0 (int, printer->n_columns);
  rwidths = g_new0 (int, printer->n_columns);
  shrinks = g_new0 (int, printer->n_columns);

  for (i = 0; i < printer->columns->len && i < printer->n_columns; i++)
    {
      TableColumn *col = g_ptr_array_index (printer->columns, i);

      if (col->skip_unique && column_is_unique (printer, i))
        col->skip = TRUE;
    }

  has_title = FALSE;
  for (i = 0; i < printer->columns->len && i < printer->n_columns; i++)
    {
      TableColumn *col = g_ptr_array_index (printer->columns, i);

      if (col->skip)
        continue;

      if (col->title)
        {
          widths[i] = MAX (widths[i], cell_width (col->title));
          has_title = TRUE;
        }
    }

  for (i = 0; i < printer->rows->len; i++)
    {
      Row *row = g_ptr_array_index (printer->rows, i);

      for (j = 0; j < row->cells->len; j++)
        {
          Cell *cell = g_ptr_array_index (row->cells, j);
          TableColumn *col = peek_table_column (printer, j);

          if (col && col->skip)
            continue;

          if (cell->span)
            width = 0;
          else
            width = cell_width (cell->text);
          widths[j] = MAX (widths[j], width);
          if (cell->align >= 0)
            {
              lwidths[j] = MAX (lwidths[j], cell->align);
              rwidths[j] = MAX (rwidths[j], width - cell->align);
            }
        }
    }

  width = printer->n_columns - 1;
  for (i = 0; i < printer->n_columns; i++)
    width += widths[i];

  expand_columns = 0;
  shrink_columns = 0;
  for (i = 0; i < printer->columns->len; i++)
    {
      TableColumn *col = g_ptr_array_index (printer->columns, i);
      if (col && col->skip)
        continue;
      if (col && col->expand)
        expand_columns++;
      if (col && col->ellipsize)
        shrink_columns++;
    }

  expand_by = 0;
  expand_extra = 0;
  if (expand_columns > 0)
    {
      int excess = CLAMP (columns - width, 0, width / 2);
      expand_by = excess / expand_columns;
      expand_extra = excess % expand_columns;
      width += excess;
    }

  if (shrink_columns > 0)
    {
      int shortfall = MAX (width - columns, 0);
      int last;
      if (shortfall > 0)
        {
          int shrinkable = 0;
          int leftover = shortfall;

          /* We're distributing the shortfall so that wider columns
           * shrink proportionally more than narrower ones, while
           * avoiding to ellipsize the titles.
           */
          for (i = 0; i < printer->columns->len && i < printer->n_columns; i++)
            {
              TableColumn *col = g_ptr_array_index (printer->columns, i);
              gboolean ellipsize = col ? col->ellipsize : FALSE;

              if (col && col->skip)
                continue;

              if (!ellipsize)
                continue;

              if (col && col->title)
                shrinkable += MAX (0, widths[i] - cell_width (col->title));
              else
                shrinkable += MAX (0, widths[i] - 5);
            }

          for (i = 0; i < printer->columns->len && i < printer->n_columns; i++)
            {
              TableColumn *col = g_ptr_array_index (printer->columns, i);
              gboolean ellipsize = col ? col->ellipsize : FALSE;

              if (col && col->skip)
                continue;

              if (ellipsize)
                {
                  int sh;
                  if (col && col->title)
                    sh = MAX (0, widths[i] - cell_width (col->title));
                  else
                    sh = MAX (0, widths[i] - 5);
                  shrinks[i] = MIN (shortfall * (sh / (double) shrinkable), widths[i]);
                  leftover -= shrinks[i];
                }
            }

          last = leftover + 1;
          while (leftover > 0 && leftover < last)
            {
              last = leftover;
              for (i = 0; i < printer->columns->len && i < printer->n_columns; i++)
                {
                  TableColumn *col = g_ptr_array_index (printer->columns, i);
                  gboolean ellipsize = col ? col->ellipsize : FALSE;

                  if (col && col->skip)
                    continue;

                  if (ellipsize && shrinks[i] < widths[i])
                    {
                      shrinks[i]++;
                      leftover--;
                    }
                  if (leftover == 0)
                    break;
                }
            }
        }

      for (i = 0; i < printer->n_columns; i++)
        width -= shrinks[i];
    }

  if (flatpak_fancy_output () && has_title)
    {
      int grow = expand_extra;
      for (i = 0; i < printer->columns->len && i < printer->n_columns; i++)
        {
          TableColumn *col = g_ptr_array_index (printer->columns, i);
          const char *title = col && col->title ? col->title : "";
          gboolean expand = col ? col->expand : FALSE;
          gboolean ellipsize = col ? col->ellipsize : FALSE;
          int len = widths[i];
          g_autofree char *freeme = NULL;

          if (col && col->skip)
            continue;

          if (expand_by > 0 && expand)
            {
              len += expand_by;
              if (grow > 0)
                {
                  len++;
                  grow--;
                }
            }

          if (shrinks[i] > 0 && ellipsize)
            {
              len -= shrinks[i];
              title = freeme = ellipsize_string (title, len);
            }

          if (i > 0)
            g_string_append_c (row_s, ' ');
          g_string_append (row_s, title);
          string_add_spaces (row_s, len - cell_width (title));
        }
      rows += print_row (row_s, TRUE, &skip, columns);
    }

  for (i = 0; i < printer->rows->len; i++)
    {
      Row *row = g_ptr_array_index (printer->rows, i);
      int grow = expand_extra;

      if (rows > total_skip)
        g_print ("\n");

      for (j = 0; j < row->cells->len; j++)
        {
          TableColumn *col = peek_table_column (printer, j);
          gboolean expand = col ? col->expand : FALSE;
          gboolean ellipsize = col ? col->ellipsize : FALSE;
          Cell *cell = g_ptr_array_index (row->cells, j);
          char *text = cell->text;
          int len = widths[j];
          g_autofree char *freeme = NULL;

          if (col && col->skip)
            continue;

          if (expand_by > 0 && expand)
            {
              len += expand_by;
              if (grow > 0)
                {
                  len++;
                  grow--;
                }
            }

          if (shrinks[j] > 0 && ellipsize)
            {
              len -= shrinks[j];
              freeme = text = ellipsize_string_full (text, len, col->ellipsize);
            }

          if (flatpak_fancy_output ())
            {
              if (j > 0)
                g_string_append_c (row_s, ' ');
              if (cell->span)
                g_string_append (row_s, cell->text);
              else if (cell->align < 0)
                {
                  g_string_append (row_s, text);
                  string_add_spaces (row_s, len - cell_width (text));
                }
              else
                {
                  string_add_spaces (row_s, lwidths[j] - cell->align);
                  g_string_append (row_s, text);
                  string_add_spaces (row_s, widths[j] - (lwidths[j] - cell->align)  - cell_width (text));
                }
            }
          else
            g_string_append_printf (row_s, "%s%s", cell->text, (j < row->cells->len - 1) ? "\t" : "");
        }
      rows += print_row (row_s, FALSE, &skip, columns);
    }

  if (table_width)
    *table_width = width;
  if (table_height)
    *table_height = rows;
}

void
flatpak_table_printer_print (FlatpakTablePrinter *printer)
{
  int rows, cols;

  flatpak_get_window_size (&rows, &cols);
  flatpak_table_printer_print_full (printer, 0, cols, NULL, NULL);
  g_print ("\n");
}

void
flatpak_table_printer_print_json (FlatpakTablePrinter *printer)
{
  g_autoptr(JsonArray) json_array = json_array_new ();
  g_autoptr(JsonNode) root_node = NULL;
  g_autoptr(JsonObject) json_object = NULL;
  Row *row;
  Cell *cell;
  TableColumn *col;
  char *title;
  g_autofree gchar *json_string = NULL;

  for (size_t i = 0; i < printer->rows->len; i++)
    {
      json_object = json_object_new ();
      row = (Row *) g_ptr_array_index (printer->rows, i);
      
      for (size_t j = 0; j < row->cells->len; j++)
        {
          cell = (Cell *) g_ptr_array_index (row->cells, j);
          col = (TableColumn *) peek_table_column (printer, j);
          title = col && col->title ? col->title : "";
          json_object_set_string_member (json_object, title, cell->text);
        }
      
      json_array_add_object_element (json_array, g_steal_pointer (&json_object));
    }

  root_node = json_node_new (JSON_NODE_ARRAY);
  json_node_take_array (root_node, g_steal_pointer (&json_array));

  json_string = json_to_string (root_node, TRUE);
  g_print ("%s\n", json_string);
}

int
flatpak_table_printer_get_current_row (FlatpakTablePrinter *printer)
{
  return printer->rows->len;
}

static void
set_cell (FlatpakTablePrinter *printer,
          int                  r,
          int                  c,
          const char          *text,
          int                  align,
          int                  append)
{
  Row *row;
  Cell *cell;
  char *old;

  row = (Row *) g_ptr_array_index (printer->rows, r);

  g_assert (row);

  cell = (Cell *) g_ptr_array_index (row->cells, c);
  g_assert (cell);

  old = cell->text;
  if (old != NULL && append)
    {
      if (append == 2 && *old != 0)
        cell->text = g_strconcat (old, ", ", text, NULL);
      else
        cell->text = g_strconcat (old, text, NULL);
    }
  else
    cell->text = g_strdup (text);
  cell->align = align;

  g_free (old);
}

void
flatpak_table_printer_set_cell (FlatpakTablePrinter *printer,
                                int                  r,
                                int                  c,
                                const char          *text)
{
  set_cell (printer, r, c, text, -1, 0);
}

void
flatpak_table_printer_append_cell (FlatpakTablePrinter *printer,
                                   int                  r,
                                   int                  c,
                                   const char          *text)
{
  set_cell (printer, r, c, text, -1, 1);
}

void
flatpak_table_printer_append_cell_with_comma (FlatpakTablePrinter *printer,
                                              int                  r,
                                              int                  c,
                                              const char          *text)
{
  set_cell (printer, r, c, text, -1, 2);
}

void
flatpak_table_printer_append_cell_with_comma_unique (FlatpakTablePrinter *printer,
                                                     int                  r,
                                                     int                  c,
                                                     const char          *text)
{
  Row *row;
  Cell *cell;

  row = (Row *) g_ptr_array_index (printer->rows, r);
  g_assert (row);
  cell = (Cell *) g_ptr_array_index (row->cells, c);
  g_assert (cell);

  /* Look for existing text in comma separated text */
  if (cell->text != NULL && *text != 0)
    {
      gsize len = strlen (text);
      const char *match = cell->text;
      while ((match = strstr (match, text)) != NULL)
        {
          if (match[len] == 0 || match[len] == ',' )
            return; /* Already in string, do nothing */
          /* Look for next match */
          match = match + len;
        }
    }

  set_cell (printer, r, c, text, -1, 2);
}

void
flatpak_table_printer_set_decimal_cell (FlatpakTablePrinter *printer,
                                        int                  r,
                                        int                  c,
                                        const char          *text)
{
  int align = -1;
  const char *decimal = find_decimal_point (text);

  if (decimal)
    align = decimal - text;

  set_cell (printer, r, c, text, align, 0);
}

void
flatpak_table_printer_set_column_expand (FlatpakTablePrinter *printer,
                                         int                  column,
                                         gboolean             expand)
{
  TableColumn *col = get_table_column (printer, column);

  col->expand = expand;
}

void
flatpak_table_printer_set_column_ellipsize (FlatpakTablePrinter *printer,
                                            int                  column,
                                            FlatpakEllipsizeMode mode)
{
  TableColumn *col = get_table_column (printer, column);

  col->ellipsize = mode;
}

/* Specifies that the column should be skipped if all values are the same */
void
flatpak_table_printer_set_column_skip_unique (FlatpakTablePrinter *printer,
                                              int                  column,
                                              gboolean             skip_unique)
{
  TableColumn *col = get_table_column (printer, column);

  col->skip_unique = skip_unique;
}

/* This modifies set_column_skip_unique to also require that the
 * unique value of the column must be this particular string. Useful if you
 * want to e.g. skip the arch list if everything is for the primary arch, but
 * not if everything is for a non-standard arch.
 */
void
flatpak_table_printer_set_column_skip_unique_string (FlatpakTablePrinter *printer,
                                                     int                  column,
                                                     const char          *str)
{
  TableColumn *col = get_table_column (printer, column);

  g_assert (col->skip_unique_str == NULL);

  col->skip_unique_str = g_strdup (str);
}
