#include "sample.h"

void
test_sample_variant (GVariant *v)
{
  Test t;
  Var var;
  GVariant *varv;
  const char *res;
  variant resv;

  g_print ("sample: %s\n", g_variant_print (v, FALSE));
  g_print ("sample with types: %s\n", g_variant_print (v, TRUE));

  g_assert (g_variant_type_equal (g_variant_get_type(v), Test_typeformat));

  t = Test_from_gvariant (v);
  g_print ("custom: %s\n", Test_print (t, FALSE));
  g_print ("custom with types: %s\n", Test_print (t, TRUE));

  g_assert_cmpstr (g_variant_print (v, FALSE), ==, Test_print (t, FALSE));
  g_assert_cmpstr (g_variant_print (v, TRUE), ==, Test_print (t, TRUE));

  var = Var_from_variant(Test_get_v(t));
  varv = g_variant_get_variant (g_variant_get_child_value (v, Test_indexof_v));
  g_assert_cmpstr (g_variant_print (varv, TRUE), ==, Var_print (var, TRUE));

  D2 d2 = Test_get_d2(t);
  GVariant *v3 = D2_dup_to_gvariant(d2);
  g_assert_cmpstr (g_variant_print (v3, TRUE), ==, D2_print(d2, FALSE));

  g_assert (D2_lookup(d2, 1, &res));
  g_assert_cmpstr (res, ==, "a");
  g_assert (D2_lookup(d2, 3, &res));
  g_assert_cmpstr (res, ==, "b");
  g_assert (!D2_lookup(d2, 2, &res));

  Metadata meta = Test_get_meta(t);
  g_print ("meta: %s\n", Metadata_print(meta, FALSE));
  GVariant *meta_v = Metadata_dup_to_gvariant(meta);

  g_assert (g_variant_type_equal (g_variant_get_type(meta_v), Metadata_typeformat));
  g_assert_cmpstr (g_variant_print (meta_v, FALSE), ==, Metadata_print (meta, FALSE));
  g_assert_cmpstr (g_variant_print (meta_v, TRUE), ==, Metadata_print (meta, TRUE));

  g_assert (Metadata_lookup(meta, "foo", &resv));
  g_assert_cmpstr ("<1>", ==, variant_print (resv, TRUE));
  g_assert (Metadata_lookup(meta, "bar", &resv));
  g_assert_cmpstr ("<'s'>", ==, variant_print (resv, TRUE));
  g_assert (!Metadata_lookup(meta, "missing", &resv));
}

int
main (int argc,
      char *argv[])
{
  GVariant *v;

#define DATA \
  "([32, 22], '%s', uint16 16, "                                        \
    "('s2', 322), ('ssss2', 3222), (323,), 324, <(int16 67, 1023)>, "   \
    "[(int16 68, 1025), (int16 69, 1026)]"                              \
    ", {1:2, 3:4}, {'foo': <1>, 'bar': <'s'>}, {1:'a', 3:'b'}"          \
    ")"

  v = g_variant_new_parsed (g_strdup_printf (DATA, "s"));

  /* Try with larger offsets */
  v = g_variant_new_parsed (g_strdup_printf (DATA, "sxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
  test_sample_variant(v);
}
