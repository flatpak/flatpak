/*
 * Copyright © 2015 Red Hat, Inc
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

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_REMOTE_H__
#define __FLATPAK_REMOTE_H__

/**
 * FlatpakRemoteType:
 * @FLATPAK_REMOTE_TYPE_STATIC: Statically configured remote
 * @FLATPAK_REMOTE_TYPE_USB: Dynamically detected local pathname remote
 * @FLATPAK_REMOTE_TYPE_LAN: Dynamically detected network remote
 *
 * Different types of @FlatpakRemote.
 */
typedef enum {
  FLATPAK_REMOTE_TYPE_STATIC,
  FLATPAK_REMOTE_TYPE_USB,
  FLATPAK_REMOTE_TYPE_LAN,
} FlatpakRemoteType;

typedef struct _FlatpakRemote FlatpakRemote;

#include <gio/gio.h>
#include <flatpak-remote-ref.h>

#define FLATPAK_TYPE_REMOTE flatpak_remote_get_type ()
#define FLATPAK_REMOTE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_REMOTE, FlatpakRemote))
#define FLATPAK_IS_REMOTE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_REMOTE))

FLATPAK_EXTERN GType flatpak_remote_get_type (void);

struct _FlatpakRemote
{
  GObject parent;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakRemoteClass;

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRemote, g_object_unref)
#endif

FLATPAK_EXTERN FlatpakRemote * flatpak_remote_new (const char    *name);
FLATPAK_EXTERN FlatpakRemote * flatpak_remote_new_from_file (const char *name,
                                                             GBytes     *data,
                                                             GError **error);

FLATPAK_EXTERN const char *  flatpak_remote_get_name (FlatpakRemote *self);
FLATPAK_EXTERN GFile *       flatpak_remote_get_appstream_dir (FlatpakRemote *self,
                                                               const char    *arch);
FLATPAK_EXTERN GFile *       flatpak_remote_get_appstream_timestamp (FlatpakRemote *self,
                                                                     const char    *arch);
FLATPAK_EXTERN char *        flatpak_remote_get_url (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_url (FlatpakRemote *self,
                                                     const char    *url);
FLATPAK_EXTERN char *        flatpak_remote_get_collection_id (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_collection_id (FlatpakRemote *self,
                                                               const char    *collection_id);
FLATPAK_EXTERN char *        flatpak_remote_get_title (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_title (FlatpakRemote *self,
                                                       const char    *title);
FLATPAK_EXTERN char *        flatpak_remote_get_comment (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_comment (FlatpakRemote *self,
                                                         const char    *comment);
FLATPAK_EXTERN char *        flatpak_remote_get_description (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_description (FlatpakRemote *self,
                                                             const char    *description);
FLATPAK_EXTERN char *        flatpak_remote_get_homepage (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_homepage (FlatpakRemote *self,
                                                          const char    *homepage);
FLATPAK_EXTERN char *        flatpak_remote_get_icon (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_icon (FlatpakRemote *self,
                                                      const char    *icon);
FLATPAK_EXTERN char *        flatpak_remote_get_default_branch (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_default_branch (FlatpakRemote *self,
                                                                const char    *default_branch);
FLATPAK_EXTERN char *        flatpak_remote_get_main_ref (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_main_ref (FlatpakRemote *self,
                                                          const char    *main_ref);
FLATPAK_EXTERN gboolean      flatpak_remote_get_gpg_verify (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_gpg_verify (FlatpakRemote *self,
                                                            gboolean       gpg_verify);
FLATPAK_EXTERN void          flatpak_remote_set_gpg_key (FlatpakRemote *self,
                                                         GBytes        *gpg_key);
FLATPAK_EXTERN gboolean      flatpak_remote_get_sign_verify (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_sign_verify (FlatpakRemote *self,
                                                             gboolean       sign_verify);
FLATPAK_EXTERN gboolean      flatpak_remote_get_noenumerate (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_noenumerate (FlatpakRemote *self,
                                                             gboolean       noenumerate);
FLATPAK_EXTERN gboolean      flatpak_remote_get_nodeps (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_nodeps (FlatpakRemote *self,
                                                        gboolean       nodeps);
FLATPAK_EXTERN gboolean      flatpak_remote_get_disabled (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_disabled (FlatpakRemote *self,
                                                          gboolean       disabled);
FLATPAK_EXTERN int           flatpak_remote_get_prio (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_prio (FlatpakRemote *self,
                                                      int            prio);
FLATPAK_EXTERN char *        flatpak_remote_get_filter (FlatpakRemote *self);
FLATPAK_EXTERN void          flatpak_remote_set_filter (FlatpakRemote *self,
                                                        const char    *filter_path);

FLATPAK_EXTERN FlatpakRemoteType flatpak_remote_get_remote_type (FlatpakRemote *self);


#endif /* __FLATPAK_REMOTE_H__ */
