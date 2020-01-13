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
      OtCommitRef commit = ot_commit_ref_from_data (contents, size);
      g_autofree char *s = ot_commit_ref_print (commit, TRUE);
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
