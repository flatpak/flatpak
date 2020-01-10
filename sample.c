#include "sample.h"

int
main (int argc,
      char *argv[])
{
  GVariant *v;
  Test t;
  Var vv;
  const char *res;
  variant resv;

  v = g_variant_new_parsed ("([32, 22], 's', uint16 16, ('s2', 322), (323,), 324, <(int16 67, 1023)>, [(int16 68, 1025), (int16 69, 1026)]"
                            ", {1:2, 3:4}, {'foo': <1>, 'bar': <'s'>}, {1:'a', 3:'b'}"
                            ")");
  g_print ("v: %s\n", g_variant_print (v, FALSE));
  g_print ("v2: %s\n", g_variant_print (v, TRUE));

  g_assert (g_variant_type_equal (g_variant_get_type(v), Test_typeformat));

  t = Test_from_gvariant (v);
  g_print ("t: %s\n", Test_print (t, FALSE));
  g_print ("t2: %s\n", Test_print (t, TRUE));
  //TODO: g_assert_cmpstr (g_variant_print (v, FALSE), ==, Test_print (t, FALSE));

  vv = Var_from_variant(Test_get_v(t));
  g_print ("t.v: %s\n", Var_print (vv, TRUE));

  Test__d2 d2 = Test_get_d2(t);
  g_print ("t.d2: %s\n", Test__d2_print(d2, FALSE));
  GVariant *v3 = Test__d2_dup_to_gvariant(d2);
  g_print ("t.d2 as gvariant: %s\n", g_variant_print (v3, TRUE));

  if (Test__d2_lookup(d2, 1, &res))
    g_print ("t.d2.lookup(1): %s\n", res);
  if (Test__d2_lookup(d2, 3, &res))
    g_print ("t.d2.lookup(3): %s\n", res);
  if (Test__d2_lookup(d2, 2, &res))
    g_print ("t.d2.lookup(2): didn't fail\n");

  Metadata meta = Test_get_meta(t);
  g_print ("meta: %s\n", Metadata_print(meta, FALSE));
  GVariant *meta_v = Metadata_dup_to_gvariant(meta);
  g_print ("meta as gvariant: %s\n", g_variant_print (meta_v, TRUE));

  if (Metadata_lookup(meta, "foo", &resv))
    g_print ("t.meta[\"foo\"]=%s\n", variant_print (resv, TRUE));
  if (Metadata_lookup(meta, "bar", &resv))
    g_print ("t.meta[\"bar\"]=%s\n", variant_print (resv, TRUE));
  if (Metadata_lookup(meta, "missing", &resv))
    g_print ("t.meta[\"missing\"] didn't fail!\n");
}
