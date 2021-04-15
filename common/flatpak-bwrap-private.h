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

typedef struct
{
  GPtrArray *argv;
  GArray    *noinherit_fds; /* Just keep these open while the bwrap lives */
  GArray    *fds;
  GStrv      envp;
  GPtrArray *runtime_dir_members;
} FlatpakBwrap;

extern char *flatpak_bwrap_empty_env[1];

FlatpakBwrap *flatpak_bwrap_new (char **env);
void          flatpak_bwrap_free (FlatpakBwrap *bwrap);
void          flatpak_bwrap_set_env (FlatpakBwrap *bwrap,
                                     const char   *variable,
                                     const char   *value,
                                     gboolean      overwrite);
gboolean      flatpak_bwrap_is_empty (FlatpakBwrap *bwrap);
void          flatpak_bwrap_finish (FlatpakBwrap *bwrap);
void          flatpak_bwrap_unset_env (FlatpakBwrap *bwrap,
                                       const char   *variable);
void          flatpak_bwrap_add_arg (FlatpakBwrap *bwrap,
                                     const char   *arg);
void          flatpak_bwrap_take_arg (FlatpakBwrap *bwrap,
                                      char         *arg);
void          flatpak_bwrap_add_noinherit_fd (FlatpakBwrap *bwrap,
                                              int           fd);
void          flatpak_bwrap_add_fd (FlatpakBwrap *bwrap,
                                    int           fd);
void          flatpak_bwrap_add_args (FlatpakBwrap *bwrap,
                                      ...) G_GNUC_NULL_TERMINATED;
void          flatpak_bwrap_add_arg_printf (FlatpakBwrap *bwrap,
                                            const char   *format,
                                            ...) G_GNUC_PRINTF (2, 3);
void          flatpak_bwrap_append_argsv (FlatpakBwrap *bwrap,
                                          char        **args,
                                          int           len);
void          flatpak_bwrap_append_bwrap (FlatpakBwrap *bwrap,
                                          FlatpakBwrap *other);       /* Steals the fds */
void          flatpak_bwrap_append_args (FlatpakBwrap *bwrap,
                                         GPtrArray    *other_array);
void          flatpak_bwrap_add_args_data_fd (FlatpakBwrap *bwrap,
                                              const char   *op,
                                              int           fd,
                                              const char   *path_optional);
gboolean      flatpak_bwrap_add_args_data (FlatpakBwrap *bwrap,
                                           const char   *name,
                                           const char   *content,
                                           gssize        content_size,
                                           const char   *path,
                                           GError      **error);
void          flatpak_bwrap_add_bind_arg (FlatpakBwrap *bwrap,
                                          const char   *type,
                                          const char   *src,
                                          const char   *dest);
void          flatpak_bwrap_sort_envp (FlatpakBwrap *bwrap);
void          flatpak_bwrap_envp_to_args (FlatpakBwrap *bwrap);
gboolean      flatpak_bwrap_bundle_args (FlatpakBwrap *bwrap,
                                         int           start,
                                         int           end,
                                         gboolean      one_arg,
                                         GError      **error);
void          flatpak_bwrap_add_runtime_dir_member (FlatpakBwrap *bwrap,
                                                    const char *name);
void          flatpak_bwrap_populate_runtime_dir (FlatpakBwrap *bwrap);

void          flatpak_bwrap_child_setup_cb (gpointer user_data);
void          flatpak_bwrap_child_setup (GArray *fd_array,
                                         gboolean close_fd_workaround);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakBwrap, flatpak_bwrap_free)


#endif /* __FLATPAK_BWRAP_H__ */
