#include "config.h"

#include <gio/gio.h>
#include "libgsystem.h"

#include "xdg-app-dir.h"

struct XdgAppDir {
  GObject parent;

  gboolean user;
  GFile *basedir;
  OstreeRepo *repo;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppDirClass;

G_DEFINE_TYPE (XdgAppDir, xdg_app_dir, G_TYPE_OBJECT)

G_DEFINE_QUARK (xdg-app-dir-error-quark, xdg_app_dir_error)

enum {
  PROP_0,

  PROP_USER,
  PROP_PATH
};

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

GFile *
xdg_app_get_system_base_dir_location (void)
{
  return g_file_new_for_path (XDG_APP_BASEDIR);
}

GFile *
xdg_app_get_user_base_dir_location (void)
{
  gs_free char *base = g_build_filename (g_get_user_data_dir (), "xdg-app", NULL);
  return g_file_new_for_path (base);
}

static void
xdg_app_dir_finalize (GObject *object)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->basedir);

  G_OBJECT_CLASS (xdg_app_dir_parent_class)->finalize (object);
}

static void
xdg_app_dir_set_property(GObject         *object,
                         guint            prop_id,
                         const GValue    *value,
                         GParamSpec      *pspec)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->basedir = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
      break;
    case PROP_USER:
      self->user = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_dir_get_property(GObject         *object,
                         guint            prop_id,
                         GValue          *value,
                         GParamSpec      *pspec)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->basedir);
      break;
    case PROP_USER:
      g_value_set_boolean (value, self->user);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_dir_class_init (XdgAppDirClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_dir_get_property;
  object_class->set_property = xdg_app_dir_set_property;
  object_class->finalize = xdg_app_dir_finalize;

  g_object_class_install_property (object_class,
                                   PROP_USER,
                                   g_param_spec_boolean ("user",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
xdg_app_dir_init (XdgAppDir *self)
{
}

gboolean
xdg_app_dir_is_user (XdgAppDir *self)
{
  return self->user;
}

GFile *
xdg_app_dir_get_path (XdgAppDir *self)
{
  return self->basedir;
}

GFile *
xdg_app_dir_get_deploy_dir (XdgAppDir     *self,
                            const char    *ref)
{
  return g_file_resolve_relative_path (self->basedir, ref);
}

GFile *
xdg_app_dir_get_app_data (XdgAppDir     *self,
                          const char    *app)
{
  gs_free char *partial_ref = NULL;

  partial_ref = g_build_filename ("app", app, "data", NULL);
  return g_file_resolve_relative_path (self->basedir, partial_ref);
}

OstreeRepo *
xdg_app_dir_get_repo (XdgAppDir *self)
{
  return self->repo;
}

gboolean
xdg_app_dir_ensure_path (XdgAppDir     *self,
                         GCancellable  *cancellable,
                         GError       **error)
{
  return gs_file_ensure_directory (self->basedir, TRUE, cancellable, error);
}

gboolean
xdg_app_dir_ensure_repo (XdgAppDir *self,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *repodir = NULL;
  gs_unref_object OstreeRepo *repo = NULL;

  if (self->repo == NULL)
    {
      if (!xdg_app_dir_ensure_path (self, cancellable, error))
        goto out;

      repodir = g_file_get_child (self->basedir, "repo");
      repo = ostree_repo_new (repodir);

      if (!g_file_query_exists (repodir, cancellable))
        {
          if (!ostree_repo_create (repo,
                                   self->user ? OSTREE_REPO_MODE_BARE_USER : OSTREE_REPO_MODE_BARE,
                                   cancellable, error))
            {
              gs_shutil_rm_rf (repodir, cancellable, NULL);
              goto out;
            }
        }
      else
        {
          if (!ostree_repo_open (repo, cancellable, error))
            goto out;
        }

      self->repo = g_object_ref (repo);
    }

  ret = TRUE;
 out:
  return ret;
}

static void
pull_progress (OstreeAsyncProgress       *progress,
               gpointer                   user_data)
{
  GSConsole *console = user_data;
  GString *buf;
  gs_free char *status = NULL;
  guint outstanding_fetches;
  guint outstanding_writes;
  guint n_scanned_metadata;

  if (!console)
    return;

  buf = g_string_new ("");

  status = ostree_async_progress_get_status (progress);
  outstanding_fetches = ostree_async_progress_get_uint (progress, "outstanding-fetches");
  outstanding_writes = ostree_async_progress_get_uint (progress, "outstanding-writes");
  n_scanned_metadata = ostree_async_progress_get_uint (progress, "scanned-metadata");
  if (status)
    {
      g_string_append (buf, status);
    }
  else if (outstanding_fetches)
    {
      guint64 bytes_transferred = ostree_async_progress_get_uint64 (progress, "bytes-transferred");
      guint fetched = ostree_async_progress_get_uint (progress, "fetched");
      guint requested = ostree_async_progress_get_uint (progress, "requested");
      guint64 bytes_sec = (g_get_monotonic_time () - ostree_async_progress_get_uint64 (progress, "start-time")) / G_USEC_PER_SEC;
      gs_free char *formatted_bytes_transferred =
        g_format_size_full (bytes_transferred, 0);
      gs_free char *formatted_bytes_sec = NULL;

      if (!bytes_sec) // Ignore first second
        formatted_bytes_sec = g_strdup ("-");
      else
        {
          bytes_sec = bytes_transferred / bytes_sec;
          formatted_bytes_sec = g_format_size (bytes_sec);
        }

      g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                              (guint)((((double)fetched) / requested) * 100),
                              fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  gs_console_begin_status_line (console, buf->str, NULL, NULL);

  g_string_free (buf, TRUE);
}

gboolean
xdg_app_dir_pull (XdgAppDir *self,
                  const char *repository,
                  const char *ref,
                  GCancellable *cancellable,
                  GError **error)
{
  gboolean ret = FALSE;
  GSConsole *console = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;
  const char *refs[2];

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (pull_progress, console);
    }

  refs[0] = ref;
  refs[1] = NULL;
  if (!ostree_repo_pull (self->repo, repository,
                         (char **)refs, OSTREE_REPO_PULL_FLAGS_NONE,
                         progress,
                         cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_deploy (XdgAppDir *self,
                    const char *ref,
                    const char *hash,
                    GCancellable *cancellable,
                    GError **error)
{
  gboolean ret = FALSE;
  gs_free char *resolved_ref = NULL;
  gs_free char *tmpname = NULL;
  gs_unref_object GFile *root = NULL;
  gs_unref_object GFile *latest_tmp_link = NULL;
  gs_unref_object GFile *latest_link = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *checkoutdir = NULL;
  gs_unref_object GFile *dotref = NULL;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (hash == NULL)
    {
      if (!ostree_repo_resolve_rev (self->repo, ref, FALSE, &resolved_ref, error))
        goto out;

      hash = resolved_ref;
    }

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, hash);
  if (g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, XDG_APP_DIR_ERROR,
                   XDG_APP_DIR_ERROR_ALREADY_DEPLOYED,
                   "%s version %s already deployed", ref, hash);
      goto out;
    }

  if (!ostree_repo_read_commit (self->repo, hash, &root, NULL, cancellable, error))
    goto out;

  file_info = g_file_query_info (root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (file_info == NULL)
    goto out;

  if (!ostree_repo_checkout_tree (self->repo,
                                  self->user ? OSTREE_REPO_CHECKOUT_MODE_USER : OSTREE_REPO_CHECKOUT_MODE_NONE,
                                  OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
                                  checkoutdir,
                                  OSTREE_REPO_FILE (root), file_info,
                                  cancellable, error))
    goto out;

  dotref = g_file_get_child (checkoutdir, ".ref");
  if (!g_file_replace_contents (dotref, "", 0, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, cancellable, error))
    goto out;


  tmpname = gs_fileutil_gen_tmp_name (".latest-", NULL);
  latest_tmp_link = g_file_get_child (deploy_base, tmpname);
  if (!g_file_make_symbolic_link (latest_tmp_link, hash, cancellable, error))
    goto out;

  latest_link = g_file_get_child (deploy_base, "latest");
  if (!gs_file_rename (latest_tmp_link,
                       latest_link,
                       cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

GFile *
xdg_app_dir_get_if_deployed (XdgAppDir     *self,
                             const char    *ref,
                             const char    *hash,
                             GCancellable  *cancellable)
{
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *deploy_dir = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  deploy_dir = g_file_get_child (deploy_base, hash ? hash : "latest");

  if (g_file_query_file_type (deploy_dir, G_FILE_QUERY_INFO_NONE, cancellable) == G_FILE_TYPE_DIRECTORY)
    return g_object_ref (deploy_dir);
  return NULL;
}

XdgAppDir*
xdg_app_dir_new (GFile *path, gboolean user)
{
  return g_object_new (XDG_APP_TYPE_DIR, "path", path, "user", user, NULL);
}

XdgAppDir *
xdg_app_dir_get_system (void)
{
  static XdgAppDir *system = NULL;

  if (system == NULL)
    {
      gs_unref_object GFile *path = xdg_app_get_system_base_dir_location ();
      system = xdg_app_dir_new (path, FALSE);
    }

  return g_object_ref (system);
}

XdgAppDir *
xdg_app_dir_get_user  (void)
{
  static XdgAppDir *user = NULL;

  if (user == NULL)
    {
      gs_unref_object GFile *path = xdg_app_get_user_base_dir_location ();
      user = xdg_app_dir_new (path, FALSE);
    }

  return g_object_ref (user);
}

XdgAppDir *
xdg_app_dir_get (gboolean user)
{
  if (user)
    return xdg_app_dir_get_user ();
  else
    return xdg_app_dir_get_system ();
}
