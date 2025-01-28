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
#include "flatpak-tty-utils-private.h"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <glib/gi18n-lib.h>

static int fancy_output = -1;

void
flatpak_disable_fancy_output (void)
{
  fancy_output = FALSE;
}

void
flatpak_enable_fancy_output (void)
{
  fancy_output = TRUE;
}

gboolean
flatpak_fancy_output (void)
{
  static gsize fancy_output_once = 0;
  enum {
    PLAIN_OUTPUT = 1,
    FANCY_OUTPUT = 2
  };

  if (fancy_output != -1)
    return fancy_output;

  if (g_once_init_enter (&fancy_output_once))
    {
      if (g_strcmp0 (g_getenv ("FLATPAK_FANCY_OUTPUT"), "0") == 0)
        g_once_init_leave (&fancy_output_once, PLAIN_OUTPUT);
      else if (getenv ("G_MESSAGES_DEBUG"))
        g_once_init_leave (&fancy_output_once, PLAIN_OUTPUT);
      else if (!isatty (STDOUT_FILENO))
        g_once_init_leave (&fancy_output_once, PLAIN_OUTPUT);
      else
        g_once_init_leave (&fancy_output_once, FANCY_OUTPUT);
    }

  return fancy_output_once == FANCY_OUTPUT;
}

gboolean
flatpak_allow_fuzzy_matching (const char *term)
{
  if (strchr (term, '/') != NULL || strchr (term, '.') != NULL)
    return FALSE;

  /* This env var is used by the unit tests and only skips the tty test not the
   * check above.
   */
  if (g_strcmp0 (g_getenv ("FLATPAK_FORCE_ALLOW_FUZZY_MATCHING"), "1") == 0)
    return TRUE;

  if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
    return FALSE;

  return TRUE;
}

char *
flatpak_prompt (gboolean allow_empty,
                const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;


  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  while (TRUE)
    {
      g_print ("%s: ", s);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("n\n");
          return NULL;
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return NULL;

      g_strstrip (buf);

      if (buf[0] != 0 || allow_empty)
        return g_strdup (buf);
    }
}

char *
flatpak_password_prompt (const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;
  gboolean was_echo;


  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  while (TRUE)
    {
      g_print ("%s: ", s);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        return NULL;

      was_echo = flatpak_set_tty_echo (FALSE);

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return NULL;

      flatpak_set_tty_echo (was_echo);

      g_strstrip (buf);

      /* We stole the return, so manual new line */
      g_print ("\n");
      return g_strdup (buf);
    }
}


gboolean
flatpak_yes_no_prompt (gboolean default_yes, const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;


  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  while (TRUE)
    {
      g_print ("%s %s: ", s, default_yes ? "[Y/n]" : "[y/n]");

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("n\n");
          return FALSE;
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return FALSE;

      g_strstrip (buf);

      if (default_yes && strlen (buf) == 0)
        return TRUE;

      if (g_ascii_strcasecmp (buf, "y") == 0 ||
          g_ascii_strcasecmp (buf, "yes") == 0)
        return TRUE;

      if (g_ascii_strcasecmp (buf, "n") == 0 ||
          g_ascii_strcasecmp (buf, "no") == 0)
        return FALSE;
    }
}

static gboolean
is_number (const char *s)
{
  if (*s == '\0')
    return FALSE;

  while (*s != 0)
    {
      if (!g_ascii_isdigit (*s))
        return FALSE;
      s++;
    }

  return TRUE;
}

long
flatpak_number_prompt (gboolean default_yes, int min, int max, const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;

  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  while (TRUE)
    {
      g_print ("%s [%d-%d]: ", s, min, max);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("0\n");
          return 0;
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return 0;

      g_strstrip (buf);

      if (default_yes && strlen (buf) == 0 &&
          max - min == 1 && min == 0)
        return 1;

      if (is_number (buf))
        {
          long res = strtol (buf, NULL, 10);

          if (res >= min && res <= max)
            return res;
        }
    }
}

static gboolean
parse_range (const char *s, int *a, int *b)
{
  char *p;

  p = strchr (s, '-');
  if (!p)
    return FALSE;

  p++;
  p[-1] = '\0';

  if (is_number (s) && is_number (p))
    {
      *a = (int) strtol (s, NULL, 10);
      *b = (int) strtol (p, NULL, 10);
      p[-1] = '-';
      return TRUE;
    }

  p[-1] = '-';
  return FALSE;
}

static void
add_number (GArray *numbers,
            int     num)
{
  int i;

  for (i = 0; i < numbers->len; i++)
    {
      if (g_array_index (numbers, int, i) == num)
        return;
    }

  g_array_append_val (numbers, num);
}

int *
flatpak_parse_numbers (const char *buf,
                       int         min,
                       int         max)
{
  g_autoptr(GArray) numbers = g_array_new (FALSE, FALSE, sizeof (int));
  g_auto(GStrv) parts = g_strsplit_set (buf, " ,", 0);
  int i, j;

  for (i = 0; parts[i]; i++)
    {
      int a, b;

      g_strstrip (parts[i]);

      if (parse_range (parts[i], &a, &b) &&
          min <= a && a <= max &&
          min <= b && b <= max)
        {
          for (j = a; j <= b; j++)
            add_number (numbers, j);
        }
      else if (is_number (parts[i]))
        {
          int res = (int) strtol (parts[i], NULL, 10);
          if (min <= res && res <= max)
            add_number (numbers, res);
          else
            return NULL;
        }
      else
        return NULL;
    }

  j = 0;
  g_array_append_val (numbers, j);

  return (int *) g_array_free (g_steal_pointer (&numbers), FALSE);
}

/* Returns a 0-terminated array of ints. Free with g_free */
int *
flatpak_numbers_prompt (gboolean default_yes, int min, int max, const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;
  g_autofree int *choice = g_new0 (int, 2);
  int *numbers;

  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  while (TRUE)
    {
      g_print ("%s [%d-%d]: ", s, min, max);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("0\n");
          choice[0] = 0;
          return g_steal_pointer (&choice);
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        {
          choice[0] = 0;
          return g_steal_pointer (&choice);
        }

      g_strstrip (buf);

      if (default_yes && strlen (buf) == 0 &&
          max - min == 1 && min == 0)
        {
          choice[0] = 0;
          return g_steal_pointer (&choice);
        }

      numbers = flatpak_parse_numbers (buf, min, max);
      if (numbers)
        return numbers;
    }
}

void
flatpak_format_choices (const char **choices,
                        const char  *prompt,
                        ...)
{
  va_list var_args;
  g_autofree char *s = NULL;
  int i;

  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  g_print ("%s\n\n", s);
  for (i = 0; choices[i]; i++)
    g_print ("  %2d) %s\n", i + 1, choices[i]);
  g_print ("\n");
}

void
flatpak_get_window_size (int *rows, int *cols)
{
  struct winsize w;

  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
    {
      /* For whatever reason, in buildbot this returns 0, 0 so add a fallback */
      if (w.ws_row == 0)
        w.ws_row = 24;
      if (w.ws_col == 0)
        w.ws_col = 80;
      *rows = w.ws_row;
      *cols = w.ws_col;
    }
  else
    {
      *rows = 24;
      *cols = 80;
    }
}

gboolean
flatpak_set_tty_echo (gboolean echo)
{
  struct termios term;
  gboolean was;

  tcgetattr (STDIN_FILENO, &term);
  was = (term.c_lflag & ECHO) != 0;

  if (echo)
    term.c_lflag |= ECHO;
  else
    term.c_lflag &= ~ECHO;
  tcsetattr (STDIN_FILENO, TCSANOW, &term);

  return was;
}

gboolean
flatpak_get_cursor_pos (int * row, int *col)
{
  fd_set readset;
  struct timeval time;
  struct termios term, initial_term;
  int res = 0;

  tcgetattr (STDIN_FILENO, &initial_term);
  term = initial_term;
  term.c_lflag &= ~ICANON;
  term.c_lflag &= ~ECHO;
  tcsetattr (STDIN_FILENO, TCSANOW, &term);

  printf ("\033[6n");
  fflush (stdout);

  FD_ZERO (&readset);
  FD_SET (STDIN_FILENO, &readset);
  time.tv_sec = 0;
  time.tv_usec = 100000;

  if (select (STDIN_FILENO + 1, &readset, NULL, NULL, &time) == 1)
    res = scanf ("\033[%d;%dR", row, col);

  tcsetattr (STDIN_FILENO, TCSADRAIN, &initial_term);

  return res == 2;
}

void
flatpak_hide_cursor (void)
{
  const size_t flatpak_hide_cursor_len = strlen (FLATPAK_ANSI_HIDE_CURSOR);
  const ssize_t write_ret = write (STDOUT_FILENO, FLATPAK_ANSI_HIDE_CURSOR,
                                   flatpak_hide_cursor_len);

  if (write_ret < 0)
    g_warning ("write() failed: %zd = write(STDOUT_FILENO, FLATPAK_ANSI_HIDE_CURSOR, %zu)",
               write_ret, flatpak_hide_cursor_len);
}

void
flatpak_show_cursor (void)
{
  const size_t flatpak_show_cursor_len = strlen (FLATPAK_ANSI_SHOW_CURSOR);
  const ssize_t write_ret = write (STDOUT_FILENO, FLATPAK_ANSI_SHOW_CURSOR,
                                   flatpak_show_cursor_len);

  if (write_ret < 0)
    g_warning ("write() failed: %zd = write(STDOUT_FILENO, FLATPAK_ANSI_SHOW_CURSOR, %zu)",
               write_ret, flatpak_show_cursor_len);
}

void
flatpak_enable_raw_mode (void)
{
  struct termios raw;

  tcgetattr (STDIN_FILENO, &raw);

  raw.c_lflag &= ~(ECHO | ICANON);

  tcsetattr (STDIN_FILENO, TCSAFLUSH, &raw);
}

void
flatpak_disable_raw_mode (void)
{
  struct termios raw;

  tcgetattr (STDIN_FILENO, &raw);

  raw.c_lflag |= (ECHO | ICANON);

  tcsetattr (STDIN_FILENO, TCSAFLUSH, &raw);
}

void
flatpak_print_escaped_string (const char        *s,
                              FlatpakEscapeFlags flags)
{
  g_autofree char *escaped = flatpak_escape_string (s, flags);
  g_print ("%s", escaped);
}

static gboolean
use_progress_escape_sequence (void)
{
  static gsize tty_progress_once = 0;
  enum {
    TTY_PROGRESS_ENABLED = 1,
    TTY_PROGRESS_DISABLED = 2
  };

  if (g_once_init_enter (&tty_progress_once))
    {
      // FIXME: make this opt-out for Flatpak 1.18
      if (g_strcmp0 (g_getenv ("FLATPAK_TTY_PROGRESS"), "1") == 0)
        g_once_init_leave (&tty_progress_once, TTY_PROGRESS_ENABLED);
      else
        g_once_init_leave (&tty_progress_once, TTY_PROGRESS_DISABLED);
    }

  return tty_progress_once == TTY_PROGRESS_ENABLED;
}

void
flatpak_pty_clear_progress (void)
{
  if (use_progress_escape_sequence ())
    g_print ("\033]9;4;0\e\\");
}

void
flatpak_pty_set_progress (guint percent)
{
  if (use_progress_escape_sequence ())
    g_print ("\033]9;4;1;%d\e\\", MIN (percent, 100));
}
