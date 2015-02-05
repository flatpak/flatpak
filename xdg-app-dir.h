#ifndef __XDG_APP_DIR_H__
#define __XDG_APP_DIR_H__

#include <ostree.h>

typedef struct XdgAppDir XdgAppDir;

#define XDG_APP_TYPE_DIR xdg_app_dir_get_type()
#define XDG_APP_DIR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_DIR, XdgAppDir))
#define XDG_APP_IS_DIR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_DIR))

GType xdg_app_dir_get_type (void);

#define XDG_APP_DIR_ERROR xdg_app_dir_error_quark()

typedef enum {
  XDG_APP_DIR_ERROR_ALREADY_DEPLOYED,
  XDG_APP_DIR_ERROR_ALREADY_UNDEPLOYED,
} XdgAppErrorEnum;

GQuark       xdg_app_dir_error_quark      (void);

GFile *  xdg_app_get_system_base_dir_location (void);
GFile *  xdg_app_get_user_base_dir_location   (void);

XdgAppDir*  xdg_app_dir_new             (GFile          *basedir,
                                         gboolean        user);
XdgAppDir  *xdg_app_dir_get             (gboolean        user);
XdgAppDir  *xdg_app_dir_get_system      (void);
XdgAppDir  *xdg_app_dir_get_user        (void);
gboolean    xdg_app_dir_is_user         (XdgAppDir      *self);
GFile *     xdg_app_dir_get_path        (XdgAppDir      *self);
GFile *     xdg_app_dir_get_deploy_dir  (XdgAppDir      *self,
                                         const char     *ref);
GFile *     xdg_app_dir_get_exports_dir (XdgAppDir      *self);
GFile *     xdg_app_dir_get_removed_dir (XdgAppDir      *self);
GFile *     xdg_app_dir_get_if_deployed (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         GCancellable   *cancellable);
GFile *     xdg_app_dir_get_app_data    (XdgAppDir      *self,
                                         const char     *app);
OstreeRepo *xdg_app_dir_get_repo        (XdgAppDir      *self);
gboolean    xdg_app_dir_ensure_path     (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_ensure_repo     (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_pull            (XdgAppDir      *self,
                                         const char     *repository,
                                         const char     *ref,
                                         GCancellable   *cancellable,
                                         GError        **error);
char *      xdg_app_dir_read_active     (XdgAppDir      *self,
                                         const char     *ref,
                                         GCancellable   *cancellable);
gboolean    xdg_app_dir_set_active      (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_list_deployed   (XdgAppDir      *self,
                                         const char     *ref,
                                         char         ***deployed_checksums,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_deploy          (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_undeploy        (XdgAppDir      *self,
                                         const char     *ref,
                                         const char     *checksum,
					 gboolean        force_remove,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_prune           (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_cleanup_removed (XdgAppDir      *self,
                                         GCancellable   *cancellable,
                                         GError        **error);
gboolean    xdg_app_dir_collect_deployed_refs (XdgAppDir *self,
					       const char *type,
					       const char *name_prefix,
					       const char *branch,
					       const char *arch,
					       GHashTable *hash,
					       GCancellable *cancellable,
					       GError **error);





#endif /* __XDG_APP_DIR_H__ */
