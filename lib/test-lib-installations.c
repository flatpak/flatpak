#include "config.h"

#include <flatpak.h>
#include <glib.h>

static void
test_get_system_installations (void)
{
  g_autoptr (GPtrArray) installs = NULL;
  g_autoptr(GError) error = NULL;
  int i;

  installs = flatpak_get_system_installations (NULL, &error);
  if (installs == NULL)
    {
      g_print ("Error getting system installations: %s", error->message);
      g_assert (FALSE);
      return;
    }

  g_print ("\nInstallations found: %d\n", installs->len);
  for (i = 0; i < installs->len; i++)
    {
      FlatpakInstallation *installation = (FlatpakInstallation*) g_ptr_array_index (installs, i);
      g_autoptr(GFile) installation_path = flatpak_installation_get_path (installation);

      g_autofree char *path = g_file_get_path (installation_path);
      g_print ("\nInstallation found: %s\n", path);
    }

  g_assert (TRUE);
}

static void
test_new_system_with_id (void)
{
  const char *installs[] = { "endless-games", "endless-sdcard", NULL };
  gboolean all_found = TRUE;
  int i;

  for (i = 0; installs[i] != NULL; i++)
    {
      g_print("Checking %s...\n", installs[i]);
      g_autoptr (FlatpakInstallation) install = NULL;
      g_autoptr(GError) error = NULL;

      install = flatpak_installation_new_system_with_id (installs[i], NULL, &error);
      if (install != NULL)
        {
          g_autoptr(GFile) path = flatpak_installation_get_path (install);
          g_print ("Installation '%s' found. Path: %s\n", installs[i], g_file_get_path (path));
        }
      else
        {
          g_print ("Could NOT find system installation '%s': %s\n", installs[i], error->message);
          all_found = FALSE;
        }
    }

  g_assert (all_found);
}

static void
test_system_installations_extra_data (void)
{
  g_autoptr (GPtrArray) installs = NULL;
  g_autoptr(GError) error = NULL;
  int i;

  installs = flatpak_get_system_installations (NULL, &error);
  if (installs == NULL)
    {
      g_print ("Error getting system installations: %s", error->message);
      g_assert (FALSE);
      return;
    }

  g_print ("\nInstallations found: %d\n", installs->len);
  for (i = 0; i < installs->len; i++)
    {
      FlatpakInstallation *installation = (FlatpakInstallation*) g_ptr_array_index (installs, i);
      const char *current_id = flatpak_installation_get_id (installation);
      const char *current_display_name = flatpak_installation_get_display_name (installation);
      const gint current_priority = flatpak_installation_get_priority (installation);
      const FlatpakStorageType current_storage_type = flatpak_installation_get_storage_type (installation);

      g_autoptr(GFile) installation_path = flatpak_installation_get_path (installation);

      g_autofree char *path = g_file_get_path (installation_path);
      g_print ("\nExtra data for system installation found at %s:\n", path);

      g_print ("\tID: %s\n", current_id);
      g_print ("\tDisplay name: %s\n", current_display_name);
      g_print ("\tPriority: %d\n", current_priority);
      g_print ("\tStorage type: %d\n", current_storage_type);

      if (current_id != NULL)
        {
          g_autoptr (FlatpakInstallation) install = NULL;
          g_print ("\n  Retrieving extra data for ID %s:\n", current_id);

          install = flatpak_installation_new_system_with_id (current_id, NULL, &error);
          if (install != NULL)
            {
              const char *queried_id = flatpak_installation_get_id (install);
              const char *queried_display_name = flatpak_installation_get_display_name (install);
              const gint queried_priority = flatpak_installation_get_priority (install);
              const FlatpakStorageType queried_storage_type = flatpak_installation_get_storage_type (install);

              g_assert_cmpstr(current_id, ==, queried_id);
              g_assert_cmpstr(current_display_name, ==, queried_display_name);
              g_assert_cmpint(current_priority, ==, queried_priority);
              g_assert_cmpint(current_storage_type, ==, queried_storage_type);

              g_print ("\t Installation '%s' found. Details:\n", current_id);
              g_print ("\t   ID: %s\n", queried_id);
              g_print ("\t   Display name: %s\n", queried_display_name);
              g_print ("\t   Priority: %d\n", queried_priority);
              g_print ("\t   Storage type: %d\n", queried_storage_type);
            }
          else
            {
              g_print ("Could NOT find system installation '%s': %s\n", current_id, error->message);
            }
        }

    }

  g_assert (TRUE);
}

int
main (int argc, char *argv[])
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/library/get_system_installations", test_get_system_installations);
  g_test_add_func ("/library/new_system_with_id", test_new_system_with_id);
  g_test_add_func ("/library/system_installations_extra_data", test_system_installations_extra_data);

  res = g_test_run ();

  return res;
}

