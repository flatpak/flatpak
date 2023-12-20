#include "config.h"

#include "flatpak-dir-private.h"

static char **opt_exclude_refs;
static gboolean opt_user;
static gboolean opt_filter_eol;
static gboolean opt_filter_autoprune;

static GOptionEntry options[] = {
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, "Work on the user installation", NULL },
  { "exclude", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_exclude_refs, "Exclude ref", "REF" },
  { "filter-eol", 0, 0, G_OPTION_ARG_NONE, &opt_filter_eol, "Filter results to include end-of-life refs", NULL },
  { "filter-autoprune", 0, 0, G_OPTION_ARG_NONE, &opt_filter_autoprune, "Filter results to include autopruned refs", NULL },
  { NULL }
};


int
main (int argc, char *argv[])
{
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) refs = NULL;
  g_autoptr(GOptionContext) context = NULL;
  FlatpakDirFilterFlags filter_flags = FLATPAK_DIR_FILTER_NONE;
  int i;

  context = g_option_context_new ("");

  g_option_context_add_main_entries (context, options, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_print("Arg error: %s\n", error->message);
      return 1;
    }

  if (opt_user)
    dir = flatpak_dir_get_user ();
  else
    dir = flatpak_dir_get_system_default ();

  if (opt_filter_eol)
    filter_flags |= FLATPAK_DIR_FILTER_EOL;

  if (opt_filter_autoprune)
    filter_flags |= FLATPAK_DIR_FILTER_AUTOPRUNE;

  refs = flatpak_dir_list_unused_refs (dir, NULL, NULL, NULL, (const char * const *)opt_exclude_refs, filter_flags, NULL, &error);
  g_assert_nonnull (refs);
  g_assert_no_error (error);

  for (i = 0; refs[i] != NULL; i++)
    g_print ("%s\n", refs[i]);

  return 0;
}
