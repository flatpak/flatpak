/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include "flatpak-progress-private.h"
#include "flatpak-pull-job-private.h"

typedef struct
{
  const char *name;
  gboolean    with_progress;
  gboolean    with_callback;
  int         error_code;
} DeploymentScenario;

typedef struct
{
  const DeploymentScenario *scenario;
  GThread                  *caller;
  GCancellable             *cancellable;
  GMutex                    mutex;
  GCond                     condition;
  gboolean                  released;
  gboolean                  finished;
  size_t                    callbacks;
} DeploymentCheck;

static gboolean
deploy (gpointer data, GCancellable *cancellable, GError **error)
{
  DeploymentCheck *check = data;
  const DeploymentScenario *scenario = check->scenario;

  g_assert_true (cancellable == check->cancellable);
  g_assert_true ((g_thread_self () != check->caller) == scenario->with_callback);
  if (scenario->with_callback)
    {
      int64_t deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

      g_mutex_lock (&check->mutex);
      while (!check->released)
        if (!g_cond_wait_until (&check->condition, &check->mutex, deadline))
          break;
      g_assert_true (check->released);
      g_mutex_unlock (&check->mutex);
    }

  if (scenario->error_code == G_IO_ERROR_CANCELLED)
    g_assert_true (g_cancellable_set_error_if_cancelled (cancellable, error));
  else if (scenario->error_code >= 0)
    g_set_error_literal (error, G_IO_ERROR, scenario->error_code, "Deployment failed");

  g_mutex_lock (&check->mutex);
  check->finished = TRUE;
  g_mutex_unlock (&check->mutex);
  return scenario->error_code < 0;
}

static void
deployment_progress (const char *status, guint percent, gboolean estimating, gpointer data)
{
  DeploymentCheck *check = data;

  g_assert_true (g_thread_self () == check->caller);
  check->callbacks++;
  if (check->callbacks == 2)
    {
      if (check->scenario->error_code == G_IO_ERROR_CANCELLED)
        g_cancellable_cancel (check->cancellable);
      g_mutex_lock (&check->mutex);
      check->released = TRUE;
      g_cond_signal (&check->condition);
      g_mutex_unlock (&check->mutex);
    }
}

static void
test_deployment (gconstpointer data)
{
  const DeploymentScenario *scenario = data;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(FlatpakProgress) progress = NULL;
  g_autoptr(GError) error = NULL;
  DeploymentCheck check = { .scenario = scenario, .caller = g_thread_self (), .cancellable = cancellable };
  gboolean result;

  g_mutex_init (&check.mutex);
  g_cond_init (&check.condition);
  if (scenario->with_progress)
    {
      progress = flatpak_progress_new (scenario->with_callback ? deployment_progress : NULL, &check);
      flatpak_progress_set_update_interval (progress, 1);
    }
  result = flatpak_progress_run_deployment (progress, deploy, &check, cancellable, &error);
  g_mutex_lock (&check.mutex);
  g_assert_true (check.finished);
  g_mutex_unlock (&check.mutex);
  if (scenario->error_code < 0)
    {
      g_assert_true (result);
      g_assert_no_error (error);
    }
  else
    {
      g_assert_false (result);
      g_assert_error (error, G_IO_ERROR, scenario->error_code);
      if (scenario->error_code != G_IO_ERROR_CANCELLED)
        g_assert_cmpstr (error->message, ==, "Deployment failed");
    }
  if (scenario->with_callback)
    g_assert_cmpuint (check.callbacks, >=, 2);
  else
    g_assert_cmpuint (check.callbacks, ==, 0);
  g_cond_clear (&check.condition);
  g_mutex_clear (&check.mutex);
}

typedef struct
{
  const char *name;
  gboolean    with_progress;
  gboolean    already_cancelled;
  int         error_code;
} PullScenario;

typedef struct
{
  GThread         *caller;
  FlatpakProgress *progress;
  GCancellable    *cancellable;
  GMutex           mutex;
  GCond            condition;
  gboolean         released;
  gboolean         started;
  gboolean         finished;
  int              error_code;
  guint            last_percent;
} PullCheck;

static gboolean
pull (gpointer data, FlatpakProgress *progress, GCancellable *cancellable, GError **error)
{
  PullCheck *check = data;

  g_assert_true (g_thread_self () != check->caller);
  check->started = TRUE;
  flatpak_progress_start_oci_pull (progress);
  flatpak_progress_update_oci_pull (progress, 128, 64, 2, 1);
  flatpak_progress_init_extra_data (progress, 1, 64);
  flatpak_progress_update_extra_data (progress, 32);
  if (check->progress != NULL)
    {
      int64_t deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

      g_mutex_lock (&check->mutex);
      while (!check->released)
        if (!g_cond_wait_until (&check->condition, &check->mutex, deadline))
          break;
      g_assert_true (check->released);
      g_mutex_unlock (&check->mutex);
    }
  if (check->error_code == G_IO_ERROR_CANCELLED)
    g_assert_true (g_cancellable_set_error_if_cancelled (cancellable, error));
  else if (check->error_code >= 0)
    g_set_error_literal (error, G_IO_ERROR, check->error_code, "Pull failed");
  else
    {
      flatpak_progress_update_oci_pull (progress, 128, 128, 2, 2);
      flatpak_progress_update_extra_data (progress, 64);
    }
  check->finished = TRUE;
  return check->error_code < 0;
}

static void
pull_progress (const char *status, guint percent, gboolean estimating, gpointer data)
{
  PullCheck *check = data;

  g_assert_true (g_thread_self () == check->caller);
  g_assert_cmpuint (percent, >=, check->last_percent);
  check->last_percent = percent;
  if (flatpak_progress_get_transferred_extra_data_bytes (check->progress) < 32)
    return;
  g_assert_cmpuint (flatpak_progress_get_bytes_transferred (check->progress), >=, 64);
  if (check->error_code == G_IO_ERROR_CANCELLED)
    g_cancellable_cancel (check->cancellable);
  g_mutex_lock (&check->mutex);
  check->released = TRUE;
  g_cond_signal (&check->condition);
  g_mutex_unlock (&check->mutex);
}

static void
test_serial_pull (gconstpointer data)
{
  const PullScenario *scenario = data;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(FlatpakProgress) progress = NULL;
  g_autoptr(GError) error = NULL;
  PullCheck check = { .caller = g_thread_self (), .cancellable = cancellable,
                      .error_code = scenario->error_code };
  gboolean result;

  g_mutex_init (&check.mutex);
  g_cond_init (&check.condition);
  if (scenario->with_progress)
    {
      progress = flatpak_progress_new (pull_progress, &check);
      flatpak_progress_set_update_interval (progress, 1);
      check.progress = progress;
    }
  if (scenario->already_cancelled)
    g_cancellable_cancel (cancellable);

  result = flatpak_pull_job_run_serial ("test-ref", pull, &check, progress, cancellable, &error);
  if (scenario->already_cancelled)
    g_assert_false (check.started);
  else
    g_assert_true (check.finished);
  if (scenario->error_code < 0)
    {
      g_assert_true (result);
      g_assert_no_error (error);
      if (progress != NULL)
        {
          g_assert_cmpuint (flatpak_progress_get_bytes_transferred (progress), ==, 128);
          g_assert_cmpuint (flatpak_progress_get_transferred_extra_data_bytes (progress), ==, 64);

          /* A second pull must include both counters from the first download. */
          check.released = FALSE;
          check.finished = FALSE;
          g_assert_true (flatpak_pull_job_run_serial ("test-ref", pull, &check, progress, cancellable, &error));
          g_assert_no_error (error);
          g_assert_true (check.finished);
          g_assert_cmpuint (flatpak_progress_get_bytes_transferred (progress), ==, 320);
          g_assert_cmpuint (flatpak_progress_get_transferred_extra_data_bytes (progress), ==, 64);
        }
    }
  else
    {
      g_assert_false (result);
      g_assert_error (error, G_IO_ERROR, scenario->error_code);
      if (scenario->error_code != G_IO_ERROR_CANCELLED)
        g_assert_cmpstr (error->message, ==, "Pull failed");
    }
  g_cond_clear (&check.condition);
  g_mutex_clear (&check.mutex);
}

typedef struct
{
  GThread *caller;
  GMutex   mutex;
  GCond    condition;
  size_t   callbacks;
} ThrottledPullCheck;

static gboolean
pull_with_frequent_updates (gpointer data, FlatpakProgress *progress,
                            GCancellable *cancellable, GError **error)
{
  ThrottledPullCheck *check = data;
  int64_t deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  g_assert_true (g_thread_self () != check->caller);
  g_mutex_lock (&check->mutex);
  while (check->callbacks == 0)
    if (!g_cond_wait_until (&check->condition, &check->mutex, deadline))
      break;
  g_assert_cmpuint (check->callbacks, ==, 1);
  g_mutex_unlock (&check->mutex);

  flatpak_progress_start_oci_pull (progress);
  for (size_t i = 1; i <= 100; i++)
    {
      flatpak_progress_update_oci_pull (progress, 100, i, 100, i);
      g_usleep (G_TIME_SPAN_MILLISECOND);
    }
  return TRUE;
}

static void
throttled_pull_progress (const char *status, guint percent, gboolean estimating, gpointer data)
{
  ThrottledPullCheck *check = data;

  g_assert_true (g_thread_self () == check->caller);
  g_mutex_lock (&check->mutex);
  check->callbacks++;
  g_cond_signal (&check->condition);
  g_mutex_unlock (&check->mutex);
}

static void
test_pull_throttling (void)
{
  ThrottledPullCheck check = { .caller = g_thread_self () };
  g_autoptr(FlatpakProgress) progress = flatpak_progress_new (throttled_pull_progress, &check);
  g_autoptr(GError) error = NULL;

  g_mutex_init (&check.mutex);
  g_cond_init (&check.condition);
  /* The final snapshot must arrive without waiting for the next interval. */
  flatpak_progress_set_update_interval (progress, 60000);
  g_assert_true (flatpak_pull_job_run_serial ("test-ref", pull_with_frequent_updates,
                                             &check, progress, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (check.callbacks, ==, 2);
  g_assert_cmpuint (flatpak_progress_get_bytes_transferred (progress), ==, 100);
  g_cond_clear (&check.condition);
  g_mutex_clear (&check.mutex);
}

int
main (int argc, char **argv)
{
  const DeploymentScenario scenarios[] = {
    { "/progress/deployment/without-progress", FALSE, FALSE, -1 },
    { "/progress/deployment/without-callback", TRUE, FALSE, G_IO_ERROR_FAILED },
    { "/progress/deployment/worker-success", TRUE, TRUE, -1 },
    { "/progress/deployment/worker-error", TRUE, TRUE, G_IO_ERROR_FAILED },
    { "/progress/deployment/worker-cancel", TRUE, TRUE, G_IO_ERROR_CANCELLED },
  };
  const PullScenario pull_scenarios[] = {
    { "/progress/pull/without-progress", FALSE, FALSE, -1 },
    { "/progress/pull/success", TRUE, FALSE, -1 },
    { "/progress/pull/error", TRUE, FALSE, G_IO_ERROR_FAILED },
    { "/progress/pull/cancel", TRUE, FALSE, G_IO_ERROR_CANCELLED },
    { "/progress/pull/already-cancelled", TRUE, TRUE, G_IO_ERROR_CANCELLED },
  };

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/progress/pull/throttling", test_pull_throttling);
  for (size_t i = 0; i < G_N_ELEMENTS (scenarios); i++)
    g_test_add_data_func (scenarios[i].name, &scenarios[i], test_deployment);
  for (size_t i = 0; i < G_N_ELEMENTS (pull_scenarios); i++)
    g_test_add_data_func (pull_scenarios[i].name, &pull_scenarios[i], test_serial_pull);
  return g_test_run ();
}
