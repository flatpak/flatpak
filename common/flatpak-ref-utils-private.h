/*
 * Copyright © 2020 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 */

#include <flatpak-common-types-private.h>
#include <flatpak-ref.h>

#ifndef __FLATPAK_REF_UTILS_H__
#define __FLATPAK_REF_UTILS_H__

gboolean flatpak_is_valid_name (const char *string,
                                gssize      len,
                                GError    **error);
gboolean flatpak_is_valid_branch (const char *string,
                                  gssize      len,
                                  GError    **error);
gboolean flatpak_is_valid_arch (const char *string,
                                gssize      len,
                                GError    **error);
gboolean flatpak_has_name_prefix (const char *string,
                                  const char *name);
gboolean flatpak_name_matches_one_wildcard_prefix (const char         *string,
                                                   const char * const *maybe_wildcard_prefixes,
                                                   gboolean            require_exact_match);

char * flatpak_make_valid_id_prefix (const char *orig_id);
gboolean flatpak_id_has_subref_suffix (const char *id,
                                       gssize      id_len);

char **flatpak_decompose_ref (const char *ref,
                              GError    **error);
char * flatpak_get_arch_for_ref (const char *ref);
const char *flatpak_get_compat_arch_reverse (const char *compat_arch);

FlatpakKinds flatpak_kinds_from_kind (FlatpakRefKind kind);

typedef struct _FlatpakDecomposed FlatpakDecomposed;
FlatpakDecomposed *flatpak_decomposed_new_from_ref          (const char         *ref,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_new_from_col_ref      (const char         *ref,
                                                             const char         *collection_id,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_new_from_refspec      (const char         *refspec,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_new_from_ref_take     (char               *ref,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_new_from_refspec_take (char               *refspec,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_new_from_parts        (FlatpakKinds        kind,
                                                             const char         *id,
                                                             const char         *arch,
                                                             const char         *branch,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_new_from_decomposed   (FlatpakDecomposed  *ref,
                                                             FlatpakKinds        opt_kind,
                                                             const char         *opt_id,
                                                             const char         *opt_arch,
                                                             const char         *opt_branch,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_new_from_pref         (FlatpakKinds        kind,
                                                             const char         *pref,
                                                             GError            **error);
FlatpakDecomposed *flatpak_decomposed_ref                   (FlatpakDecomposed  *ref);
void               flatpak_decomposed_unref                 (FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_get_ref               (FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_get_refspec           (FlatpakDecomposed  *ref);
char *             flatpak_decomposed_dup_ref               (FlatpakDecomposed  *ref);
char *             flatpak_decomposed_dup_refspec           (FlatpakDecomposed  *ref);
char *             flatpak_decomposed_dup_remote            (FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_get_collection_id     (FlatpakDecomposed  *ref);
char *             flatpak_decomposed_dup_collection_id     (FlatpakDecomposed  *ref);
gboolean           flatpak_decomposed_equal                 (FlatpakDecomposed  *ref_a,
                                                             FlatpakDecomposed  *ref_b);
gint               flatpak_decomposed_strcmp                (FlatpakDecomposed  *ref_a,
                                                             FlatpakDecomposed  *ref_b);
gint               flatpak_decomposed_strcmp_p              (FlatpakDecomposed **ref_a,
                                                             FlatpakDecomposed **ref_b);
guint              flatpak_decomposed_hash                  (FlatpakDecomposed  *ref);
gboolean           flatpak_decomposed_is_app                (FlatpakDecomposed  *ref);
gboolean           flatpak_decomposed_is_runtime            (FlatpakDecomposed  *ref);
FlatpakKinds       flatpak_decomposed_get_kinds             (FlatpakDecomposed  *ref);
FlatpakRefKind     flatpak_decomposed_get_kind              (FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_get_kind_str          (FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_get_kind_metadata_group(FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_get_pref              (FlatpakDecomposed  *ref);
char *             flatpak_decomposed_dup_pref              (FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_peek_id               (FlatpakDecomposed  *ref,
                                                             gsize              *out_len);
char *             flatpak_decomposed_dup_id                (FlatpakDecomposed  *ref);
char *             flatpak_decomposed_dup_readable_id       (FlatpakDecomposed  *ref);
gboolean           flatpak_decomposed_is_id                 (FlatpakDecomposed  *ref,
                                                             const char         *id);
gboolean           flatpak_decomposed_id_has_suffix         (FlatpakDecomposed  *ref,
                                                             const char         *suffix);
gboolean           flatpak_decomposed_is_id_fuzzy           (FlatpakDecomposed  *ref,
                                                             const char         *id);
gboolean           flatpak_decomposed_id_is_subref          (FlatpakDecomposed  *ref);
const char *       flatpak_decomposed_peek_arch             (FlatpakDecomposed  *ref,
                                                             gsize              *out_len);
char *             flatpak_decomposed_dup_arch              (FlatpakDecomposed  *ref);
gboolean           flatpak_decomposed_is_arch               (FlatpakDecomposed  *ref,
                                                             const char         *arch);
gboolean           flatpak_decomposed_is_arches             (FlatpakDecomposed  *ref,
                                                             const char        **arches);
const char *       flatpak_decomposed_peek_branch           (FlatpakDecomposed  *ref,
                                                             gsize              *out_len);
const char *       flatpak_decomposed_get_branch            (FlatpakDecomposed  *ref);
char *             flatpak_decomposed_dup_branch            (FlatpakDecomposed  *ref);
gboolean           flatpak_decomposed_is_branch             (FlatpakDecomposed  *ref,
                                                             const char         *branch);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDecomposed, flatpak_decomposed_unref)

FlatpakKinds flatpak_kinds_from_bools (gboolean app,
                                       gboolean runtime);

gboolean flatpak_split_partial_ref_arg (const char   *partial_ref,
                                        FlatpakKinds  default_kinds,
                                        const char   *default_arch,
                                        const char   *default_branch,
                                        FlatpakKinds *out_kinds,
                                        char        **out_id,
                                        char        **out_arch,
                                        char        **out_branch,
                                        GError      **error);
gboolean flatpak_split_partial_ref_arg_novalidate (const char   *partial_ref,
                                                   FlatpakKinds  default_kinds,
                                                   const char   *default_arch,
                                                   const char   *default_branch,
                                                   FlatpakKinds *out_kinds,
                                                   char        **out_id,
                                                   char        **out_arch,
                                                   char        **out_branch);

int flatpak_compare_ref (const char *ref1,
                         const char *ref2);

char * flatpak_compose_ref (gboolean    app,
                            const char *name,
                            const char *branch,
                            const char *arch,
                            GError    **error);

char * flatpak_build_untyped_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * flatpak_build_runtime_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * flatpak_build_app_ref (const char *app,
                              const char *branch,
                              const char *arch);


#endif /* __FLATPAK_REF_UTILS_H__ */
