#include "ostree_test.h"

static void
handle (char *filename)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize size;

  if (!g_file_get_contents (filename, &contents, &size, &error))
    {
      g_print ("Failed to load %s: %s\n", filename, error->message);
      return;
    }

  if (g_str_has_suffix (filename, ".commit"))
    {
      OtCommitRef commit = ot_commit_from_data (contents, size);
      g_autofree char *s = ot_commit_print (commit, TRUE);
      g_print ("%s: %s\n", filename, s);
    }
  else if (g_str_has_suffix (filename, ".dirtree"))
    {
      OtTreeMetaRef tree = ot_tree_meta_from_data (contents, size);
      g_autofree char *s = ot_tree_meta_print (tree, TRUE);
      g_print ("%s: %s\n", filename, s);
    }
  else if (g_str_has_suffix (filename, ".dirmeta"))
    {
      OtDirMetaRef dir = ot_dir_meta_from_data (contents, size);
      g_autofree char *s = ot_dir_meta_print (dir, TRUE);
      g_print ("%s: %s\n", filename, s);
    }
  else if (g_str_has_suffix (filename, "summary"))
    {
      OtSummaryRef summary = ot_summary_from_data (contents, size);
      g_autofree char *s = ot_summary_print (summary, TRUE);
      g_print ("%s: %s\n", filename, s);
    }
  else
    {
      g_print ("Unknown type %s\n", filename);
    }
}


int
main (int argc,
      char *argv[])
{
  for (int i = 1; i < argc; i++)
    handle (argv[i]);

  return 0;
}
