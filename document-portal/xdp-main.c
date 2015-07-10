#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "xdp-dbus.h"
#include "xdp-doc-db.h"
#include "xdp-error.h"
#include "xdp-util.h"
#include "xdp-fuse.h"

typedef struct
{
  char *doc_id;
  int fd;
  char *owner;
  guint flags;

  GDBusMethodInvocation *finish_invocation;
} XdpDocUpdate;


static GMainLoop *loop = NULL;
static XdpDocDb *db = NULL;
static GDBusNodeInfo *introspection_data = NULL;

static guint save_timeout = 0;

static gboolean
queue_db_save_timeout (gpointer user_data)
{
  g_autoptr(GError) error = NULL;

  save_timeout = 0;

  if (xdp_doc_db_is_dirty (db))
    {
      if (!xdp_doc_db_save (db, &error))
        g_warning ("db save: %s\n", error->message);
    }

  return FALSE;
}

static void
queue_db_save (void)
{
  if (save_timeout != 0)
    return;

  if (xdp_doc_db_is_dirty (db))
    save_timeout = g_timeout_add_seconds (10, queue_db_save_timeout, NULL);
}

static XdpPermissionFlags
parse_permissions (const char **permissions, GError **error)
{
  XdpPermissionFlags perms;
  int i;

  perms = 0;
  for (i = 0; permissions[i]; i++)
    {
      if (strcmp (permissions[i], "read") == 0)
        perms |= XDP_PERMISSION_FLAGS_READ;
      else if (strcmp (permissions[i], "write") == 0)
        perms |= XDP_PERMISSION_FLAGS_WRITE;
      else if (strcmp (permissions[i], "grant-permissions") == 0)
        perms |= XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS;
      else if (strcmp (permissions[i], "delete") == 0)
        perms |= XDP_PERMISSION_FLAGS_DELETE;
      else
        {
          g_set_error (error, XDP_ERROR, XDP_ERROR_INVALID_ARGUMENT,
                       "No such permission: %s", permissions[i]);
          return -1;
        }
    }

  return perms;
}

static void
portal_grant_permissions (GDBusMethodInvocation *invocation,
                          GVariant *parameters,
                          const char *app_id)
{
  const char *target_app_id;
  guint32 doc_id;
  g_autoptr(GVariant) doc = NULL;
  g_autoptr(GError) error = NULL;
  const char **permissions;
  XdpPermissionFlags perms;

  g_variant_get (parameters, "(u&s^a&s)", &doc_id, &target_app_id, &permissions);

  doc = xdp_doc_db_lookup_doc (db, doc_id);
  if (doc == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, XDP_ERROR, XDP_ERROR_NOT_FOUND,
                                             "No such document: %d", doc_id);
      return;
    }
  
  perms = parse_permissions (permissions, &error);
  if (perms == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  /* Must have grant-permissions and all the newly granted permissions */
  if (!xdp_doc_has_permissions (doc, app_id, XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS | perms))
    {
      g_dbus_method_invocation_return_error (invocation, XDP_ERROR, XDP_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }

  xdp_doc_db_set_permissions (db, doc_id, target_app_id, perms, TRUE);
  queue_db_save ();

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));

}

static void
portal_revoke_permissions (GDBusMethodInvocation *invocation,
                           GVariant *parameters,
                           const char *app_id)
{
  const char *target_app_id;
  g_autoptr(GVariant) doc = NULL;
  g_autoptr(GError) error = NULL;
  guint32 doc_id;
  const char **permissions;
  XdpPermissionFlags perms;

  g_variant_get (parameters, "(u&s^a&s)", &doc_id, &target_app_id, &permissions);

  doc = xdp_doc_db_lookup_doc (db, doc_id);
  if (doc == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, XDP_ERROR, XDP_ERROR_NOT_FOUND,
                                             "No such document: %d", doc_id);
      return;
    }

  perms = parse_permissions (permissions, &error);
  if (perms == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return;
    }

  /* Must have grant-permissions, or be itself */
  if (!xdp_doc_has_permissions (doc, app_id, XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS) ||
      strcmp (app_id, target_app_id) == 0)
    {
      g_dbus_method_invocation_return_error (invocation, XDP_ERROR, XDP_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }

  xdp_doc_db_set_permissions (db, doc_id, target_app_id,
                              xdp_doc_get_permissions (doc, target_app_id) & ~perms,
                              FALSE);
  queue_db_save ();

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
portal_delete (GDBusMethodInvocation *invocation,
               GVariant *parameters,
               const char *app_id)
{
  guint32 doc_id;
  g_autoptr(GVariant) doc = NULL;

  g_variant_get (parameters, "(u)", &doc_id);

  doc = xdp_doc_db_lookup_doc (db, doc_id);
  if (doc == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, XDP_ERROR, XDP_ERROR_NOT_FOUND,
                                             "No such document: %d", doc_id);
      return;
    }

  if (!xdp_doc_has_permissions (doc, app_id, XDP_PERMISSION_FLAGS_DELETE))
    {
      g_dbus_method_invocation_return_error (invocation, XDP_ERROR, XDP_ERROR_NOT_ALLOWED,
                                             "Not enough permissions");
      return;
    }
  
  xdp_doc_db_delete_doc (db, doc_id);
  queue_db_save ();

  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static void
portal_add (GDBusMethodInvocation *invocation,
            GVariant *parameters,
            const char *app_id)
{
  guint32 id;
  const char *uri;

  if (app_id[0] != '\0')
    {
      /* don't allow this from within the sandbox */
      g_dbus_method_invocation_return_error (invocation,
                                             XDP_ERROR, XDP_ERROR_NOT_ALLOWED,
                                             "Not allowed inside sandbox");
      return;
    }

  g_variant_get (parameters, "(&s)", &uri);

  id = xdp_doc_db_create_doc (db, uri);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(u)", id));
  queue_db_save ();
}

static void
portal_add_local (GDBusMethodInvocation *invocation,
                  GVariant *parameters,
                  const char *app_id)
{
  GDBusMessage *message;
  GUnixFDList *fd_list;
  guint32 id;
  g_autofree char *proc_path = NULL;
  g_autofree char *uri = NULL;
  int fd_id, fd, fds_len, fd_flags;
  const int *fds;
  char path_buffer[PATH_MAX+1];
  g_autoptr(GFile) file = NULL;
  ssize_t symlink_size;
  struct stat st_buf, real_st_buf;

  g_variant_get (parameters, "(h)", &fd_id);

  message = g_dbus_method_invocation_get_message (invocation);
  fd_list = g_dbus_message_get_unix_fd_list (message);

  fd = -1;
  if (fd_list != NULL)
    {
      fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);
      if (fd_id < fds_len)
        fd = fds[fd_id];
    }

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);

  if (fd == -1 ||
      /* Must be able to fstat */
      fstat (fd, &st_buf) < 0 ||
      /* Must be a regular file */
      (st_buf.st_mode & S_IFMT) != S_IFREG ||
      /* Must be able to get fd flags */
      (fd_flags = fcntl (fd, F_GETFL)) == -1 ||
      /* Must be able to read */
      (fd_flags & O_ACCMODE) == O_WRONLY ||
      /* Must be able to read path from /proc/self/fd */
      (symlink_size = readlink (proc_path, path_buffer, sizeof (path_buffer) - 1)) < 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDP_ERROR, XDP_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  path_buffer[symlink_size] = 0;

  if (lstat (path_buffer, &real_st_buf) < 0 ||
      st_buf.st_dev != real_st_buf.st_dev ||
      st_buf.st_ino != real_st_buf.st_ino)
    {
      /* Don't leak any info about real file path existance, etc */
      g_dbus_method_invocation_return_error (invocation,
                                             XDP_ERROR, XDP_ERROR_INVALID_ARGUMENT,
                                             "Invalid fd passed");
      return;
    }

  file = g_file_new_for_path (path_buffer);
  uri = g_file_get_uri (file);

  id = xdp_doc_db_create_doc (db, uri);

  if (app_id[0] != '\0')
    {
      /* also grant app-id perms based on file_mode */
      guint32 perms = XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS | XDP_PERMISSION_FLAGS_READ;
      if ((fd_flags & O_ACCMODE) == O_RDWR)
        perms |= XDP_PERMISSION_FLAGS_WRITE;
      xdp_doc_db_set_permissions (db, id, app_id, perms, TRUE);
    }

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(u)", id));
  queue_db_save ();
}

typedef void (*PortalMethod) (GDBusMethodInvocation *invocation,
                              GVariant *parameters,
                              const char *app_id);

static void
got_app_id_cb (GObject *source_object,
               GAsyncResult *res,
               gpointer user_data)
{
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (source_object);
  g_autoptr(GError) error = NULL;
  g_autofree char *app_id = NULL;
  PortalMethod portal_method = user_data;

  app_id = xdp_invocation_lookup_app_id_finish (invocation, res, &error);

  if (app_id == NULL)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    portal_method (invocation, g_dbus_method_invocation_get_parameters (invocation), app_id);
}

static gboolean
handle_method (GCallback method_callback,
               GDBusMethodInvocation *invocation)
{
  xdp_invocation_lookup_app_id (invocation, NULL, got_app_id_cb, method_callback);

  return TRUE;
}

static gboolean
handle_get_mount_point (XdpDbusDocuments *object, GDBusMethodInvocation *invocation)
{
  xdp_dbus_documents_complete_get_mount_point (object, invocation, xdp_fuse_get_mountpoint ());
  return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  XdpDbusDocuments *helper;
  GError *error = NULL;

  helper = xdp_dbus_documents_skeleton_new ();

  g_signal_connect_swapped (helper, "handle-get-mount-point", G_CALLBACK (handle_get_mount_point), NULL);
  g_signal_connect_swapped (helper, "handle-add", G_CALLBACK (handle_method), portal_add);
  g_signal_connect_swapped (helper, "handle-add-local", G_CALLBACK (handle_method), portal_add_local);
  g_signal_connect_swapped (helper, "handle-grant-permissions", G_CALLBACK (handle_method), portal_grant_permissions);
  g_signal_connect_swapped (helper, "handle-revoke-permissions", G_CALLBACK (handle_method), portal_revoke_permissions);
  g_signal_connect_swapped (helper, "handle-delete", G_CALLBACK (handle_method), portal_delete);

  xdp_connection_track_name_owners (connection);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         "/org/freedesktop/portal/documents",
                                         &error))
    {
      g_warning ("error: %s\n", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_autoptr(GError) error = NULL;
  if (!xdp_fuse_init (db, &error))
    {
      g_printerr ("fuse init failed: %s\n", error->message);
      exit (1);
    }

}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_main_loop_quit (loop);
}

static void
session_bus_closed (GDBusConnection *connection,
                    gboolean         remote_peer_vanished,
                    GError          *bus_error)
{
  g_main_loop_quit (loop);
}

static void
exit_handler (int sig)
{
  g_main_loop_quit (loop);
}

static int
set_one_signal_handler (int sig,
                        void (*handler)(int),
                        int remove)
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

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { NULL }
};

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("XDP: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;
  GBytes *introspection_bytes;
  g_autoptr(GList) object_types = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *data_path = NULL;
  g_autofree char *db_path = NULL;
  g_autoptr(GFile) data_dir = NULL;
  g_autoptr(GFile) db_file = NULL;
  GDBusConnection  *session_bus;
  GOptionContext *context;

  setlocale (LC_ALL, "");

  /* Avoid even loading gvfs to avoid accidental confusion */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  context = g_option_context_new ("- document portal");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_print ("option parsing failed: %s\n", error->message);
      return 1;
    }

  if (opt_verbose)
    g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  loop = g_main_loop_new (NULL, FALSE);

  data_path = g_build_filename (g_get_user_data_dir(), "xdg-document-portal", NULL);
  if (g_mkdir_with_parents  (data_path, 0700))
    {
      g_printerr ("Unable to create dir %s\n", data_path);
      return 1;
    }

  data_dir = g_file_new_for_path (data_path);
  db_file = g_file_get_child (data_dir, "main.gvdb");
  db_path = g_file_get_path (db_file);

  db = xdp_doc_db_new (db_path, &error);
  if (db == NULL)
    {
      g_print ("%s\n", error->message);
      return 2;
    }

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_print ("No session bus: %s\n", error->message);
      return 3;
    }

  /* We want do do our custom post-mainloop exit */
  g_dbus_connection_set_exit_on_close (session_bus, FALSE);

  g_signal_connect (session_bus, "closed", G_CALLBACK (session_bus_closed), NULL);

  if (set_one_signal_handler(SIGHUP, exit_handler, 0) == -1 ||
      set_one_signal_handler(SIGINT, exit_handler, 0) == -1 ||
      set_one_signal_handler(SIGTERM, exit_handler, 0) == -1 ||
      set_one_signal_handler(SIGPIPE, SIG_IGN, 0) == -1)
    return -1;

  introspection_bytes = g_resources_lookup_data ("/org/freedesktop/portal/Documents/org.freedesktop.portal.documents.xml", 0, NULL);
  g_assert (introspection_bytes != NULL);

  introspection_data = g_dbus_node_info_new_for_xml (g_bytes_get_data (introspection_bytes, NULL), NULL);

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.portal.Documents",
                             G_BUS_NAME_OWNER_FLAGS_NONE,
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  g_main_loop_run (loop);

  if (xdp_doc_db_is_dirty (db))
    {
      g_autoptr(GError) error = NULL;
      if (!xdp_doc_db_save (db, &error))
        g_warning ("db save: %s\n", error->message);
    }

  xdp_fuse_exit ();

  g_bus_unown_name (owner_id);

  g_dbus_node_info_unref (introspection_data);

  return 0;
}
