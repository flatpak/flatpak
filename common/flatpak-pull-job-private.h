/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include "flatpak-dir-private.h"
#include "flatpak-progress-private.h"

typedef struct _FlatpakPullJob FlatpakPullJob;
typedef struct
{
  FlatpakProgress *progress;
  FlatpakProgress *snapshot;
  int64_t          last_emission_us;
  uint64_t         previous_bytes;
} FlatpakPullProgress;

typedef void (*FlatpakPullJobUpdate) (FlatpakPullJob *job,
                                      gpointer        user_data);
typedef gboolean (*FlatpakPullJobFunc) (gpointer         data,
                                        FlatpakProgress *progress,
                                        GCancellable    *cancellable,
                                        GError         **error);

/* Borrows data exclusively until the worker has joined. Progress callbacks run
 * on the caller thread; source-specific locks and cleanup stay with the caller. */
gboolean flatpak_pull_job_run_serial (const char         *ref,
                                     FlatpakPullJobFunc   pull,
                                     gpointer             data,
                                     FlatpakProgress     *progress,
                                     GCancellable        *cancellable,
                                     GError             **error);

/* The caller checks remote/ref eligibility. Returns NULL if worker repository
 * verification requires a serial pull. */
FlatpakPullJob *flatpak_pull_job_new (FlatpakDir         *dir,
                                      FlatpakRemoteState *state,
                                      const char         *ref,
                                      const char         *commit,
                                      const char * const *subpaths,
                                      GBytes             *metadata,
                                      const char         *token,
                                      FlatpakPullFlags    flags,
                                      GCancellable       *cancellable);
void flatpak_pull_job_cancel (FlatpakPullJob *self);
/* Stops the worker but keeps its snapshot and prune lock until free(). */
void flatpak_pull_job_cancel_and_join (FlatpakPullJob *self);
void flatpak_pull_job_free (FlatpakPullJob *self);
const char **flatpak_pull_job_get_subpaths (FlatpakPullJob *self);
/* Copies a synchronized progress snapshot and returns whether the pull ended. */
gboolean flatpak_pull_job_snapshot (FlatpakPullJob  *self,
                                    FlatpakProgress *progress);
/* Delivers snapshots at the recipient's update interval, always including the
 * first and final update. Returns whether the pull ended. */
gboolean flatpak_pull_job_update_progress (FlatpakPullJob      *self,
                                          FlatpakPullProgress *update);
gboolean flatpak_pull_job_wait (FlatpakPullJob      *self,
                                FlatpakPullJobUpdate update,
                                gpointer             user_data,
                                GError             **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakPullJob, flatpak_pull_job_free)
