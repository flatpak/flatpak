#include "performance.h"

gint32
sum_gvariant (GVariant *v)
{
  guint32 sum;
  gint16 a;
  gint32 b;
  const char *c;
  GVariant *list;
  guchar d;
  guint16 tuple_a;
  guchar tuple_b;
  gsize len, i;
  gint32 list_a;
  guint16 list_b;

  g_variant_get (v, "(ni&s@a(iq)y(qy))",
                 &a, &b, &c, &list, &d, &tuple_a, &tuple_b);
  sum = a + b + strlen(c) + d + tuple_a + tuple_b;

  len = g_variant_n_children (list);
  for (i = 0; i < len; i++)
    {
      g_variant_get_child (list, i, "(iq)", &list_a, &list_b);
      sum += list_a + list_b;
    }

  return sum;
}

gint32
sum_generated (PerformanceContainerRef v)
{
  guint32 sum;
  PerformanceTupleRef tuple;
  PerformanceListRef list;
  PerformanceItemRef item;
  gsize len, i;

  tuple = performance_container_get_tuple (v);
  sum = performance_container_get_a(v) +
    performance_container_get_b(v) +
    strlen(performance_container_get_c(v)) +
    performance_container_get_d(v) +
    performance_tuple_get_a (tuple) +
    performance_tuple_get_b (tuple);

  list = performance_container_get_list (v);
  len = performance_list_get_length (list);
  for (i = 0; i < len; i++)
    {
     item = performance_list_get_at (list, i);
     sum += performance_item_get_a (item) + performance_item_get_b (item);
    }

  return sum;
}


int
main (int argc,
      char *argv[])
{
  GVariant *v;
  gconstpointer serialized_data;
  int i, count = 100000;
  guint64 total;
  GTimer *timer = g_timer_new ();
  PerformanceContainerRef c;

#define DATA "(int16 17, 32, 'foobar', [(44, uint16 12), (48, uint16 14), (99, uint16 100)], byte 128, (uint16 4, byte 11))"

  v = g_variant_new_parsed (DATA);
  g_assert (g_variant_type_equal (g_variant_get_type (v), PERFORMANCE_CONTAINER_TYPEFORMAT));

  /* Ensure data is serialized */
  serialized_data = g_variant_get_data (v);

  /* Warmup */
  total = 0;
  for (i = 0; i < 10; i++)
    total += sum_gvariant (v);
  g_assert (total == 10 * 515);

  total = 0;
  g_timer_start (timer);
  for (i = 0; i < count; i++)
    {
      total += sum_gvariant (v);
      total += sum_gvariant (v);
      total += sum_gvariant (v);
      total += sum_gvariant (v);
      total += sum_gvariant (v);
    }
  g_timer_stop (timer);
  g_assert (total == 5 * count * 515);

  g_print ("GVariant performance: %.1f kiloiterations per second\n", (count/1000.0)/g_timer_elapsed (timer, NULL));

  c = performance_container_from_gvariant (v);

  /* Warmup */
  total = 0;
  for (i = 0; i < 10; i++)
    total += sum_generated (c);
  g_assert (total == 10 * 515);

  g_timer_reset (timer);

  total = 0;
  g_timer_start (timer);
  for (i = 0; i < count; i++)
    {
      total += sum_generated (c);
      total += sum_generated (c);
      total += sum_generated (c);
      total += sum_generated (c);
      total += sum_generated (c);
    }
  g_timer_stop (timer);
  g_assert (total == 5 * count * 515);

  g_print ("Generated performance: %.1f kiloiterations per second\n", (count/1000.0)/g_timer_elapsed (timer, NULL));

  return 0;
}
