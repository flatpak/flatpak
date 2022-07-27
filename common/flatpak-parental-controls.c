/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2018 Endless Mobile, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <glib.h>
#include <gio/gio.h>
#include <libmalcontent/app-filter.h>

#include "flatpak-parental-controls-private.h"

/*
 * See https://www.freedesktop.org/software/appstream/docs/chap-Metadata.html#tag-content_rating
 * for details of the appstream content rating specification.
 *
 * See https://hughsie.github.io/oars/ for details of OARS. Specifically,
 * https://github.com/hughsie/oars/tree/master/specification/.
 */

/* Convert an appstream <content_attribute/> value to #MctAppFilterOarsValue.
 * https://www.freedesktop.org/software/appstream/docs/chap-Metadata.html#tag-content_rating
 */
static MctAppFilterOarsValue
app_filter_oars_value_from_appdata (const gchar *appdata_value)
{
  g_return_val_if_fail (appdata_value != NULL, MCT_APP_FILTER_OARS_VALUE_UNKNOWN);

  if (g_str_equal (appdata_value, "intense"))
    return MCT_APP_FILTER_OARS_VALUE_INTENSE;
  else if (g_str_equal (appdata_value, "moderate"))
    return MCT_APP_FILTER_OARS_VALUE_MODERATE;
  else if (g_str_equal (appdata_value, "mild"))
    return MCT_APP_FILTER_OARS_VALUE_MILD;
  else if (g_str_equal (appdata_value, "none"))
    return MCT_APP_FILTER_OARS_VALUE_NONE;
  else if (g_str_equal (appdata_value, "unknown"))
    return MCT_APP_FILTER_OARS_VALUE_UNKNOWN;
  else
    return MCT_APP_FILTER_OARS_VALUE_UNKNOWN;
}

static const gchar *
app_filter_oars_value_to_string (MctAppFilterOarsValue oars_value)
{
  switch (oars_value)
    {
    case MCT_APP_FILTER_OARS_VALUE_UNKNOWN: return "unknown";
    case MCT_APP_FILTER_OARS_VALUE_INTENSE: return "intense";
    case MCT_APP_FILTER_OARS_VALUE_MODERATE: return "moderate";
    case MCT_APP_FILTER_OARS_VALUE_MILD: return "mild";
    case MCT_APP_FILTER_OARS_VALUE_NONE: return "none";
    default: return "unknown";
    }
}

/**
 * flatpak_oars_check_rating:
 * @content_rating: (nullable) (transfer none): OARS ratings for the app,
 *    or %NULL if none are known
 * @content_rating_type: (nullable): scheme used in @content_rating, such as
 *    `oars-1.0` or `oars-1.1`, or %NULL if @content_rating is %NULL
 * @filter: user’s parental controls settings
 *
 * Check whether the OARS rating in @content_rating is as, or less, extreme than
 * the user’s preferences in @filter. If so (i.e. if the app is suitable for
 * this user to use), return %TRUE; otherwise return %FALSE.
 *
 * @content_rating may be %NULL if no OARS ratings are provided for the app. If
 * so, we have to assume the most restrictive ratings.
 *
 * Returns: %TRUE if the app is safe to install, %FALSE otherwise
 */
gboolean
flatpak_oars_check_rating (GHashTable   *content_rating,
                           const gchar  *content_rating_type,
                           MctAppFilter *filter)
{
  const gchar * const supported_rating_types[] = { "oars-1.0", "oars-1.1", NULL };
  g_autofree const gchar **oars_sections = mct_app_filter_get_oars_sections (filter);
  MctAppFilterOarsValue default_rating_value;

  if (content_rating_type != NULL &&
      !g_strv_contains (supported_rating_types, content_rating_type))
    return FALSE;

  /* If the app has a <content_rating/> element, even if it has no OARS sections
   * in it, use a default value of `none` for any missing sections. Otherwise,
   * if the app has no <content_rating/> element, use `unknown`. */
  if (content_rating != NULL)
    default_rating_value = MCT_APP_FILTER_OARS_VALUE_NONE;
  else
    default_rating_value = MCT_APP_FILTER_OARS_VALUE_UNKNOWN;

  for (gsize i = 0; oars_sections[i] != NULL; i++)
    {
      MctAppFilterOarsValue rating_value;
      MctAppFilterOarsValue filter_value = mct_app_filter_get_oars_value (filter,
                                                                          oars_sections[i]);
      const gchar *appdata_value = NULL;

      if (content_rating != NULL)
        appdata_value = g_hash_table_lookup (content_rating, oars_sections[i]);

      if (appdata_value != NULL)
        rating_value = app_filter_oars_value_from_appdata (appdata_value);
      else
        rating_value = default_rating_value;

      if (filter_value < rating_value ||
          (rating_value == MCT_APP_FILTER_OARS_VALUE_UNKNOWN &&
           filter_value != MCT_APP_FILTER_OARS_VALUE_UNKNOWN) ||
          (rating_value != MCT_APP_FILTER_OARS_VALUE_UNKNOWN &&
           filter_value == MCT_APP_FILTER_OARS_VALUE_UNKNOWN))
        {
          g_debug ("%s: Comparing rating ‘%s’: app has ‘%s’ but policy has ‘%s’ unknown: OARS check failed",
                   G_STRFUNC, oars_sections[i],
                   app_filter_oars_value_to_string (rating_value),
                   app_filter_oars_value_to_string (filter_value));
          return FALSE;
        }
    }

  return TRUE;
}
