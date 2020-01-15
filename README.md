variant-schema-compiler takes a schema describing a set of GVariant types and generates C code to efficiently access variants in these formats in a typesafe way.

A simple example is `gadget.gv`:
```
type Gadget {
  name: string;
  size: {
     width: int32;
     height: int32;
  };
  array: []int32;
  dict: [string]int32;
};
```

You compile this like:
```
variant-schema-compiler --prefix myapp --outfile gadget.h gadget.gv
```

Which will generate a header file `gadget.h` that you can include in your code.

This will define a structure MyappGadgetRef which you can create from a GVariant with
`myapp_gadget_from_gvariant()` or directly from raw memory using `myapp_gadget_from_bytes()` or
`myapp_gadget_from_data()`. You can then use the generated accessor functions to get the fields
of the variant.

As an example, here is a sample of accessors defined by the above:
```
const char *myapp_gadget_get_name (MyappGadgetRef v);
MyappGadgetSizeRef myapp_gadget_get_size (MyappGadgetRef v);
MyappArrayofint32Ref myapp_gadget_get_array (MyappGadgetRef v);
const gint32 *myapp_gadget_peek_array (MyappGadgetRef v,
                                       gsize *len);
gboolean myapp_gadget_dict_lookup (MyappGadgetDictRef v,
                                   const char * key,
                                   gint32 *out);
```

Using such accessors is much faster than working with the GVariant API for a number of reasons:
 * Getting a child item doesn't allocate a new GVariant object (no generated accessor API allocates memory).
 * No constant ref:ing and unref:ing of GVariants.
 * All the accessors are inlined and coded to be very efficient
 * Less validation than the generic GVariant APIs.

Additionally the APIs are just nicer to use due to using named fields of tuples/structs instead of indexes.

Of course there are some disadvantages, for instance:
  * Only pre-declared types are supported.
  * Less validation means you should not use it for untrusted data.
  * Lack of ownership (ref/unref) means you have to make sure memory ownership is correct in other ways.

However, in many usecases this is not a problem.

Note that the main binding object (the XxxRef:s) are value types (internally they are a basepointer + size tuple) and
not pointers. Also, there is no reference counting, so it is up to the caller to ensure that the memory referenced by
the variant ref is kept alive.

Some GVariant types are "fixed size", which means that all instances of these types have a well known size. 
GVariant encodes such data in a way that is very efficient to access, (with padding as needed, etc).
For such types the compiler also generates a regular struct, which makes it very easy to use these.

For example:
```
type Fixed {
  small: byte;
  large: uint32;
  medium: int16;
  other: {
    huge: double;
  };
};

type User {
  one: Fixed;
  many:  []Fixed;
};
```

Will generate: (among other things)
```
typedef struct {
  double huge;
} MyappFixedOther;

typedef struct {
  guint8 small;
  guchar _padding1[3];
  guint32 large;
  gint16 medium;
  guchar _padding2[6];
  MyappFixedOther other;
} MyappFixed;

MyappFixed *myapp_fixed_peek (MyappFixedRef v);
const MyappFixed *myapp_user_peek_one (MyappUserRef v);
const MyappFixed *myapp_user_peek_many (MyappUserRef v, gsize *len);
```

Here are some more interesting features:
 * field endianness attributes which are auto-applied by the getters().
 * Inline naming of subtypes.
 * Automatically generated formatters/printers.
 * Generation of gvariant typestrings and field index defines for the types
