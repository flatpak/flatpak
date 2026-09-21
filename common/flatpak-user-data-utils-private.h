#pragma once

#include <gio/gio.h>

gboolean flatpak_user_data_delete (const char  *app_id,
                                  GError     **error);
