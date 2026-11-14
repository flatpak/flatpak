/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include "flatpak-download-tracker-private.h"
#include "flatpak-transaction.h"

static void
test_estimates (void)
{
  g_autoptr(FlatpakTransactionOperation) first = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);
  g_autoptr(FlatpakTransactionOperation) second = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);
  g_autoptr(FlatpakDownloadTracker) tracker = flatpak_download_tracker_new ();
  g_autoptr(FlatpakDownloadSnapshot) snapshot = NULL;

  flatpak_download_tracker_add (tracker, first, 1000);
  flatpak_download_tracker_add (tracker, second, 0);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_cmpuint (snapshot->aggregate.bytes_total, ==, 0);
  g_assert_cmpuint (snapshot->n_items, ==, 0);
  g_clear_pointer (&snapshot, flatpak_download_snapshot_free);

  flatpak_download_tracker_update (tracker, first, 200, 900, 100, TRUE);
  flatpak_download_tracker_update (tracker, second, 50, 150, 25, TRUE);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_cmpuint (snapshot->aggregate.bytes_transferred, ==, 250);
  g_assert_cmpuint (snapshot->aggregate.bytes_total, ==, 1050);
  g_assert_cmpuint (snapshot->aggregate.bytes_per_second, ==, 125);
  g_assert_cmpuint (snapshot->aggregate.active_downloads, ==, 2);
  g_assert_cmpuint (snapshot->n_items, ==, 2);
  g_assert_true (snapshot->items[0].operation == first);
  g_assert_true (snapshot->items[1].operation == second);
  g_clear_pointer (&snapshot, flatpak_download_snapshot_free);

  flatpak_download_tracker_update (tracker, first, 100, 80, 50, TRUE);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_cmpuint (snapshot->aggregate.bytes_transferred, ==, 250);
  g_assert_cmpuint (snapshot->aggregate.bytes_total, ==, 350);
  g_assert_cmpuint (snapshot->items[0].stats.bytes_total, ==, 200);
}

static void
test_retire_and_snapshot_lifetime (void)
{
  g_autoptr(FlatpakTransactionOperation) downloading = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);
  g_autoptr(FlatpakTransactionOperation) queued = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);
  g_autoptr(FlatpakDownloadTracker) tracker = flatpak_download_tracker_new ();
  g_autoptr(FlatpakDownloadSnapshot) before = NULL;
  g_autoptr(FlatpakDownloadSnapshot) after = NULL;

  flatpak_download_tracker_add (tracker, downloading, 1000);
  flatpak_download_tracker_add (tracker, queued, 2000);
  flatpak_download_tracker_update (tracker, downloading, 300, 1000, 50, TRUE);
  before = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_cmpuint (before->aggregate.bytes_total, ==, 3000);

  flatpak_download_tracker_retire (tracker, downloading);
  flatpak_download_tracker_retire (tracker, queued);
  flatpak_download_tracker_update (tracker, downloading, 400, 1000, 50, TRUE);
  after = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_cmpuint (after->n_items, ==, 1);
  g_assert_cmpuint (after->aggregate.bytes_transferred, ==, 300);
  g_assert_cmpuint (after->aggregate.bytes_total, ==, 300);
  g_assert_cmpuint (after->aggregate.active_downloads, ==, 0);
  g_assert_cmpuint (after->aggregate.bytes_per_second, ==, 0);
  g_assert_true (after->items[0].changed);

  g_clear_pointer (&tracker, flatpak_download_tracker_free);
  g_assert_cmpuint (before->aggregate.bytes_total, ==, 3000);
  g_assert_cmpuint (before->items[0].stats.active_downloads, ==, 1);
  g_assert_cmpuint (after->items[0].stats.active_downloads, ==, 0);
}

static void
test_download_phases (void)
{
  g_autoptr(FlatpakTransactionOperation) operation = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);
  g_autoptr(FlatpakDownloadTracker) tracker = flatpak_download_tracker_new ();
  g_autoptr(FlatpakDownloadSnapshot) snapshot = NULL;

  flatpak_download_tracker_add (tracker, operation, 1000);
  flatpak_download_tracker_update (tracker, operation, 100, 0, 0, FALSE);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_true (snapshot->items[0].changed);
  g_clear_pointer (&snapshot, flatpak_download_snapshot_free);

  flatpak_download_tracker_update (tracker, operation, 100, 0, 0, FALSE);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_false (snapshot->items[0].changed);
  g_clear_pointer (&snapshot, flatpak_download_snapshot_free);

  flatpak_download_tracker_update (tracker, operation, 150, 500, 25, TRUE);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_true (snapshot->items[0].changed);
  g_assert_cmpuint (snapshot->aggregate.active_downloads, ==, 1);
  g_assert_cmpuint (snapshot->aggregate.bytes_transferred, ==, 150);
  g_clear_pointer (&snapshot, flatpak_download_snapshot_free);

  flatpak_download_tracker_retire (tracker, operation);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_cmpuint (snapshot->aggregate.bytes_total, ==, 150);
  g_assert_cmpuint (snapshot->aggregate.active_downloads, ==, 0);
  g_clear_pointer (&snapshot, flatpak_download_snapshot_free);
  flatpak_download_tracker_retire (tracker, operation);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_false (snapshot->items[0].changed);
}

static void
test_unknown_total (void)
{
  g_autoptr(FlatpakTransactionOperation) operation = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);
  g_autoptr(FlatpakTransactionOperation) untracked = g_object_new (FLATPAK_TYPE_TRANSACTION_OPERATION, NULL);
  g_autoptr(FlatpakDownloadTracker) tracker = flatpak_download_tracker_new ();
  g_autoptr(FlatpakDownloadSnapshot) snapshot = NULL;

  flatpak_download_tracker_add (tracker, operation, 0);
  flatpak_download_tracker_update (tracker, operation, 100, 0, 25, TRUE);
  flatpak_download_tracker_update (tracker, untracked, 500, 1000, 100, TRUE);
  flatpak_download_tracker_retire (tracker, untracked);
  snapshot = flatpak_download_tracker_take_snapshot (tracker);
  g_assert_cmpuint (snapshot->aggregate.bytes_transferred, ==, 100);
  g_assert_cmpuint (snapshot->aggregate.bytes_total, ==, 0);
  g_assert_cmpuint (snapshot->n_items, ==, 1);
  g_assert_cmpuint (snapshot->items[0].stats.bytes_total, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/download-tracker/estimates", test_estimates);
  g_test_add_func ("/download-tracker/retire-and-snapshot-lifetime", test_retire_and_snapshot_lifetime);
  g_test_add_func ("/download-tracker/phases", test_download_phases);
  g_test_add_func ("/download-tracker/unknown-total", test_unknown_total);
  return g_test_run ();
}
