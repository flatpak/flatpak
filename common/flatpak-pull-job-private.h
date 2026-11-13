/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include "flatpak-dir-private.h"
#include "flatpak-progress-private.h"

typedef struct _FlatpakPullJob FlatpakPullJob;
typedef void (*FlatpakPullJobUpdate) (FlatpakPullJob *job,
                                      gpointer        user_data);

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
gboolean flatpak_pull_job_wait (FlatpakPullJob      *self,
                                FlatpakPullJobUpdate update,
                                gpointer             user_data,
                                GError             **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakPullJob, flatpak_pull_job_free)
