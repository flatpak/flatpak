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

gboolean
xdg_app_builtin_make_repo (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object GFile *base = NULL;
  gs_unref_object GFile *files = NULL;
  gs_unref_object GFile *metadata = NULL;
  gs_unref_object GFile *export = NULL;
  gs_unref_object GFile *repofile = NULL;
  gs_unref_object GFile *arg = NULL;
  gs_unref_object GFile *root = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *repoarg;
  const char *directory;
  const char *name;
  const char *arch;
  const char *branch;
  gs_free char *full_branch = NULL;
  gs_free char *parent = NULL;
  gs_free char *commit_checksum = NULL;
  gs_unref_object OstreeMutableTree *mtree = NULL;
  char *subject = NULL;
  gs_free char *body = NULL;
  OstreeRepoTransactionStats stats;

  context = g_option_context_new ("REPO DIRECTORY NAME [BRANCH] - Create a repository from a build directory");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 4)
    {
      usage_error (context, "RUNTIME must be specified", error);
      goto out;
    }

  repoarg = argv[1];
  directory = argv[2];
  name = argv[3];

  if (argc >= 5)
    branch = argv[4];
  else
    branch = "master";

  if (opt_arch)
    arch = opt_arch;
  else
    arch = xdg_app_get_arch ();

  subject = "Import an application build";
  body = g_strconcat ("Name: ", name, "\nArch: ", arch, "\nBranch: ", branch, NULL);
  full_branch = g_strconcat ("app/", name, "/", arch, "/", branch, NULL);

  base = g_file_new_for_commandline_arg (directory);
  files = g_file_get_child (base, "files");
  metadata = g_file_get_child (base, "metadata");
  export = g_file_get_child (base, "export");

  if (!g_file_query_exists (files, cancellable) ||
      !g_file_query_exists (metadata, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Build directory %s not initialized", directory);
      goto out;
    }

  if (!g_file_query_exists (export, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Build directory %s not finalized", directory);
      goto out;
    }

  repofile = g_file_new_for_commandline_arg (repoarg);
  repo = ostree_repo_new (repofile);

  if (g_file_query_exists (repofile, cancellable))
    {
      if (!ostree_repo_open (repo, cancellable, error))
        goto out;

      if (!ostree_repo_resolve_rev (repo, full_branch, TRUE, &parent, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_create (repo, OSTREE_REPO_MODE_ARCHIVE_Z2, cancellable, error))
        goto out;
    }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();
  arg = g_file_new_for_commandline_arg (directory);
  if (!ostree_repo_write_directory_to_mtree (repo, arg, mtree, NULL, cancellable, error))
    goto out;

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (!ostree_repo_write_commit (repo, parent, subject, body, NULL,
                                 OSTREE_REPO_FILE (root),
                                 &commit_checksum, cancellable, error))
    goto out;

  ostree_repo_transaction_set_ref (repo, NULL, full_branch, commit_checksum);

  if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
    goto out;

  g_print ("Commit: %s\n", commit_checksum);
  g_print ("Metadata Total: %u\n", stats.metadata_objects_total);
  g_print ("Metadata Written: %u\n", stats.metadata_objects_written);
  g_print ("Content Total: %u\n", stats.content_objects_total);
  g_print ("Content Written: %u\n", stats.content_objects_written);
  g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats.content_bytes_written);

  ret = TRUE;

 out:
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  if (context)
    g_option_context_free (context);

  return ret;
}
