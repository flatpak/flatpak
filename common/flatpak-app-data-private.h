#pragma once

#include <gio/gio.h>

gboolean flatpak_delete_app_data           (const char              *app_id,
                                             GError                 **error);
