#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libglnx/libglnx.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "document-portal/xdp-dbus.h"

#include "flatpak-dbus.h"

char outdir[] = "/tmp/xdp-test-XXXXXX";

GTestDBus *dbus;
GDBusConnection *session_bus;
XdpDbusDocuments *documents;
char *mountpoint;
static gboolean have_fuse;

static char *
make_doc_dir (const char *id, const char *app)
{
  if (app)
    return g_build_filename (mountpoint, "by-app", app, id, NULL);
  else
    return g_build_filename (mountpoint, id, NULL);
}

static char *
make_doc_path (const char *id, const char *basename, const char *app)
{
  g_autofree char *dir = make_doc_dir (id, app);

  return g_build_filename (dir, basename, NULL);
}

static void
assert_host_has_contents (const char *basename, const char *expected_contents)
{
  g_autofree char *path = g_build_filename (outdir, basename, NULL);
  g_autofree char *real_contents = NULL;
  gsize real_contents_length;
  GError *error = NULL;

  g_file_get_contents (path, &real_contents, &real_contents_length, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (real_contents, ==, expected_contents);
  g_assert_cmpuint (real_contents_length, ==, strlen (expected_contents));
}

static void
assert_doc_has_contents (const char *id, const char *basename, const char *app, const char *expected_contents)
{
  g_autofree char *path = make_doc_path (id, basename, app);
  g_autofree char *real_contents = NULL;
  gsize real_contents_length;
  GError *error = NULL;

  g_file_get_contents (path, &real_contents, &real_contents_length, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (real_contents, ==, expected_contents);
  g_assert_cmpuint (real_contents_length, ==, strlen (expected_contents));
}

static void
assert_doc_not_exist (const char *id, const char *basename, const char *app)
{
  g_autofree char *path = make_doc_path (id, basename, app);
  struct stat buf;
  int res, fd;

  res = stat (path, &buf);
  g_assert_cmpint (res, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);

  fd = open (path, O_RDONLY);
  g_assert_cmpint (fd, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);
}

static char *
export_file (const char *path, gboolean unique)
{
  int fd, fd_id;
  GUnixFDList *fd_list = NULL;

  g_autoptr(GVariant) reply = NULL;
  GError *error = NULL;
  char *doc_id;

  fd = open (path, O_PATH | O_CLOEXEC);
  g_assert (fd >= 0);

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, &error);
  g_assert_no_error (error);
  close (fd);

  reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                         "org.freedesktop.portal.Documents",
                                                         "/org/freedesktop/portal/documents",
                                                         "org.freedesktop.portal.Documents",
                                                         "Add",
                                                         g_variant_new ("(hbb)", fd_id, !unique, FALSE),
                                                         G_VARIANT_TYPE ("(s)"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         30000,
                                                         fd_list, NULL,
                                                         NULL,
                                                         &error);
  g_object_unref (fd_list);
  g_assert_no_error (error);
  g_assert (reply != NULL);

  g_variant_get (reply, "(s)", &doc_id);
  g_assert (doc_id != NULL);
  return doc_id;
}

static char *
export_new_file (const char *basename, const char *contents, gboolean unique)
{
  g_autofree char *path = NULL;
  GError *error = NULL;

  path = g_build_filename (outdir, basename, NULL);

  g_file_set_contents (path, contents, -1, &error);
  g_assert_no_error (error);

  return export_file (path, unique);
}

static gboolean
update_doc (const char *id, const char *basename, const char *app, const char *contents, GError **error)
{
  g_autofree char *path = make_doc_path (id, basename, app);

  return g_file_set_contents (path, contents, -1, error);
}

static gboolean
update_from_host (const char *basename, const char *contents, GError **error)
{
  g_autofree char *path = g_build_filename (outdir, basename, NULL);

  return g_file_set_contents (path, contents, -1, error);
}


static void
grant_permissions (const char *id, const char *app, gboolean write)
{
  g_autoptr(GPtrArray) permissions = g_ptr_array_new ();
  GError *error = NULL;

  g_ptr_array_add (permissions, "read");
  if (write)
    g_ptr_array_add (permissions, "write");
  g_ptr_array_add (permissions, NULL);

  xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                  id,
                                                  app,
                                                  (const char **) permissions->pdata,
                                                  NULL,
                                                  &error);
  g_assert_no_error (error);
}

static void
test_create_doc (void)
{
  g_autofree char *doc_path = NULL;
  g_autofree char *doc_app_path = NULL;
  g_autofree char *host_path = NULL;
  g_autofree char *id = NULL;
  g_autofree char *id2 = NULL;
  g_autofree char *id3 = NULL;
  g_autofree char *id4 = NULL;
  g_autofree char *id5 = NULL;
  const char *basename = "a-file";
  GError *error = NULL;

  if (!have_fuse)
    {
      g_test_skip ("this test requires FUSE");
      return;
    }

  /* Export a document */
  id = export_new_file (basename, "content", FALSE);

  /* Ensure its there and not viewable by apps */
  assert_doc_has_contents (id, basename, NULL, "content");
  assert_host_has_contents (basename, "content");
  assert_doc_not_exist (id, basename, "com.test.App1");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "another-file", NULL);
  assert_doc_not_exist ("anotherid", basename, NULL);

  /* Create a tmp file in same dir, ensure it works and can't be seen by other apps */
  assert_doc_not_exist (id, "tmp1", NULL);
  update_doc (id, "tmp1", NULL, "tmpdata1", &error);
  g_assert_no_error (error);
  assert_doc_has_contents (id, "tmp1", NULL, "tmpdata1");
  assert_doc_not_exist (id, "tmp1", "com.test.App1");

  /* Let App 1 see the document (but not write) */
  grant_permissions (id, "com.test.App1", FALSE);

  /* Ensure App 1 and only it can see the document and tmpfile */
  assert_doc_has_contents (id, basename, "com.test.App1", "content");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "tmp1", "com.test.App1");

  /* Make sure App 1 can't create a tmpfile */
  assert_doc_not_exist (id, "tmp2", "com.test.App1");
  update_doc (id, "tmp2", "com.test.App1", "tmpdata2", &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error (&error);
  assert_doc_not_exist (id, "tmp2", "com.test.App1");

  /* Update the document contents, ensure this is propagater */
  update_doc (id, basename, NULL, "content2", &error);
  g_assert_no_error (error);
  assert_host_has_contents (basename, "content2");
  assert_doc_has_contents (id, basename, NULL, "content2");
  assert_doc_has_contents (id, basename, "com.test.App1", "content2");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "tmp1", "com.test.App2");

  /* Update the document contents outside fuse fd, ensure this is propagater */
  update_from_host (basename, "content3", &error);
  g_assert_no_error (error);
  assert_host_has_contents (basename, "content3");
  assert_doc_has_contents (id, basename, NULL, "content3");
  assert_doc_has_contents (id, basename, "com.test.App1", "content3");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "tmp1", "com.test.App2");

  /* Try to update the doc from an app that can't write to it */
  update_doc (id, basename, "com.test.App1", "content4", &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error (&error);

  /* Try to create a tmp file for an app that is not allowed */
  assert_doc_not_exist (id, "tmp2", "com.test.App1");
  update_doc (id, "tmp2", "com.test.App1", "tmpdata2", &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error (&error);
  assert_doc_not_exist (id, "tmp2", "com.test.App1");
  assert_doc_not_exist (id, "tmp2", NULL);

  /* Grant write permissions to App1 */
  grant_permissions (id, "com.test.App1", TRUE);

  /* update the doc from an app with write access */
  update_doc (id, basename, "com.test.App1", "content5", &error);
  g_assert_no_error (error);
  assert_host_has_contents (basename, "content5");
  assert_doc_has_contents (id, basename, NULL, "content5");
  assert_doc_has_contents (id, basename, "com.test.App1", "content5");
  assert_doc_not_exist (id, basename, "com.test.App2");

  /* Try to create a tmp file for an app */
  assert_doc_not_exist (id, "tmp3", "com.test.App1");
  update_doc (id, "tmp3", "com.test.App1", "tmpdata3", &error);
  g_assert_no_error (error);
  assert_doc_has_contents (id, "tmp3", "com.test.App1", "tmpdata3");
  assert_doc_not_exist (id, "tmp3", NULL);

  /* Re-Create a file from a fuse document file, in various ways */
  doc_path = make_doc_path (id, basename, NULL);
  doc_app_path = make_doc_path (id, basename, "com.test.App1");
  host_path = g_build_filename (outdir, basename, NULL);
  id2 = export_file (doc_path, FALSE);
  g_assert_cmpstr (id, ==, id2);
  id3 = export_file (doc_app_path, FALSE);
  g_assert_cmpstr (id, ==, id3);
  id4 = export_file (host_path, FALSE);
  g_assert_cmpstr (id, ==, id4);

  /* Ensure we can make a unique document */
  id5 = export_file (host_path, TRUE);
  g_assert_cmpstr (id, !=, id5);
}

static void
test_recursive_doc (void)
{
  g_autofree char *id = NULL;
  g_autofree char *id2 = NULL;
  g_autofree char *id3 = NULL;
  const char *basename = "recursive-file";
  g_autofree char *path = NULL;
  g_autofree char *app_path = NULL;

  if (!have_fuse)
    {
      g_test_skip ("this test requires FUSE");
      return;
    }

  id = export_new_file (basename, "recursive-content", FALSE);

  assert_doc_has_contents (id, basename, NULL, "recursive-content");

  path = make_doc_path (id, basename, NULL);
  g_print ("path: %s\n", path);

  id2 = export_file (path, FALSE);

  g_assert_cmpstr (id, ==, id2);

  grant_permissions (id, "com.test.App1", FALSE);

  app_path = make_doc_path (id, basename, "com.test.App1");

  id3 = export_file (app_path, FALSE);

  g_assert_cmpstr (id, ==, id3);
}

static void
test_create_docs (void)
{
  GError *error = NULL;
  g_autofree char *path1 = NULL;
  g_autofree char *path2 = NULL;
  int fd1, fd2;
  guint32 fd_ids[2];
  GUnixFDList *fd_list = NULL;
  gboolean res;
  char **out_doc_ids;
  g_autoptr(GVariant) out_extra = NULL;
  const char *permissions[] = { "read", NULL };
  const char *basenames[] = { "doc1", "doc2" };
  int i;

  if (!have_fuse)
    {
      g_test_skip ("this test requires FUSE");
      return;
    }

  path1 = g_build_filename (outdir, basenames[0], NULL);
  g_file_set_contents (path1, basenames[0], -1, &error);
  g_assert_no_error (error);

  fd1 = open (path1, O_PATH | O_CLOEXEC);
  g_assert (fd1 >= 0);

  path2 = g_build_filename (outdir, basenames[1], NULL);
  g_file_set_contents (path2, basenames[1], -1, &error);
  g_assert_no_error (error);

  fd2 = open (path2, O_PATH | O_CLOEXEC);
  g_assert (fd2 >= 0);

  fd_list = g_unix_fd_list_new ();
  fd_ids[0] = g_unix_fd_list_append (fd_list, fd1, &error);
  g_assert_no_error (error);
  close (fd1);
  fd_ids[1] = g_unix_fd_list_append (fd_list, fd2, &error);
  g_assert_no_error (error);
  close (fd2);

  res = xdp_dbus_documents_call_add_full_sync (documents,
                                               g_variant_new_fixed_array (G_VARIANT_TYPE_HANDLE,
                                                                          fd_ids, 2, sizeof (guint32)),
                                               0,
                                               "org.other.App",
                                               permissions,
                                               fd_list,
                                               &out_doc_ids,
                                               &out_extra,
                                               NULL,
                                               NULL, &error);
  g_assert_no_error (error);
  g_assert (res);

  g_assert (g_strv_length (out_doc_ids) == 2);
  for (i = 0; i < 2; i++)
    {
      const char *id = out_doc_ids[i];

      /* Ensure its there and not viewable by apps */
      assert_doc_has_contents (id, basenames[i], NULL, basenames[i]);
      assert_host_has_contents (basenames[i], basenames[i]);
      assert_doc_not_exist (id, basenames[i], "com.test.App1");
      assert_doc_not_exist (id, basenames[i], "com.test.App2");
      assert_doc_not_exist (id, "another-file", NULL);
      assert_doc_not_exist ("anotherid", basenames[i], NULL);

      assert_doc_has_contents (id, basenames[i], "org.other.App", basenames[i]);
      update_doc (id, basenames[i], "org.other.App", "tmpdata2", &error);
      g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
      g_clear_error (&error);
    }
  g_assert (g_variant_lookup_value (out_extra, "mountpoint", G_VARIANT_TYPE_VARIANT) == 0);
}


static void
global_setup (void)
{
  gboolean inited;
  g_autofree gchar *fusermount = NULL;
  GError *error = NULL;
  g_autofree gchar *services = NULL;

  fusermount = g_find_program_in_path ("fusermount");
  /* cache result so subsequent tests can be marked as skipped */
  have_fuse = (access ("/dev/fuse", W_OK) == 0 &&
               fusermount != NULL &&
               g_file_test (fusermount, G_FILE_TEST_IS_EXECUTABLE));

  if (!have_fuse)
    return;

  g_mkdtemp (outdir);
  g_print ("outdir: %s\n", outdir);

  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);

  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  services = g_test_build_filename (G_TEST_BUILT, "services", NULL);
  g_test_dbus_add_service_dir (dbus, services);
  g_test_dbus_up (dbus);

  /* g_test_dbus_up unsets this, so re-set */
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, &error);
  g_assert_no_error (error);
  g_assert (documents != NULL);

  inited = xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint,
                                                         NULL, &error);
  g_assert_no_error (error);
  g_assert (inited);
  g_assert (mountpoint != NULL);
}

static void
global_teardown (void)
{
  GError *error = NULL;

  if (!have_fuse)
    return;

  g_free (mountpoint);

  g_object_unref (documents);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_object_unref (session_bus);

  g_test_dbus_down (dbus);

  g_object_unref (dbus);

  /* We race on the unmount of the fuse fs, which causes the rm -rf to stop at the doc dir.
     This makes the chance of completely removing the directory higher */
  sleep (1);

  glnx_shutil_rm_rf_at (-1, outdir, NULL, NULL);
}

int
main (int argc, char **argv)
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/db/create_doc", test_create_doc);
  g_test_add_func ("/db/recursive_doc", test_recursive_doc);
  g_test_add_func ("/db/create_docs", test_create_docs);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}
