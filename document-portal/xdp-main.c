#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "xdp-dbus.h"
#include "xdp-util.h"
#include "flatpak-db.h"
#include "flatpak-dbus.h"
#include "flatpak-utils.h"
#include "flatpak-portal-error.h"
#include "permission-store/permission-store-dbus.h"
#include "xdp-fuse.h"

#include <sys/eventfd.h>

#define TABLE_NAME "documents"

typedef struct
{
  char                  *doc_id;
  int                    fd;
  char                  *owner;
  guint                  flags;

  GDBusMethodInvocation *finish_invocation;
} XdpDocUpdate;


static GMainLoop *loop = NULL;
static FlatpakDb *db = NULL;
static XdgPermissionStore *permission_store;
static int daemon_event_fd = -1;
static int final_exit_status = 0;
static GError *exit_error = NULL;
static dev_t fuse_dev = 0;
static GQueue get_mount_point_invocations = G_QUEUE_INIT;
static XdpDbusDocuments *dbus_api;
FlatpakDir *user_dir = NULL;
FlatpakDir *system_dir = NULL;

G_LOCK_DEFINE (db);

char **
xdp_list_apps (void)
{
  AUTOLOCK (db);
  return flatpak_db_list_apps (db);
}

char **
xdp_list_docs (void)
{
  AUTOLOCK (db);
  return flatpak_db_list_ids (db);
}

FlatpakDbEntry *
xdp_lookup_doc (const char *doc_id)
{
  AUTOLOCK (db);
  return flatpak_db_lookup (db, doc_id);
}

static gboolean
persist_entry (FlatpakDbEntry *entry)
{
  guint32 flags = xdp_entry_get_flags (entry);

  return (flags & XDP_ENTRY_FLAG_TRANSIENT) == 0;
}

static void
do_set_permissions (FlatpakDbEntry    *entry,
                    const char        *doc_id,
                    const char        *app_id,
                    XdpPermissionFlags perms)
{
  g_autofree const char **perms_s = xdg_unparse_permissions (perms);

  g_autoptr(FlatpakDbEntry) new_entry = NULL;

  g_debug ("set_permissions %s %s %x", doc_id, app_id, perms);

  new_entry = flatpak_db_entry_set_app_permissions (entry, app_id, perms_s);
  flatpak_db_set_entry (db, doc_id, new_entry);

  if (persist_entry (new_entry))
    {
      xdg_permission_store_call_set_permission (permission_store,
                                                TABLE_NAME,
                                                FALSE,
                                                doc_id,
                                                app_id,
                                                perms_s,
                                                NULL,
                                                NULL, NULL);
    }
}

static void
portal_grant_permissions (GDBusMethodInvocation *invocation,
                          GVariant              *parameters,
                          const char            *app_id)
{
  const char *target_app_id;
  const char *id;
  g_autofree const char **permissions = NULL;
  XdpPermissionFlags perms;

  g_autoptr(FlatpakDbEntry) entry = NULL;

  g_variant_get (parameters, "(&s&s^a&s)", &id, &target_app_id, &permissions);

  {
    AUTOLOCK (db);

    entry = flatpak_db_lookup (db, id);
    if (entry == NULL)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_FOUND,
                                               "No such document: %s", id);
        return;
      }

    if (!flatpak_is_valid_name (target_app_id))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "Invalid app name: %s", target_app_id);
        return;
      }

    perms = xdp_parse_permissions (permissions);

    /* Must have grant-permissions and all the newly granted permissions */
    if (!xdp_entry_has_permissions (entry, app_id,
                                    XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS | perms))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    do_set_permissions (entry, id, target_app_id,
                        perms | xdp_entry_get_permissions (entry, target_app_id));
  }

  /* Invalidate with lock dropped to avoid deadlock */
  xdp_fuse_invalidate_doc_app (id, target_app_id);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
portal_revoke_permissions (GDBusMethodInvocation *invocation,
                           GVariant              *parameters,
                           const char            *app_id)
{
  const char *target_app_id;
  const char *id;
  g_autofree const char **permissions = NULL;

  g_autoptr(FlatpakDbEntry) entry = NULL;
  XdpPermissionFlags perms;

  g_variant_get (parameters, "(&s&s^a&s)", &id, &target_app_id, &permissions);

  {
    AUTOLOCK (db);

    entry = flatpak_db_lookup (db, id);
    if (entry == NULL)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_FOUND,
                                               "No such document: %s", id);
        return;
      }

    if (!flatpak_is_valid_name (target_app_id))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                                               "Invalid app name: %s", target_app_id);
        return;
      }

    perms = xdp_parse_permissions (permissions);

    /* Must have grant-permissions, or be itself */
    if (!xdp_entry_has_permissions (entry, app_id,
                                    XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS) ||
        strcmp (app_id, target_app_id) == 0)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    do_set_permissions (entry, id, target_app_id,
                        ~perms & xdp_entry_get_permissions (entry, target_app_id));
  }

  /* Invalidate with lock dropped to avoid deadlock */
  xdp_fuse_invalidate_doc_app (id, target_app_id);

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
portal_delete (GDBusMethodInvocation *invocation,
               GVariant              *parameters,
               const char            *app_id)
{
  const char *id;

  g_autoptr(FlatpakDbEntry) entry = NULL;
  g_autofree const char **old_apps = NULL;
  int i;

  g_variant_get (parameters, "(s)", &id);

  {
    AUTOLOCK (db);

    entry = flatpak_db_lookup (db, id);
    if (entry == NULL)
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_FOUND,
                                               "No such document: %s", id);
        return;
      }

    if (!xdp_entry_has_permissions (entry, app_id, XDP_PERMISSION_FLAGS_DELETE))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
                                               "Not enough permissions");
        return;
      }

    g_debug ("delete %s", id);

    flatpak_db_set_entry (db, id, NULL);

    if (persist_entry (entry))
      xdg_permission_store_call_delete (permission_store, TABLE_NAME,
                                        id, NULL, NULL, NULL);
  }

  /* All i/o is done now, so drop the lock so we can invalidate the fuse caches */
  old_apps = flatpak_db_entry_list_apps (entry);
  for (i = 0; old_apps[i] != NULL; i++)
    xdp_fuse_invalidate_doc_app (id, old_apps[i]);
  xdp_fuse_invalidate_doc_app (id, NULL);

  /* Now fuse view is up-to-date, so we can return the call */
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

char *
do_create_doc (struct stat *parent_st_buf, const char *path, gboolean reuse_existing, gboolean persistent)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;
  g_auto(GStrv) ids = NULL;
  char *id = NULL;
  guint32 flags = 0;

  if (!reuse_existing)
    flags |= XDP_ENTRY_FLAG_UNIQUE;
  if (!persistent)
    flags |= XDP_ENTRY_FLAG_TRANSIENT;
  data =
    g_variant_ref_sink (g_variant_new ("(^ayttu)",
                                       path,
                                       (guint64) parent_st_buf->st_dev,
                                       (guint64) parent_st_buf->st_ino,
                                       flags));

  if (reuse_existing)
    {
      ids = flatpak_db_list_ids_by_value (db, data);

      if (ids[0] != NULL)
        return g_strdup (ids[0]);  /* Reuse pre-existing entry with same path */
    }

  while (TRUE)
    {
      g_autoptr(FlatpakDbEntry) existing = NULL;

      g_clear_pointer (&id, g_free);
      id = xdp_name_from_id ((guint32) g_random_int ());
      existing = flatpak_db_lookup (db, id);
      if (existing == NULL)
        break;
    }

  g_debug ("create_doc %s", id);

  entry = flatpak_db_entry_new (data);
  flatpak_db_set_entry (db, id, entry);

  if (persistent)
    {
      xdg_permission_store_call_set (permission_store,
                                     TABLE_NAME,
                                     TRUE,
                                     id,
                                     g_variant_new_array (G_VARIANT_TYPE ("{sas}"), NULL, 0),
                                     g_variant_new_variant (data),
                                     NULL, NULL, NULL);
    }

  return id;
}

static gboolean
validate_fd_common (int fd,
                    struct stat *st_buf,
                    char *path_buffer,
                    GError **error)
{
  g_autofree char *proc_path = NULL;
  ssize_t symlink_size;
  int fd_flags;

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);

  if (fd == -1 ||
      /* Must be able to get fd flags */
      (fd_flags = fcntl (fd, F_GETFL)) == -1 ||
      /* Must be O_PATH */
      ((fd_flags & O_PATH) != O_PATH) ||
      /* Must not be O_NOFOLLOW (because we want the target file) */
      ((fd_flags & O_NOFOLLOW) == O_NOFOLLOW) ||
      /* Must be able to fstat */
      fstat (fd, st_buf) < 0 ||
      /* Must be a regular file */
      (st_buf->st_mode & S_IFMT) != S_IFREG ||
      /* Must be able to read path from /proc/self/fd */
      /* This is an absolute and (at least at open time) symlink-expanded path */
      (symlink_size = readlink (proc_path, path_buffer, PATH_MAX)) < 0)
    {
      g_set_error (error,
                   FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid fd passed");
      return FALSE;
    }

  path_buffer[symlink_size] = 0;
  return TRUE;
}

static gboolean
validate_parent_dir (const char *path,
                     struct stat *st_buf,
                     struct stat *real_parent_st_buf,
                     GError **error)
{
  g_autofree char *dirname = NULL;
  g_autofree char *name = NULL;
  glnx_fd_close int dir_fd = -1;
  struct stat real_st_buf;

  /* We open the parent directory and do the stat in that, so that we have
   * trustworthy parent dev/ino for later verification. Otherwise the caller
   * could later replace a parent with a symlink and make us read some other file
   */
  dirname = g_path_get_dirname (path);
  name = g_path_get_basename (path);
  dir_fd = open (dirname, O_CLOEXEC | O_PATH);

  if (fstat (dir_fd, real_parent_st_buf) < 0 ||
      fstatat (dir_fd, name, &real_st_buf, AT_SYMLINK_NOFOLLOW) < 0 ||
      st_buf->st_dev != real_st_buf.st_dev ||
      st_buf->st_ino != real_st_buf.st_ino)
    {
      /* Don't leak any info about real file path existence, etc */
      g_set_error (error,
                   FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Invalid fd passed");
      return FALSE;
    }

  return TRUE;
}

static gboolean
validate_fd (int fd,
             GKeyFile *app_info,
             struct stat *st_buf,
             struct stat *real_parent_st_buf,
             char *path_buffer,
             GError **error)
{
  g_autofree char *app_path = NULL;
  g_autofree char *runtime_path = NULL;

  if (!validate_fd_common (fd, st_buf, path_buffer, error))
    return FALSE;

  /* For apps we translate /app and /usr to the installed locations.
     Also, we need to rewrite to drop the /newroot prefix added by
     bubblewrap for other files to work. */
  app_path = g_key_file_get_string (app_info, "Instance", "app-path", NULL);
  runtime_path = g_key_file_get_string (app_info, "Instance", "runtime-path", NULL);
  if (app_path != NULL || runtime_path != NULL)
    {
      if (app_path != NULL &&
          g_str_has_prefix (path_buffer, "/newroot/app/"))
        {
          char *rel_path = path_buffer + strlen ("/newroot/app/");
          g_autofree char *real_path = g_build_filename (app_path, rel_path, NULL);
          strncpy (path_buffer, real_path, PATH_MAX);
        }
      else if (runtime_path != NULL &&
               g_str_has_prefix (path_buffer, "/newroot/usr/"))
        {
          char *rel_path = path_buffer + strlen ("/newroot/usr/");
          g_autofree char *real_path = g_build_filename (runtime_path, rel_path, NULL);
          strncpy (path_buffer, real_path, PATH_MAX);
        }
      else if (g_str_has_prefix (path_buffer, "/newroot/"))
        {
          char *rel_path = path_buffer + strlen ("/newroot");
          g_strlcpy (path_buffer, rel_path, PATH_MAX);
        }
    }

  if (!validate_parent_dir (path_buffer, st_buf, real_parent_st_buf, error))
    return FALSE;

  return TRUE;
}

static void
portal_add (GDBusMethodInvocation *invocation,
            GVariant              *parameters,
            const char            *app_id)
{
  GDBusMessage *message;
  GUnixFDList *fd_list;
  g_autofree char *id = NULL;
  int fd_id, fd, fds_len;
  char path_buffer[PATH_MAX + 1];
  const int *fds;
  struct stat st_buf, real_parent_st_buf;
  gboolean reuse_existing, persistent;
  GError *error = NULL;
  GKeyFile *app_info = g_object_get_data (G_OBJECT (invocation), "app-info");

  g_variant_get (parameters, "(hbb)", &fd_id, &reuse_existing, &persistent);

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  fd = -1;
  if (fd_list != NULL)
    {
      fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (fd_id < fds_len)
        fd = fds[fd_id];
    }

  if (!validate_fd (fd, app_info, &st_buf, &real_parent_st_buf, path_buffer, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return;
    }

  if (st_buf.st_dev == fuse_dev)
    {
      /* The passed in fd is on the fuse filesystem itself */
      g_autoptr(FlatpakDbEntry) old_entry = NULL;

      id = xdp_fuse_lookup_id_for_inode (st_buf.st_ino);
      g_debug ("path on fuse, id %s", id);
      if (id == NULL)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 "Invalid fd passed");
          return;
        }

      /* Don't lock the db before doing the fuse call above, because it takes takes a lock
         that can block something calling back, causing a deadlock on the db lock */

      AUTOLOCK (db);

      /* If the entry doesn't exist anymore, fail.  Also fail if not
       * reuse_existing, because otherwise the user could use this to
       * get a copy with permissions and thus escape later permission
       * revocations
       */
      old_entry = flatpak_db_lookup (db, id);
      if (old_entry == NULL ||
          !reuse_existing)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                                                 "Invalid fd passed");
          return;
        }
    }
  else
    {
      {
        AUTOLOCK (db);

        id = do_create_doc (&real_parent_st_buf, path_buffer, reuse_existing, persistent);

        if (app_id[0] != '\0')
          {
            g_autoptr(FlatpakDbEntry) entry = NULL;
            XdpPermissionFlags perms =
              XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS |
              XDP_PERMISSION_FLAGS_READ |
              XDP_PERMISSION_FLAGS_WRITE;
            {
              entry = flatpak_db_lookup (db, id);

              /* If its a unique one its safe for the creator to
                 delete it at will */
              if (!reuse_existing)
                perms |= XDP_PERMISSION_FLAGS_DELETE;

              do_set_permissions (entry, id, app_id, perms);
            }
          }
      }

      /* Invalidate with lock dropped to avoid deadlock */
      xdp_fuse_invalidate_doc_app (id, NULL);
      if (app_id[0] != '\0')
        xdp_fuse_invalidate_doc_app (id, app_id);
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", id));
}

static void
portal_add_named (GDBusMethodInvocation *invocation,
                  GVariant              *parameters,
                  const char            *app_id)
{
  GDBusMessage *message;
  GUnixFDList *fd_list;
  g_autofree char *id = NULL;
  int parent_fd_id, parent_fd, fds_len;
  const int *fds;
  char parent_path_buffer[PATH_MAX + 1];
  g_autofree char *path = NULL;
  struct stat parent_st_buf;
  const char *filename;
  gboolean reuse_existing, persistent;
  g_autoptr(GError) error = NULL;

  g_autoptr(GVariant) filename_v = NULL;

  g_variant_get (parameters, "(h@aybb)", &parent_fd_id, &filename_v, &reuse_existing, &persistent);
  filename = g_variant_get_bytestring (filename_v);

  /* This is only allowed from the host, or else we could leak existence of files */
  if (*app_id != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  parent_fd = -1;
  if (fd_list != NULL)
    {
      fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (parent_fd_id < fds_len)
        parent_fd = fds[parent_fd_id];
    }

  if (strchr (filename, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid filename passed");
      return;
    }

  if (!validate_fd_common (parent_fd, &parent_st_buf, parent_path_buffer, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  if (parent_st_buf.st_dev == fuse_dev)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  path = g_build_filename (parent_path_buffer, filename, NULL);

  g_debug ("portal_add_named %s", path);

  AUTOLOCK (db);

  id = do_create_doc (&parent_st_buf, path, reuse_existing, persistent);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", id));
}


typedef void (*PortalMethod) (GDBusMethodInvocation *invocation,
                              GVariant              *parameters,
                              const char            *app_id);

static void
got_app_id_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (source_object);

  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) app_info = NULL;
  g_autofree char *app_id = NULL;
  PortalMethod portal_method = user_data;

  app_info = flatpak_invocation_lookup_app_info_finish (invocation, res, &error);
  if (app_info != NULL)
    app_id = g_key_file_get_string (app_info, "Application", "name", &error);

  if (app_id == NULL)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    {
      g_object_set_data_full (G_OBJECT (invocation), "app-info", g_steal_pointer (&app_info), (GDestroyNotify)g_key_file_unref);
      portal_method (invocation, g_dbus_method_invocation_get_parameters (invocation), app_id);
    }
}

static gboolean
handle_method (GCallback              method_callback,
               GDBusMethodInvocation *invocation)
{
  flatpak_invocation_lookup_app_info (invocation, NULL, got_app_id_cb, method_callback);

  return TRUE;
}

static gboolean
handle_get_mount_point (XdpDbusDocuments *object, GDBusMethodInvocation *invocation)
{
  if (fuse_dev == 0)
    {
      /* We mustn't reply to this until the FUSE mount point is open for
       * business. */
      g_queue_push_tail (&get_mount_point_invocations, g_object_ref (invocation));
      return TRUE;
    }

  xdp_dbus_documents_complete_get_mount_point (object, invocation, xdp_fuse_get_mountpoint ());
  return TRUE;
}

static gboolean
portal_lookup (GDBusMethodInvocation *invocation,
               GVariant *parameters,
               const char *app_id)
{
  const char *filename;
  char path_buffer[PATH_MAX + 1];
  int fd;
  struct stat st_buf, real_parent_st_buf;
  g_auto(GStrv) ids = NULL;
  g_autofree char *id = NULL;
  GError *error = NULL;
  GKeyFile *app_info = g_object_get_data (G_OBJECT (invocation), "app-info");

  if (strcmp (app_id, "") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed in sandbox");
      return TRUE;
    }

  g_variant_get (parameters, "(^&ay)", &filename);

  fd = open (filename, O_PATH | O_CLOEXEC);
  if (fd == -1)
    {
      glnx_set_error_from_errno (&error);
      g_dbus_method_invocation_take_error (invocation, error);
      return TRUE;
    }

  if (!validate_fd (fd, app_info, &st_buf, &real_parent_st_buf, path_buffer, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      return TRUE;
    }

  if (st_buf.st_dev == fuse_dev)
    {
      /* The passed in fd is on the fuse filesystem itself */
      id = xdp_fuse_lookup_id_for_inode (st_buf.st_ino);
      g_debug ("path on fuse, id %s", id);
    }
  else
    {
      g_autoptr(GVariant) data = NULL;

      data = g_variant_ref_sink (g_variant_new ("(^ayttu)",
                                                path_buffer,
                                                (guint64)real_parent_st_buf.st_dev,
                                                (guint64)real_parent_st_buf.st_ino,
                                                0));
      ids = flatpak_db_list_ids_by_value (db, data);
      if (ids[0] != NULL)
        id = g_strdup (ids[0]);
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", id ? id : ""));

  return TRUE;
}

static GVariant *
get_app_permissions (FlatpakDbEntry *entry)
{
  g_autofree const char **apps = NULL;
  GVariantBuilder builder;
  int i;

  apps = flatpak_db_entry_list_apps (entry);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));

  for (i = 0; apps[i] != NULL; i++)
    {
      g_autofree const char **permissions = flatpak_db_entry_list_permissions (entry, apps[i]);
      g_variant_builder_add_value (&builder,
                                   g_variant_new ("{s^as}", apps[i], permissions));
    }

  return g_variant_builder_end (&builder);
}

static GVariant *
get_path (FlatpakDbEntry *entry)
{
  g_autoptr (GVariant) data = flatpak_db_entry_get_data (entry);
  const char *path;

  g_variant_get (data, "(^ayttu)", &path, NULL, NULL, NULL);
  return g_variant_new_bytestring (path);
}

static gboolean
portal_info (GDBusMethodInvocation *invocation,
             GVariant *parameters,
             const char *app_id)
{
  const char *id = NULL;
  g_autoptr(FlatpakDbEntry) entry = NULL;

  if (strcmp (app_id, "") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed in sandbox");
      return TRUE;
    }

  g_variant_get (parameters, "(&s)", &id);

  AUTOLOCK (db);

  entry = flatpak_db_lookup (db, id);

  if (!entry)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
                                             "Invalid ID passed");
      return TRUE;
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@ay@a{sas})",
                                                        get_path (entry),
                                                        get_app_permissions (entry)));

  return TRUE;
}

static gboolean
portal_list (GDBusMethodInvocation *invocation,
             GVariant *parameters,
             const char *app_id)
{
  g_auto(GStrv) ids = NULL;
  GVariantBuilder builder;
  int i;

  if (strcmp (app_id, "") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
                                             "Not allowed in sandbox");
      return TRUE;
    }

  g_variant_get (parameters, "(&s)", &app_id);

  AUTOLOCK (db);

  if (strcmp (app_id, "") == 0)
    ids = flatpak_db_list_ids (db);
  else
    ids = flatpak_db_list_ids_by_app (db, app_id);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{say}"));
  for (i = 0; ids[i]; i++)
    {
      g_autoptr(FlatpakDbEntry) entry = NULL;

      entry = flatpak_db_lookup (db, ids[i]);

      g_variant_builder_add (&builder, "{s@ay}", ids[i], get_path (entry));
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(@a{say})",
                                                        g_variant_builder_end (&builder)));

  return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  dbus_api = xdp_dbus_documents_skeleton_new ();

  g_signal_connect_swapped (dbus_api, "handle-get-mount-point", G_CALLBACK (handle_get_mount_point), NULL);
  g_signal_connect_swapped (dbus_api, "handle-add", G_CALLBACK (handle_method), portal_add);
  g_signal_connect_swapped (dbus_api, "handle-add-named", G_CALLBACK (handle_method), portal_add_named);
  g_signal_connect_swapped (dbus_api, "handle-grant-permissions", G_CALLBACK (handle_method), portal_grant_permissions);
  g_signal_connect_swapped (dbus_api, "handle-revoke-permissions", G_CALLBACK (handle_method), portal_revoke_permissions);
  g_signal_connect_swapped (dbus_api, "handle-delete", G_CALLBACK (handle_method), portal_delete);
  g_signal_connect_swapped (dbus_api, "handle-lookup", G_CALLBACK (handle_method), portal_lookup);
  g_signal_connect_swapped (dbus_api, "handle-info", G_CALLBACK (handle_method), portal_info);
  g_signal_connect_swapped (dbus_api, "handle-list", G_CALLBACK (handle_method), portal_list);

  flatpak_connection_track_name_owners (connection);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (dbus_api),
                                         connection,
                                         "/org/freedesktop/portal/documents",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }
}

static void
daemon_report_done (int status)
{
  if (daemon_event_fd != -1)
    {
      guint64 counter;

      counter = status + 1;
      if (write (daemon_event_fd, &counter, sizeof (counter)) < 0)
          g_critical ("Unable to report exit status: %s", g_strerror (errno));

      daemon_event_fd = -1;
    }
}

static void
do_exit (int status)
{
  daemon_report_done (status);
  exit (status);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  struct stat stbuf;
  gpointer invocation;

  g_debug ("%s acquired", name);

  if (!xdp_fuse_init (&exit_error))
    {
      final_exit_status = 6;
      g_printerr ("fuse init failed: %s", exit_error->message);
      g_main_loop_quit (loop);
      return;
    }

  if (stat (xdp_fuse_get_mountpoint (), &stbuf) != 0)
    {
      g_set_error (&exit_error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "fuse stat failed: %s", strerror (errno));
      final_exit_status = 7;
      g_printerr ("fuse stat failed: %s", strerror (errno));
      g_main_loop_quit (loop);
      return;
    }

  fuse_dev = stbuf.st_dev;

  while ((invocation = g_queue_pop_head (&get_mount_point_invocations)) != NULL)
    {
      xdp_dbus_documents_complete_get_mount_point (dbus_api, invocation, xdp_fuse_get_mountpoint ());
      g_object_unref (invocation);
    }

  daemon_report_done (0);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("%s lost", name);
  final_exit_status = 20;
  g_set_error (&exit_error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "D-Bus name \"%s\" lost", name);
  g_main_loop_quit (loop);
}

static void
exit_handler (int sig)
{
  /* We cannot set exit_error here, because malloc() in a signal handler
   * is undefined behaviour. Rely on main() coping gracefully with
   * that. */
  g_main_loop_quit (loop);
}

static void
session_bus_closed (GDBusConnection *connection,
                    gboolean         remote_peer_vanished,
                    GError          *bus_error)
{
  g_set_error (&exit_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE, "Disconnected from session bus");
  g_main_loop_quit (loop);
}

static int
set_one_signal_handler (int    sig,
                        void (*handler)(int),
                        int    remove)
{
  struct sigaction sa;
  struct sigaction old_sa;

  memset (&sa, 0, sizeof (struct sigaction));
  sa.sa_handler = remove ? SIG_DFL : handler;
  sigemptyset (&(sa.sa_mask));
  sa.sa_flags = 0;

  if (sigaction (sig, NULL, &old_sa) == -1)
    {
      g_warning ("cannot get old signal handler");
      return -1;
    }

  if (old_sa.sa_handler == (remove ? handler : SIG_DFL) &&
      sigaction (sig, &sa, NULL) == -1)
    {
      g_warning ("cannot set signal handler");
      return -1;
    }

  return 0;
}

static gboolean opt_verbose;
static gboolean opt_daemon;
static gboolean opt_replace;
static gboolean opt_version;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information", NULL },
  { "daemon", 'd', 0, G_OPTION_ARG_NONE, &opt_daemon, "Run in background", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version and exit", NULL },
  { NULL }
};

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    printf ("XDP: %s\n", message);
  else
    printf ("%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const gchar *string)
{
  int is_tty = isatty (1);
  const char *prefix = "";
  const char *suffix = "";
  if (is_tty)
    {
      prefix = "\x1b[31m\x1b[1m"; /* red, bold */
      suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
    }
  fprintf (stderr, "%serror: %s%s\n", prefix, suffix, string);
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;

  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  GDBusConnection *session_bus;
  GOptionContext *context;
  GDBusMethodInvocation *invocation;

  setlocale (LC_ALL, "");

  /* Avoid even loading gvfs to avoid accidental confusion */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  flatpak_migrate_from_xdg_app ();

  g_set_printerr_handler (printerr_handler);

  context = g_option_context_new ("- document portal");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s", error->message);
      return 1;
    }

  if (opt_version)
    {
      g_print ("%s\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  if (opt_daemon)
    {
      pid_t pid;
      ssize_t read_res;

      daemon_event_fd = eventfd (0, EFD_CLOEXEC);
      pid = fork ();
      if (pid != 0)
        {
          guint64 counter;

          read_res = read (daemon_event_fd, &counter, sizeof (counter));
          if (read_res != 8)
            exit (1);
          exit (counter - 1);
        }
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  loop = g_main_loop_new (NULL, FALSE);

  path = g_build_filename (g_get_user_data_dir (), "flatpak/db", TABLE_NAME, NULL);
  db = flatpak_db_new (path, FALSE, &error);
  if (db == NULL)
    {
      g_printerr ("Failed to load db: %s", error->message);
      do_exit (2);
    }

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s", error->message);
      do_exit (3);
    }

  permission_store = xdg_permission_store_proxy_new_sync (session_bus, G_DBUS_PROXY_FLAGS_NONE,
                                                          "org.freedesktop.impl.portal.PermissionStore",
                                                          "/org/freedesktop/impl/portal/PermissionStore",
                                                          NULL, &error);
  if (permission_store == NULL)
    {
      g_print ("No permission store: %s", error->message);
      do_exit (4);
    }

  user_dir = flatpak_dir_get_user ();
  system_dir = flatpak_dir_get_system ();

  /* We want do do our custom post-mainloop exit */
  g_dbus_connection_set_exit_on_close (session_bus, FALSE);

  g_signal_connect (session_bus, "closed", G_CALLBACK (session_bus_closed), NULL);

  if (set_one_signal_handler (SIGHUP, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGINT, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGTERM, exit_handler, 0) == -1 ||
      set_one_signal_handler (SIGPIPE, SIG_IGN, 0) == -1)
    do_exit (5);

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.portal.Documents",
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  g_main_loop_run (loop);

  while ((invocation = g_queue_pop_head (&get_mount_point_invocations)) != NULL)
    {
      if (exit_error != NULL)
        g_dbus_method_invocation_return_gerror (invocation, exit_error);
      else
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Terminated");

      g_object_unref (invocation);
    }

  xdp_fuse_exit ();

  g_bus_unown_name (owner_id);

  do_exit (final_exit_status);

  return 0;
}
