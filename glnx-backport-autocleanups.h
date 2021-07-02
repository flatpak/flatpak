/*
 * Copyright © 2015 Canonical Limited
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the licence, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Ryan Lortie <desrt@desrt.ca>
 */

#pragma once

#include <glnx-backport-autoptr.h>

#if !GLIB_CHECK_VERSION(2, 43, 4)

static inline void
g_autoptr_cleanup_generic_gfree (void *p)
{ 
  void **pp = (void**)p;
  if (*pp)
    g_free (*pp);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAsyncQueue, g_async_queue_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GBookmarkFile, g_bookmark_file_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GBytes, g_bytes_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GChecksum, g_checksum_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDateTime, g_date_time_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDir, g_dir_close)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GError, g_error_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GHashTable, g_hash_table_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GHmac, g_hmac_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GIOChannel, g_io_channel_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GKeyFile, g_key_file_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GList, g_list_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GArray, g_array_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPtrArray, g_ptr_array_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMainContext, g_main_context_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMainLoop, g_main_loop_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSource, g_source_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMappedFile, g_mapped_file_unref)
#if GLIB_CHECK_VERSION(2, 36, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMarkupParseContext, g_markup_parse_context_unref)
#endif
G_DEFINE_AUTOPTR_CLEANUP_FUNC(gchar, g_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GNode, g_node_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GOptionContext, g_option_context_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GOptionGroup, g_option_group_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GPatternSpec, g_pattern_spec_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GQueue, g_queue_free)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GQueue, g_queue_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GRand, g_rand_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GRegex, g_regex_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMatchInfo, g_match_info_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GScanner, g_scanner_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSequence, g_sequence_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSList, g_slist_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GStringChunk, g_string_chunk_free)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(GStrv, g_strfreev, NULL)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GThread, g_thread_unref)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GMutex, g_mutex_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GCond, g_cond_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTimer, g_timer_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTimeZone, g_time_zone_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTree, g_tree_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariant, g_variant_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantBuilder, g_variant_builder_unref)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GVariantBuilder, g_variant_builder_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantIter, g_variant_iter_free)
#if GLIB_CHECK_VERSION(2, 40, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantDict, g_variant_dict_unref)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GVariantDict, g_variant_dict_clear)
#endif
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVariantType, g_variant_type_free)
#if GLIB_CHECK_VERSION(2, 40, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSubprocess, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSubprocessLauncher, g_object_unref)
#endif

/* Add GObject-based types as needed. */
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GAsyncResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GCancellable, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GConverter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GConverterOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDataInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFile, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileEnumerator, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileIOStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileInfo, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileMonitor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GFileOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMemoryInputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMemoryOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMount, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GOutputStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocket, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketAddress, g_object_unref)
#if GLIB_CHECK_VERSION(2, 36, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTask, g_object_unref)
#endif
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsCertificate, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsDatabase, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GTlsInteraction, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusConnection, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GDBusMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GVolumeMonitor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GZlibCompressor, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GZlibDecompressor, g_object_unref)

#endif

#if !GLIB_CHECK_VERSION(2, 45, 8)

static inline void
g_autoptr_cleanup_gstring_free (GString *string)
{
  if (string)
    g_string_free (string, TRUE);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GString, g_autoptr_cleanup_gstring_free)

#endif
