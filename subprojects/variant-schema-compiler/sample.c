#define SAMPLE_DEEP_VARIANT_FORMAT
#include "sample.h"
#include "sample-impl.h"

void
test_sample_variant (GVariant *v)
{
  SampleTestRef t;
  SampleVarRef var;
  GVariant *varv;
  const char *res;
  gint32 resi;
  SampleVariantRef resv;

  g_print ("sample type: %s\n", g_variant_get_type_string (v));
  g_print ("sample: %s\n", g_variant_print (v, FALSE));
  g_print ("sample with types: %s\n", g_variant_print (v, TRUE));

  g_assert (g_variant_type_equal (g_variant_get_type(v), SAMPLE_TEST_TYPEFORMAT));

  t = sample_test_from_gvariant (v);
  g_print ("custom: %s\n", sample_test_print (t, FALSE));
  g_print ("custom with types: %s\n", sample_test_print (t, TRUE));

  g_assert_cmpstr (g_variant_print (v, FALSE), ==, sample_test_print (t, FALSE));
  g_assert_cmpstr (g_variant_print (v, TRUE), ==, sample_test_print (t, TRUE));

  var = sample_var_from_variant(sample_test_get_v(t));
  varv = g_variant_get_variant (g_variant_get_child_value (v, SAMPLE_TEST_INDEXOF_V));
  g_assert_cmpstr (g_variant_print (varv, TRUE), ==, sample_var_print (var, TRUE));

  SampleTestD1Ref d1 = sample_test_get_d1(t);
  GVariant *v2 = sample_test_d1_dup_to_gvariant(d1);
  g_assert_cmpstr (g_variant_print (v2, TRUE), ==, sample_test_d1_print(d1, FALSE));

  g_assert (!sample_test_d1_lookup(d1, 0,NULL , &resi));
  g_assert (sample_test_d1_lookup(d1, 1,NULL , &resi));
  g_assert_cmpint (resi, ==, 2);
  g_assert (!sample_test_d1_lookup(d1, 2,NULL , &resi));
  g_assert (sample_test_d1_lookup(d1, 3,NULL , &resi));
  g_assert_cmpint (resi, ==, 4);
  g_assert (!sample_test_d1_lookup(d1, 4,NULL , &resi));
  g_assert (sample_test_d1_lookup(d1, 5,NULL , &resi));
  g_assert_cmpint (resi, ==, 6);
  g_assert (!sample_test_d1_lookup(d1, 6,NULL , &resi));

  SampleD1sRef d1s = sample_test_get_d1s(t);
  GVariant *v2s = sample_d1s_dup_to_gvariant(d1s);
  g_assert_cmpstr (g_variant_print (v2s, TRUE), ==, sample_d1s_print(d1s, FALSE));

  g_assert (!sample_d1s_lookup(d1s, 0,NULL , &resi));
  g_assert (sample_d1s_lookup(d1s, 1,NULL , &resi));
  g_assert_cmpint (resi, ==, 2);
  g_assert (!sample_d1s_lookup(d1s, 2,NULL , &resi));
  g_assert (sample_d1s_lookup(d1s, 3,NULL , &resi));
  g_assert_cmpint (resi, ==, 4);
  g_assert (!sample_d1s_lookup(d1s, 4,NULL , &resi));
  g_assert (sample_d1s_lookup(d1s, 5,NULL , &resi));
  g_assert_cmpint (resi, ==, 6);
  g_assert (!sample_d1s_lookup(d1s, 6,NULL , &resi));

  SampleD2Ref d2 = sample_test_get_d2(t);
  GVariant *v3 = sample_d2_dup_to_gvariant(d2);
  g_assert_cmpstr (g_variant_print (v3, TRUE), ==, sample_d2_print(d2, FALSE));

  g_assert (sample_d2_lookup(d2, 1,NULL , &res));
  g_assert_cmpstr (res, ==, "a");
  g_assert (sample_d2_lookup(d2, 3,NULL , &res));
  g_assert_cmpstr (res, ==, "b");
  g_assert (!sample_d2_lookup(d2, 2,NULL , &res));

  SampleMetadataRef meta = sample_test_get_meta(t);
  GVariant *meta_v = sample_metadata_dup_to_gvariant(meta);

  g_assert (g_variant_type_equal (g_variant_get_type(meta_v), SAMPLE_METADATA_TYPEFORMAT));
  g_assert_cmpstr (g_variant_print (meta_v, FALSE), ==, sample_metadata_print (meta, FALSE));
  g_assert_cmpstr (g_variant_print (meta_v, TRUE), ==, sample_metadata_print (meta, TRUE));

  g_assert (sample_metadata_lookup(meta, "bar",NULL , &resv));
  g_assert_cmpstr ("<1>", ==, sample_variant_print (resv, TRUE));
  g_assert (sample_metadata_lookup(meta, "foo",NULL , &resv));
  g_assert_cmpstr ("<'s'>", ==, sample_variant_print (resv, TRUE));
  g_assert (!sample_metadata_lookup(meta, "missing",NULL , &resv));

  SampleSortedMetadataRef metas = sample_test_get_metas(t);
  GVariant *metas_v = sample_sorted_metadata_dup_to_gvariant(metas);

  g_assert (g_variant_type_equal (g_variant_get_type(metas_v), SAMPLE_SORTED_METADATA_TYPEFORMAT));
  g_assert_cmpstr (g_variant_print (metas_v, FALSE), ==, sample_sorted_metadata_print (metas, FALSE));
  g_assert_cmpstr (g_variant_print (metas_v, TRUE), ==, sample_sorted_metadata_print (metas, TRUE));

  g_assert (!sample_sorted_metadata_lookup(metas, "aaa",NULL , &resv));
  g_assert (sample_sorted_metadata_lookup(metas, "bar",NULL , &resv));
  g_assert_cmpstr ("<1>", ==, sample_variant_print (resv, TRUE));
  g_assert (!sample_sorted_metadata_lookup(metas, "ccc",NULL , &resv));
  g_assert (sample_sorted_metadata_lookup(metas, "foo",NULL , &resv));
  g_assert_cmpstr ("<'s'>", ==, sample_variant_print (resv, TRUE));
  g_assert (!sample_sorted_metadata_lookup(metas, "dddmissing",NULL , &resv));
}

int
main (int argc,
      char *argv[])
{
  GVariant *v;

#define DATA \
  "([32, 22], '%s', uint16 16, "                                        \
    "('s2', 322), ('ssss2', 3222), (323,), 324, "                       \
    "<(int16 67, 1023, byte 3, (uint16 5, byte 6))>, "                                          \
    "[(int16 68, 1025, byte 42, (uint16 7, byte 8)), (int16 69, 1026, byte 42, (uint16 9, byte 11))]"                              \
    ", {1:2, 3:4, 5:6}, {1:2, 3:4, 5:6}, {'bar': <1>, 'foo': <'s'>}, {'bar': <1>, 'foo': <'s'>}, {1:'a', 3:'b'}, "        \
    "just (objectpath '/', signature 's', true, handle 3, int64 88, uint64 89, 3.1415 )"             \
    ")"

  v = g_variant_new_parsed (g_strdup_printf (DATA, "s"));
  test_sample_variant(v);

  /* Try with larger offsets */
  v = g_variant_new_parsed (g_strdup_printf (DATA, "sxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  test_sample_variant(v);
}
