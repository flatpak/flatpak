/*
 * Copyright 2019-2021 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

extern gchar *cannot_use_fuse;
gboolean check_fuse (void);
gboolean check_fuse_or_skip_test (void);
