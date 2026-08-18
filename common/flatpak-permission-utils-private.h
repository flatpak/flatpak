#pragma once

#include <gio/gio.h>

#include "flatpak-permission-dbus-generated.h"

char **  get_permission_tables             (void);
gboolean flatpak_reset_permissions_for_app (const char *app_id,
                                             GError    **error);
