#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include "libgsystem.h"

#include "xdg-app-dir.h"
#include "xdg-app-utils.h"

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
xdg_app_dir_get_exports_dir (XdgAppDir     *self)
{
  return g_file_resolve_relative_path (self->basedir, "exports");
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
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
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

char *
xdg_app_dir_read_active (XdgAppDir *self,
                         const char *ref,
                         GCancellable *cancellable)
{
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *active_link = NULL;
  gs_unref_object GFileInfo *file_info = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  file_info = g_file_query_info (active_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info == NULL)
    return NULL;

  return g_strdup (g_file_info_get_symlink_target (file_info));
}

gboolean
xdg_app_dir_set_active (XdgAppDir *self,
                        const char *ref,
                        const char *checksum,
                        GCancellable *cancellable,
                        GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *deploy_base = NULL;
  gs_free char *tmpname = NULL;
  gs_unref_object GFile *active_tmp_link = NULL;
  gs_unref_object GFile *active_link = NULL;
  gs_free_error GError *my_error = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  if (checksum != NULL)
    {
      tmpname = gs_fileutil_gen_tmp_name (".active-", NULL);
      active_tmp_link = g_file_get_child (deploy_base, tmpname);
      if (!g_file_make_symbolic_link (active_tmp_link, checksum, cancellable, error))
        goto out;

      if (!gs_file_rename (active_tmp_link,
                           active_link,
                           cancellable, error))
        goto out;
    }
  else
    {
      if (!g_file_delete (active_link, cancellable, &my_error) &&
          !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, my_error);
          my_error = NULL;
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}


gboolean
xdg_app_dir_run_triggers (XdgAppDir *self,
			  GCancellable *cancellable,
			  GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;
  gs_unref_object GFile *triggersdir = NULL;
  gs_unref_object GFile *exports = NULL;
  GError *temp_error = NULL;

  g_debug ("running triggers");

  exports = xdg_app_dir_get_exports_dir (self);

  triggersdir = g_file_new_for_path (XDG_APP_TRIGGERDIR);

  dir_enum = g_file_enumerate_children (triggersdir, "standard::type,standard::name",
                                        0, cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      gs_unref_object GFile *child = NULL;
      const char *name;
      GError *trigger_error = NULL;

      name = g_file_info_get_name (child_info);

      child = g_file_get_child (triggersdir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_REGULAR &&
	  g_str_has_suffix (name, ".trigger"))
	{
	  gs_unref_ptrarray GPtrArray *argv_array = NULL;

	  g_debug ("running trigger %s", name);

	  argv_array = g_ptr_array_new_with_free_func (g_free);
	  g_ptr_array_add (argv_array, g_strdup (HELPER));
	  g_ptr_array_add (argv_array, g_strdup ("-a"));
	  g_ptr_array_add (argv_array, g_file_get_path (self->basedir));
	  g_ptr_array_add (argv_array, g_strdup ("-e"));
	  g_ptr_array_add (argv_array, g_strdup ("-F"));
	  g_ptr_array_add (argv_array, g_strdup ("/usr"));
	  g_ptr_array_add (argv_array, g_file_get_path (child));
	  g_ptr_array_add (argv_array, NULL);

	  if (!g_spawn_sync ("/",
			     (char **)argv_array->pdata,
			     NULL,
			     G_SPAWN_DEFAULT,
			     NULL, NULL,
			     NULL, NULL,
			     NULL, &trigger_error))
	    {
	      g_warning ("Error running trigger %s: %s", name, trigger_error->message);
	      g_clear_error (&trigger_error);
	    }
	}

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}



gboolean
xdg_app_dir_deploy (XdgAppDir *self,
                    const char *ref,
                    const char *checksum,
                    GCancellable *cancellable,
                    GError **error)
{
  gboolean ret = FALSE;
  gboolean is_app;
  gs_free char *resolved_ref = NULL;
  gs_unref_object GFile *root = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *checkoutdir = NULL;
  gs_unref_object GFile *dotref = NULL;
  gs_unref_object GFile *export = NULL;
  gs_unref_object GFile *exports = NULL;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (checksum == NULL)
    {
      if (!ostree_repo_resolve_rev (self->repo, ref, FALSE, &resolved_ref, error))
        goto out;

      checksum = resolved_ref;
    }

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, checksum);
  if (g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, XDG_APP_DIR_ERROR,
                   XDG_APP_DIR_ERROR_ALREADY_DEPLOYED,
                   "%s version %s already deployed", ref, checksum);
      goto out;
    }

  if (!ostree_repo_read_commit (self->repo, checksum, &root, NULL, cancellable, error))
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

  is_app = g_str_has_prefix (ref, "app");

  exports = xdg_app_dir_get_exports_dir (self);
  if (is_app)
    {
      export = g_file_get_child (checkoutdir, "export");
      if (g_file_query_exists (export, cancellable))
        {
          gs_free char *relative_path = NULL;
          gs_free char *symlink_prefix = NULL;

          relative_path = g_file_get_relative_path (self->basedir, export);
          symlink_prefix = g_build_filename ("..", relative_path, NULL);

          if (!xdg_app_overlay_symlink_tree (export, exports,
                                             symlink_prefix,
                                             cancellable,
                                             error))
              goto out;
        }
    }

  if (!xdg_app_dir_set_active (self, ref, checksum, cancellable, error))
    goto out;

  if (is_app && g_file_query_exists (exports, cancellable))
    {
      if (!xdg_app_remove_dangling_symlinks (exports, cancellable, error))
	goto out;

      if (!xdg_app_dir_run_triggers (self, cancellable, error))
	goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_list_deployed (XdgAppDir *self,
                           const char *ref,
                           char ***deployed_checksums,
                           GCancellable *cancellable,
                           GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_ptrarray GPtrArray *checksums = NULL;
  GError *temp_error = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *child = NULL;
  gs_unref_object GFileInfo *child_info = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checksums = g_ptr_array_new_with_free_func (g_free);

  dir_enum = g_file_enumerate_children (deploy_base, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (deploy_base, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' &&
          strlen (name) == 64)
        g_ptr_array_add (checksums, g_strdup (name));

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  g_ptr_array_add (checksums, NULL);
  *deployed_checksums = (char **)g_ptr_array_free (checksums, FALSE);
  checksums = NULL;

  ret = TRUE;
 out:
  return ret;

}

gboolean
xdg_app_dir_undeploy (XdgAppDir *self,
                      const char *ref,
                      const char *checksum,
                      GCancellable *cancellable,
                      GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *checkoutdir = NULL;
  gs_unref_object GFile *removeddir = NULL;
  gs_free char *tmpname = NULL;
  gs_free char *active = NULL;
  int i;

  g_assert (ref != NULL);
  g_assert (checksum != NULL);

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, checksum);
  if (!g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, XDG_APP_DIR_ERROR,
                   XDG_APP_DIR_ERROR_ALREADY_UNDEPLOYED,
                   "%s version %s already undeployed", ref, checksum);
      goto out;
    }

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  active = xdg_app_dir_read_active (self, ref, cancellable);
  if (active != NULL && strcmp (active, checksum) == 0)
    {
      gs_strfreev char **deployed_checksums = NULL;
      const char *some_deployment;

      /* We're removing the active deployment, start by repointing that
         to another deployment if one exists */

      if (!xdg_app_dir_list_deployed (self, ref,
                                      &deployed_checksums,
                                      cancellable, error))
        goto out;

      some_deployment = NULL;
      for (i = 0; deployed_checksums[i] != NULL; i++)
        {
          if (strcmp (deployed_checksums[i], checksum) == 0)
            continue;

          some_deployment = deployed_checksums[i];
          break;
        }

      if (!xdg_app_dir_set_active (self, ref, some_deployment, cancellable, error))
        goto out;
    }

  tmpname = gs_fileutil_gen_tmp_name (".removed-", checksum);

  checkoutdir = g_file_get_child (deploy_base, checksum);
  removeddir = g_file_get_child (deploy_base, tmpname);

  if (!gs_file_rename (checkoutdir,
                       removeddir,
                       cancellable, error))
    goto out;

  if (!gs_shutil_rm_rf (removeddir, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_prune (XdgAppDir      *self,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  gint objects_total, objects_pruned;
  guint64 pruned_object_size_total;
  gs_free char *formatted_freed_size = NULL;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (!ostree_repo_prune (self->repo,
                          OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY,
                          0,
                          &objects_total,
                          &objects_pruned,
                          &pruned_object_size_total,
                          cancellable, error))
    goto out;

  formatted_freed_size = g_format_size_full (pruned_object_size_total, 0);
  g_debug ("Pruned %d/%d objects, size %s", objects_total, objects_pruned, formatted_freed_size);

  ret = TRUE;
 out:
  return ret;

}

GFile *
xdg_app_dir_get_if_deployed (XdgAppDir     *self,
                             const char    *ref,
                             const char    *checksum,
                             GCancellable  *cancellable)
{
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *deploy_dir = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  deploy_dir = g_file_get_child (deploy_base, checksum ? checksum : "active");

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
      user = xdg_app_dir_new (path, TRUE);
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
