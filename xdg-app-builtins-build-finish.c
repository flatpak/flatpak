#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ftw.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_command;

static GOptionEntry options[] = {
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, "Command to set", "COMMAND" },
  { NULL }
};

static GFile *show_export_base;

static int
show_export (const char *fpath, const struct stat *sb, int typeflag)
{
  if (typeflag == FTW_F)
    {
      gs_unref_object GFile *file;
      gs_free char *relpath;

      file = g_file_new_for_path (fpath);
      relpath = g_file_get_relative_path (show_export_base, file);
      g_print ("Exporting %s\n", relpath);
    }

  return 0;
}

static gboolean
collect_exports (GFile *base, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *files = NULL;
  gs_unref_object GFile *export = NULL;
  const char *paths[] = {
    "share/applications",    /* Copy desktop files */
    "share/icons/hicolor",   /* Icons */
    "share/dbus-1/services", /* D-Bus service files */
    NULL,
  };
  int i;
  const char *path;

  files = g_file_get_child (base, "files");
  export = g_file_get_child (base, "export");

  for (i = 0; paths[i]; i++)
    {
      gs_unref_object GFile *src = NULL;
      src = g_file_resolve_relative_path (files, paths[i]);
      if (g_file_query_exists (src, cancellable))
        {
          g_debug ("Exporting from %s", paths[i]);
          gs_unref_object GFile *dest = NULL;
          dest = g_file_resolve_relative_path (export, paths[i]);
          g_debug ("Ensuring export/%s exists", paths[i]);
          if (!gs_file_ensure_directory (dest, TRUE, cancellable, error))
            goto out;
          g_debug ("Copying from files/%s", paths[i]);
          if (!gs_shutil_cp_a (src, dest, cancellable, error))
            goto out;
        }
    }

  path = g_file_get_path (export);
  show_export_base = export;
  ftw (path, show_export, 10);

  ret = TRUE;

  g_assert_no_error (*error);
out:
  return ret;
}

static gboolean
update_metadata (GFile *base, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *metadata = NULL;
  gs_free char *path = NULL;
  gs_unref_keyfile GKeyFile *keyfile = NULL;
  GError *temp_error = NULL;

  metadata = g_file_get_child (base, "metadata");
  if (!g_file_query_exists (metadata, cancellable))
    goto out;

  path = g_file_get_path (metadata);
  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    goto out;

  if (g_key_file_has_key (keyfile, "Application", "command", NULL))
    {
      g_debug ("Command key is present");

      if (opt_command)
        g_key_file_set_string (keyfile, "Application", "command", opt_command);
    }
  else if (opt_command)
    {
      g_debug ("Using explicitly provided command %s", opt_command);

      g_key_file_set_string (keyfile, "Application", "command", opt_command);
    }
  else
    {
      gs_free char *command = NULL;
      gs_unref_object GFile *bin_dir = NULL;
      gs_unref_object GFileEnumerator *bin_enum = NULL;
      gs_unref_object GFileInfo *child_info = NULL;

      g_debug ("Looking for executables");

      bin_dir = g_file_resolve_relative_path (base, "files/bin");
      if (g_file_query_exists (bin_dir, cancellable))
        {
          bin_enum = g_file_enumerate_children (bin_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                cancellable, error);
          if (!bin_enum)
            goto out;

          while ((child_info = g_file_enumerator_next_file (bin_enum, cancellable, &temp_error)))
            {
              if (command != NULL)
                {
                  g_print ("More than one executable found\n");
                  break;
                }
              command = g_strdup (g_file_info_get_name (child_info));
            }
          if (temp_error != NULL)
            goto out;
        }

      if (command)
        {
          g_print ("Using %s as command\n", command);
          g_key_file_set_string (keyfile, "Application", "command", command);
        }
      else
        {
          g_print ("No executable found\n");
        }
    }

  g_print ("Adding permissive environment\n");
  g_key_file_set_boolean (keyfile, "Environment", "x11", TRUE);
  g_key_file_set_boolean (keyfile, "Environment", "ipc", TRUE);
  g_key_file_set_boolean (keyfile, "Environment", "pulseaudio", TRUE);
  g_key_file_set_boolean (keyfile, "Environment", "system-dbus", TRUE);
  g_key_file_set_boolean (keyfile, "Environment", "session-dbus", TRUE);
  g_key_file_set_boolean (keyfile, "Environment", "network", TRUE);
  g_key_file_set_boolean (keyfile, "Environment", "host-fs", TRUE);
  g_key_file_set_boolean (keyfile, "Environment", "homedir", TRUE);

  if (!g_key_file_save_to_file (keyfile, path, error))
    goto out;

  ret = TRUE;

out:
  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  return ret;
}

static gboolean
clean_up_var (GFile *base, GCancellable *cancellable, GError **error)
{
  gs_unref_object GFile *var = NULL;

  var = g_file_get_child  (base, "var");
  return gs_shutil_rm_rf (var, cancellable, error);
}

gboolean
xdg_app_builtin_build_finish (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object GFile *base = NULL;
  gs_unref_object GFile *files_dir = NULL;
  gs_unref_object GFile *export_dir = NULL;
  gs_unref_object GFile *var_dir = NULL;
  gs_unref_object GFile *var_tmp_dir = NULL;
  gs_unref_object GFile *var_run_dir = NULL;
  gs_unref_object GFile *metadata_file = NULL;
  gs_unref_object XdgAppDir *user_dir = NULL;
  gs_unref_object XdgAppDir *system_dir = NULL;
  const char *directory;

  context = g_option_context_new ("DIRECTORY - Convert a directory to a bundle");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "DIRECTORY must be specified", error);
      goto out;
    }

  directory = argv[1];

  base = g_file_new_for_commandline_arg (directory);

  files_dir = g_file_get_child (base, "files");
  export_dir = g_file_get_child (base, "export");
  var_dir = g_file_get_child (base, "var");
  var_tmp_dir = g_file_get_child (var_dir, "tmp");
  var_run_dir = g_file_get_child (var_dir, "run");
  metadata_file = g_file_get_child (base, "metadata");

  if (!g_file_query_exists (files_dir, cancellable) ||
      !g_file_query_exists (metadata_file, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Build directory %s not initialized", directory);
      goto out;
    }

  if (g_file_query_exists (export_dir, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Build directory %s already finalized", directory);
      goto out;
    }

  g_debug ("Collecting exports");
  if (!collect_exports (base, cancellable, error))
    goto out;

  g_debug ("Updating metadata");
  if (!update_metadata (base, cancellable, error))
    goto out;

  g_debug ("Cleaning up var");
  if (!clean_up_var (base, cancellable, error))
    goto out;

  g_print ("Please review the exported files and the metadata\n");

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
