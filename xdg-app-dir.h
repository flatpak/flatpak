#ifndef __XDG_APP_DIR_H__
#define __XDG_APP_DIR_H__

#include <ostree.h>

typedef struct XdgAppDir XdgAppDir;

#define XDG_APP_TYPE_DIR xdg_app_dir_get_type()
#define XDG_APP_DIR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_DIR, XdgAppDir))
#define XDG_APP_IS_DIR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_DIR))

GType xdg_app_dir_get_type (void);

GFile *  xdg_app_get_system_base_dir_location (void);
GFile *  xdg_app_get_user_base_dir_location   (void);

XdgAppDir*  xdg_app_dir_new            (GFile         *basedir,
                                        gboolean       user);
XdgAppDir  *xdg_app_dir_get            (gboolean       user);
XdgAppDir  *xdg_app_dir_get_system     (void);
XdgAppDir  *xdg_app_dir_get_user       (void);
gboolean    xdg_app_dir_is_user        (XdgAppDir     *self);
GFile *     xdg_app_dir_get_path       (XdgAppDir     *self);
GFile *     xdg_app_dir_get_deploy_dir (XdgAppDir     *self,
                                        const char    *ref);
OstreeRepo *xdg_app_dir_get_repo       (XdgAppDir     *self);
gboolean    xdg_app_dir_ensure_path    (XdgAppDir     *self,
                                        GCancellable  *cancellable,
                                        GError       **error);
gboolean    xdg_app_dir_ensure_repo    (XdgAppDir     *self,
                                        GCancellable  *cancellable,
                                        GError       **error);

#endif /* __XDG_APP_DIR_H__ */
