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
#include "flatpak-utils-private.h"

#define FLATPAK_ANSI_ALT_SCREEN_ON "\x1b[?1049h"
#define FLATPAK_ANSI_ALT_SCREEN_OFF "\x1b[?1049l"
#define FLATPAK_ANSI_HIDE_CURSOR "\x1b[?25l"
#define FLATPAK_ANSI_SHOW_CURSOR "\x1b[?25h"
#define FLATPAK_ANSI_BOLD_ON "\x1b[1m"
#define FLATPAK_ANSI_BOLD_OFF "\x1b[22m"
#define FLATPAK_ANSI_FAINT_ON "\x1b[2m"
#define FLATPAK_ANSI_FAINT_OFF "\x1b[22m"
#define FLATPAK_ANSI_RED "\x1b[31m"
#define FLATPAK_ANSI_GREEN "\x1b[32m"
#define FLATPAK_ANSI_COLOR_RESET "\x1b[0m"

#define FLATPAK_ANSI_ROW_N "\x1b[%d;1H"
#define FLATPAK_ANSI_CLEAR "\x1b[0J"

gboolean flatpak_set_tty_echo (gboolean echo);
void flatpak_get_window_size (int *rows,
                              int *cols);
gboolean flatpak_get_cursor_pos (int *row,
                                 int *col);
void flatpak_hide_cursor (void);
void flatpak_show_cursor (void);

void flatpak_enable_raw_mode (void);
void flatpak_disable_raw_mode (void);

void     flatpak_disable_fancy_output (void);
void     flatpak_enable_fancy_output (void);
gboolean flatpak_fancy_output (void);

gboolean flatpak_allow_fuzzy_matching (const char *term);

char * flatpak_prompt (gboolean allow_empty,
                       const char *prompt,
                       ...) G_GNUC_PRINTF (2, 3);

char * flatpak_password_prompt (const char *prompt,
                                ...) G_GNUC_PRINTF (1, 2);

gboolean flatpak_yes_no_prompt (gboolean    default_yes,
                                const char *prompt,
                                ...) G_GNUC_PRINTF (2, 3);

long flatpak_number_prompt (gboolean    default_yes,
                            int         min,
                            int         max,
                            const char *prompt,
                            ...) G_GNUC_PRINTF (4, 5);
int *flatpak_numbers_prompt (gboolean    default_yes,
                             int         min,
                             int         max,
                             const char *prompt,
                             ...) G_GNUC_PRINTF (4, 5);
int *flatpak_parse_numbers (const char *buf,
                            int         min,
                            int         max);

void flatpak_format_choices (const char **choices,
                             const char  *prompt,
                             ...) G_GNUC_PRINTF (2, 3);

void   flatpak_print_escaped_string (const char        *s,
                                     FlatpakEscapeFlags flags);

void   flatpak_print_appstream_release_description_markup (const char *s);
