#include "ostree_test.h"


static guint
slow_get_offset_size (gsize size)
{
  if (size > G_MAXUINT16)
    {
      if (size > G_MAXUINT32)
        return 8;
      else
        return 4;
    }
  else
    {
      if (size > G_MAXUINT8)
         return 2;
      else
         return 1;
    }
}

int
main (int argc,
      char *argv[])
{
  g_print ("Validating ot_ref_get_offset_size up to G_MAXUINT32\n");
  for (gsize i = 1; i < G_MAXUINT32; i++)
    {
      guint res = ot_ref_get_offset_size (i);
      if (res != slow_get_offset_size (i))
        {
          g_print ("failed: ot_ref_get_offset_size (%"G_GSIZE_FORMAT") == %d, should be %d\n", i, res, slow_get_offset_size (i));
          exit (1);
        }
    }

#if GLIB_SIZEOF_SIZE_T > 4
  g_print ("Validating ot_ref_get_offset_size up to 2*G_MAXUINT32\n");
  for (gsize i = (gsize)G_MAXUINT32; i < (gsize)G_MAXUINT32 * 2; i++)
    {
      guint res = ot_ref_get_offset_size (i);
      if (res != slow_get_offset_size (i))
        {
          g_print ("failed: ot_ref_get_offset_size (%"G_GSIZE_FORMAT") == %d, should be %d\n", i, res, slow_get_offset_size (i));
          exit (1);
        }
    }
#endif

  return 0;
}
