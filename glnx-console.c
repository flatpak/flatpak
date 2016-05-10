/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014,2015 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "glnx-console.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>

static char *current_text = NULL;
static gint current_percent = -1;
static gboolean locked;

static gboolean
stdout_is_tty (void)
{
  static gsize initialized = 0;
  static gboolean stdout_is_tty_v;

  if (g_once_init_enter (&initialized))
    {
      stdout_is_tty_v = isatty (1);
      g_once_init_leave (&initialized, 1);
    }

  return stdout_is_tty_v;
}

static volatile guint cached_columns = 0;
static volatile guint cached_lines = 0;

static int
fd_columns (int fd)
{
  struct winsize ws = {};

  if (ioctl (fd, TIOCGWINSZ, &ws) < 0)
    return -errno;

  if (ws.ws_col <= 0)
    return -EIO;

  return ws.ws_col;
}

/**
 * glnx_console_columns:
 * 
 * Returns: The number of columns for terminal output
 */
guint
glnx_console_columns (void)
{
  if (G_UNLIKELY (cached_columns == 0))
    {
      int c;

      c = fd_columns (STDOUT_FILENO);

      if (c <= 0)
        c = 80;

      if (c > 256)
        c = 256;

      cached_columns = c;
    }

  return cached_columns;
}

static int
fd_lines (int fd)
{
  struct winsize ws = {};

  if (ioctl (fd, TIOCGWINSZ, &ws) < 0)
    return -errno;

  if (ws.ws_row <= 0)
    return -EIO;

  return ws.ws_row;
}

/**
 * glnx_console_lines:
 * 
 * Returns: The number of lines for terminal output
 */
guint
glnx_console_lines (void)
{
  if (G_UNLIKELY (cached_lines == 0))
    {
      int l;

      l = fd_lines (STDOUT_FILENO);

      if (l <= 0)
        l = 24;

      cached_lines = l;
    }

  return cached_lines;
}

static void
on_sigwinch (int signum)
{
  cached_columns = 0;
  cached_lines = 0;
}

void
glnx_console_lock (GLnxConsoleRef *console)
{
  static gsize sigwinch_initialized = 0;

  g_return_if_fail (!locked);
  g_return_if_fail (!console->locked);

  console->is_tty = stdout_is_tty ();

  locked = console->locked = TRUE;

  current_percent = 0;

  if (console->is_tty)
    {
      if (g_once_init_enter (&sigwinch_initialized))
        {
          signal (SIGWINCH, on_sigwinch);
          g_once_init_leave (&sigwinch_initialized, 1);
        }
      
      { static const char initbuf[] = { '\n', 0x1B, 0x37 };
        (void) fwrite (initbuf, 1, sizeof (initbuf), stdout);
      }
    }
}

static void
printpad (const char *padbuf,
          guint       padbuf_len,
          guint       n)
{
  const guint d = n / padbuf_len;
  const guint r = n % padbuf_len;
  guint i;

  for (i = 0; i < d; i++)
    fwrite (padbuf, 1, padbuf_len, stdout);
  fwrite (padbuf, 1, r, stdout);
}

static void
text_percent_internal (const char *text,
                       int percentage)
{
  static const char equals[] = "====================";
  const guint n_equals = sizeof (equals) - 1;
  static const char spaces[] = "                    ";
  const guint n_spaces = sizeof (spaces) - 1;
  const guint ncolumns = glnx_console_columns ();
  const guint bar_min = 10;
  const guint input_textlen = text ? strlen (text) : 0;

  if (text && !*text)
    text = NULL;

  if (percentage == current_percent
      && g_strcmp0 (text, current_text) == 0)
    return;

  if (!stdout_is_tty ())
    {
      if (text)
        fprintf (stdout, "%s", text);
      if (percentage != -1)
        {
          if (text)
            fputc (' ', stdout);
          fprintf (stdout, "%u%%", percentage);
        }
      fputc ('\n', stdout);
      fflush (stdout);
      return;
    }

  if (ncolumns < bar_min)
    return; /* TODO: spinner */

  /* Restore cursor */
  { const char beginbuf[2] = { 0x1B, 0x38 };
    (void) fwrite (beginbuf, 1, sizeof (beginbuf), stdout);
  }

  if (percentage == -1)
    {
      const guint spacelen = ncolumns - input_textlen;
      fwrite (text, 1, input_textlen, stdout);
      printpad (spaces, n_spaces, spacelen);
    }
  else
    {
      const guint textlen = MIN (input_textlen, ncolumns - bar_min);
      const guint barlen = ncolumns - (textlen + 1);;

      if (textlen > 0)
        {
          fwrite (text, 1, textlen, stdout);
          fputc (' ', stdout);
        }

      {
        const guint nbraces = 2;
        const guint textpercent_len = 5;
        const guint bar_internal_len = barlen - nbraces - textpercent_len;
        const guint eqlen = bar_internal_len * (percentage / 100.0);
        const guint spacelen = bar_internal_len - eqlen; 

        fputc ('[', stdout);
        printpad (equals, n_equals, eqlen);
        printpad (spaces, n_spaces, spacelen);
        fputc (']', stdout);
        fprintf (stdout, " %3d%%", percentage);
      }
    }

  fflush (stdout);
}

/**
 * glnx_console_progress_text_percent:
 * @text: Show this text before the progress bar
 * @percentage: An integer in the range of 0 to 100
 *
 * On a tty, print to the console @text followed by an ASCII art
 * progress bar whose percentage is @percentage.  If stdout is not a
 * tty, a more basic line by line change will be printed.
 *
 * You must have called glnx_console_lock() before invoking this
 * function.
 *
 */
void
glnx_console_progress_text_percent (const char *text,
                                    guint percentage)
{
  g_return_if_fail (percentage >= 0 && percentage <= 100);

  text_percent_internal (text, percentage);
}

void
glnx_console_text (const char *text)
{
  text_percent_internal (text, -1);
}

/**
 * glnx_console_unlock:
 *
 * Print a newline, and reset all cached console progress state.
 *
 * This function does nothing if stdout is not a tty.
 */
void
glnx_console_unlock (GLnxConsoleRef *console)
{
  g_return_if_fail (locked);
  g_return_if_fail (console->locked);

  current_percent = -1;
  g_clear_pointer (&current_text, g_free);

  if (console->is_tty)
    fputc ('\n', stdout);
      
  locked = console->locked = FALSE;
}
