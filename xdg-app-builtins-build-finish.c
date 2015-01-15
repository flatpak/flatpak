#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { NULL }
};

static gboolean
copy_subdir (GFile *files, GFile *export, const char *path, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *source_dir = NULL;
  gs_unref_object GFile *dest_dir = NULL;
  gs_unref_object GFileEnumerator *source_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;
  GError *temp_error = NULL;

  source_dir = g_file_resolve_relative_path (files, path);
  if (!g_file_query_exists (source_dir, cancellable))
    {
      ret = TRUE;
      goto out;
    }

  dest_dir = g_file_resolve_relative_path (export, path);
  if (!gs_file_ensure_directory (dest_dir, TRUE, cancellable, error))
    goto out;

  source_enum = g_file_enumerate_children (source_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           cancellable, error);
  if (!source_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (source_enum, cancellable, &temp_error)))
    {
      gs_unref_object GFile *source = NULL;
      gs_unref_object GFile *destination = NULL;
      gs_free char *relpath = NULL;
      const char *name;

      name = g_file_info_get_name (child_info);

      source = g_file_get_child (source_dir, name);
      destination = g_file_get_child (dest_dir, name);
      if (!g_file_copy (source, destination, G_FILE_COPY_OVERWRITE, cancellable, NULL, NULL, error))
        goto out;

      relpath = g_file_get_relative_path (files, source);
      g_print ("Exporting %s\n", relpath);
    }

  ret = TRUE;

out:
  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  return ret;
}

static gboolean
collect_exports (GFile *base, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *files = NULL;
  gs_unref_object GFile *export = NULL;
  const gchar *sizes[] = {
    "16x16", "24x24", "32x32", "48x48", "64x64", "96x96",
    "128x128", "256x256", "512x512", "scalable",
    NULL
  };
  int i;

  files = g_file_get_child (base, "files");
  export = g_file_get_child (base, "export");

  /* Copy desktop files */
  if (!copy_subdir (files, export, "share/applications", cancellable, error))
    goto out;

  /* Copy icons */
  for (i = 0; sizes[i]; i++)
    {
      gs_free char *path;

      path = g_strconcat ("share/icons/hicolor/", sizes[i], "/apps", NULL);
      if (!copy_subdir (files, export, path, cancellable, error))
        goto out;
    }

  /* Copy D-Bus service files */
  if (!copy_subdir (files, export, "share/dbus-1/services", cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
}

static gboolean
update_metadata (GFile *base, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *bin_dir = NULL;
  gs_unref_object GFile *metadata = NULL;
  gs_unref_object GFileEnumerator *bin_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;
  gs_free char *command = NULL;
  gs_free char *path = NULL;
  GError *temp_error = NULL;
  gs_unref_keyfile GKeyFile *keyfile = NULL;

  bin_dir = g_file_resolve_relative_path (base, "files/bin");
  if (!g_file_query_exists (bin_dir, cancellable))
    goto rewrite;

  bin_enum = g_file_enumerate_children (bin_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!bin_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (bin_enum, cancellable, &temp_error)))
    {
      if (command != NULL)
        {
          g_print ("More than one executable\n");
          break;
        }

      command = g_strdup (g_file_info_get_name (child_info));
    }

rewrite:
  metadata = g_file_get_child (base, "metadata");
  if (!g_file_query_exists (metadata, cancellable))
    goto out;

  path = g_file_get_path (metadata);
  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
    goto out;

  if (command)
    {
      g_print ("Using %s as command\n", command);
      g_key_file_set_string (keyfile, "Application", "command", command);
    }
  else
    {
      g_print ("No executable found\n");
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

  if (!gs_file_ensure_directory (base, TRUE, cancellable, error))
    goto out;

  files_dir = g_file_get_child (base, "files");
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

  if (!collect_exports (base, cancellable, error))
    goto out;

  if (!update_metadata (base, cancellable, error))
    goto out;

  if (!clean_up_var (base, cancellable, error))
    goto out;

  g_print ("Please review the exported files and the metadata\n");

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
