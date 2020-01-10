#include "sample.h"

void
test_sample_variant (GVariant *v)
{
  SampleTest t;
  SampleVar var;
  GVariant *varv;
  const char *res;
  SampleVariant resv;

  g_print ("sample type: %s\n", g_variant_get_type_string (v));
  g_print ("sample: %s\n", g_variant_print (v, FALSE));
  g_print ("sample with types: %s\n", g_variant_print (v, TRUE));

  g_assert (g_variant_type_equal (g_variant_get_type(v), SampleTest_typeformat));

  t = SampleTest_from_gvariant (v);
  g_print ("custom: %s\n", SampleTest_print (t, FALSE));
  g_print ("custom with types: %s\n", SampleTest_print (t, TRUE));

  g_assert_cmpstr (g_variant_print (v, FALSE), ==, SampleTest_print (t, FALSE));
  g_assert_cmpstr (g_variant_print (v, TRUE), ==, SampleTest_print (t, TRUE));

  var = SampleVar_from_variant(SampleTest_get_v(t));
  varv = g_variant_get_variant (g_variant_get_child_value (v, SampleTest_indexof_v));
  g_assert_cmpstr (g_variant_print (varv, TRUE), ==, SampleVar_print (var, TRUE));

  SampleD2 d2 = SampleTest_get_d2(t);
  GVariant *v3 = SampleD2_dup_to_gvariant(d2);
  g_assert_cmpstr (g_variant_print (v3, TRUE), ==, SampleD2_print(d2, FALSE));

  g_assert (SampleD2_lookup(d2, 1, &res));
  g_assert_cmpstr (res, ==, "a");
  g_assert (SampleD2_lookup(d2, 3, &res));
  g_assert_cmpstr (res, ==, "b");
  g_assert (!SampleD2_lookup(d2, 2, &res));

  SampleMetadata meta = SampleTest_get_meta(t);
  GVariant *meta_v = SampleMetadata_dup_to_gvariant(meta);

  g_assert (g_variant_type_equal (g_variant_get_type(meta_v), SampleMetadata_typeformat));
  g_assert_cmpstr (g_variant_print (meta_v, FALSE), ==, SampleMetadata_print (meta, FALSE));
  g_assert_cmpstr (g_variant_print (meta_v, TRUE), ==, SampleMetadata_print (meta, TRUE));

  g_assert (SampleMetadata_lookup(meta, "foo", &resv));
  g_assert_cmpstr ("<1>", ==, SampleVariant_print (resv, TRUE));
  g_assert (SampleMetadata_lookup(meta, "bar", &resv));
  g_assert_cmpstr ("<'s'>", ==, SampleVariant_print (resv, TRUE));
  g_assert (!SampleMetadata_lookup(meta, "missing", &resv));
}

int
main (int argc,
      char *argv[])
{
  GVariant *v;

#define DATA \
  "([32, 22], '%s', uint16 16, "                                        \
    "('s2', 322), ('ssss2', 3222), (323,), 324, "                       \
    "<(int16 67, 1023, byte 3)>, "                                          \
    "[(int16 68, 1025, byte 42), (int16 69, 1026, byte 42)]"                              \
    ", {1:2, 3:4}, {'foo': <1>, 'bar': <'s'>}, {1:'a', 3:'b'}, "        \
    "just (objectpath '/', signature 's', true, handle 3, int64 88, uint64 89, 3.1415 )"             \
    ")"

  v = g_variant_new_parsed (g_strdup_printf (DATA, "s"));
  test_sample_variant(v);

  /* Try with larger offsets */
  v = g_variant_new_parsed (g_strdup_printf (DATA, "sxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  test_sample_variant(v);
}
