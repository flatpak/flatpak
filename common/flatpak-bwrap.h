/*
 * Copyright © 2014-2018 Red Hat, Inc
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

#ifndef __FLATPAK_BWRAP_H__
#define __FLATPAK_BWRAP_H__

typedef struct {
  GPtrArray *argv;
  GArray *fds;
  GStrv envp;
} FlatpakBwrap;

FlatpakBwrap *flatpak_bwrap_new              (char         **env);
void          flatpak_bwrap_free             (FlatpakBwrap  *bwrap);
void          flatpak_bwrap_set_env          (FlatpakBwrap  *bwrap,
                                              const char    *variable,
                                              const char    *value,
                                              gboolean       overwrite);
void          flatpak_bwrap_unset_env        (FlatpakBwrap  *bwrap,
                                              const char    *variable);
void          flatpak_bwrap_add_args         (FlatpakBwrap  *bwrap,
                                              ...);
void          flatpak_bwrap_append_argsv     (FlatpakBwrap *bwrap,
                                              char        **args,
                                              int           len);
void          flatpak_bwrap_append_args      (FlatpakBwrap  *bwrap,
                                              GPtrArray     *other_array);
void          flatpak_bwrap_add_args_data_fd (FlatpakBwrap  *bwrap,
                                              const char    *op,
                                              int            fd,
                                              const char    *path_optional);
gboolean      flatpak_bwrap_add_args_data    (FlatpakBwrap  *bwrap,
                                              const char    *name,
                                              const char    *content,
                                              gssize         content_size,
                                              const char    *path,
                                              GError       **error);
void          flatpak_bwrap_add_bind_arg     (FlatpakBwrap  *bwrap,
                                              const char    *type,
                                              const char    *src,
                                              const char    *dest);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakBwrap, flatpak_bwrap_free)


#endif /* __FLATPAK_BWRAP_H__ */
