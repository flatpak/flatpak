/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include "flatpak-progress-private.h"

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

  g_test_init (&argc, &argv, NULL);
  for (size_t i = 0; i < G_N_ELEMENTS (scenarios); i++)
    g_test_add_data_func (scenarios[i].name, &scenarios[i], test_deployment);
  return g_test_run ();
}
