#!/bin/env python3

import sys
import os
from pyparsing import *
from pyparsing import pyparsing_common as ppc

LBRACK, RBRACK, LBRACE, RBRACE, COLON, SEMI = map(Suppress, "[]{}:;")

ident = Word(alphas + "_", alphanums + "_").setName("identifier")

named_types = {}

def generate_header(filename):
    print(
"""/* generated code for {filename} */
#include <glib.h>
typedef struct {{
 gconstpointer base;
 gsize size;
}} VariantChunk;

/* Note: clz is undefinded for 0, so never call this size == 0 */
G_GNUC_CONST static inline guint
variant_chunk_get_offset_size (gsize size)
{{
#if defined(__GNUC__) && (__GNUC__ >= 4) && defined(__OPTIMIZE__)
  /* Instead of using a lookup table we use nibbles in a lookup word */
  guint32 v = (guint32)0x88884421;
  return (v >> (((__builtin_clzl(size) ^ 63) / 8) * 4)) & 0xf;
#else
  if (size > G_MAXUINT16)
    {{
      if (size > G_MAXUINT32)
        return 8;
      else
        return 4;
    }}
  else
    {{
      if (size > G_MAXUINT8)
         return 2;
      else
         return 1;
    }}
#endif
}}

G_GNUC_PURE static inline gsize
variant_chunk_read_unaligned_le (guchar *bytes, guint   size)
{{
  union
  {{
    guchar bytes[GLIB_SIZEOF_SIZE_T];
    gsize integer;
  }} tmpvalue;

  tmpvalue.integer = 0;
  /* we unroll the size checks here so that memcpy gets constant args */
  if (size >= 4)
    {{
      if (size == 8)
        memcpy (&tmpvalue.bytes, bytes, 8);
      else
        memcpy (&tmpvalue.bytes, bytes, 4);
    }}
  else
    {{
      if (size == 2)
        memcpy (&tmpvalue.bytes, bytes, 2);
      else
        memcpy (&tmpvalue.bytes, bytes, 1);
    }}

  return GSIZE_FROM_LE (tmpvalue.integer);
}}

static inline void
__variant_string_append_double (GString *string, double d)
{{
  gchar buffer[100];
  gint i;

  g_ascii_dtostr (buffer, sizeof buffer, d);
  for (i = 0; buffer[i]; i++)
    if (buffer[i] == '.' || buffer[i] == 'e' ||
        buffer[i] == 'n' || buffer[i] == 'N')
      break;

  /* if there is no '.' or 'e' in the float then add one */
  if (buffer[i] == '\\0')
    {{
      buffer[i++] = '.';
      buffer[i++] = '0';
      buffer[i++] = '\\0';
    }}
   g_string_append (string, buffer);
}}

static inline void
__variant_string_append_string (GString *string, const char *str)
{{
  gunichar quote = strchr (str, '\\'') ? '"' : '\\'';

  g_string_append_c (string, quote);
  while (*str)
    {{
      gunichar c = g_utf8_get_char (str);

      if (c == quote || c == '\\\\')
        g_string_append_c (string, '\\\\');

      if (g_unichar_isprint (c))
        g_string_append_unichar (string, c);
      else
        {{
          g_string_append_c (string, '\\\\');
          if (c < 0x10000)
            switch (c)
              {{
              case '\\a':
                g_string_append_c (string, 'a');
                break;

              case '\\b':
                g_string_append_c (string, 'b');
                break;

              case '\\f':
                g_string_append_c (string, 'f');
                break;

              case '\\n':
                g_string_append_c (string, 'n');
                break;

              case '\\r':
                g_string_append_c (string, 'r');
                break;

              case '\\t':
                g_string_append_c (string, 't');
                break;

              case '\\v':
                g_string_append_c (string, 'v');
                break;

              default:
                g_string_append_printf (string, "u%04x", c);
                break;
              }}
           else
             g_string_append_printf (string, "U%08x", c);
        }}

      str = g_utf8_next_char (str);
    }}

  g_string_append_c (string, quote);
}}
""".format(filename=filename))

def generate_footer(filename):
    print(
"""
""".format(filename=filename))

def align_down(value, alignment):
    return value & ~(alignment - 1)

def align_up(value, alignment):
    return align_down(value + alignment - 1, alignment)

def add_named_type(name, type):
    assert not name in named_types
    type.set_typename(name, True)
    named_types[name] = type

def get_named_type(name):
    assert name in named_types
    return named_types[name]

class TypeDef:
    def __init__(self, name, type):
        self.name = name
        self.type = type

        add_named_type(name, type)

    def generate(self, generated):
        def do_generate (type, generated):
            for c in type.get_children():
                do_generate (c, generated)
            if type.typename != None and type.typename not in generated:
                generated[type.typename] = True
                type.generate()

        do_generate(self.type, generated)


class Type:
    def __init__(self):
        self.typename = None

    def typestring(self):
        assert False

    def set_typename(self, name, override = False):
        if self.typename == None or override:
            self.typename = name
            self.propagate_typename(name)

    def propagate_typename(self, typename):
        pass

    def is_basic(self):
        return False

    def is_fixed(self):
        return False

    def get_fixed_size(self):
         assert False # Should not be reached

    def alignment(self):
        return 1

    def get_children(self):
        return []

    def generate(self):
        print("/* TODO: Generate %s -- %s */" % (self.typename, self))

    def get_ctype(self):
         return self.typename

    def can_printf_format(self):
         return False

    def generate_append_value(self, value, with_type_annotate):
        print ("  {typename}_format ({value}, s, {ta});".format(typename=self.typename, value=value, ta="type_annotate" if with_type_annotate else "FALSE"))

basic_types = {
    "boolean": ("b", True, 1, "gboolean", "", '%s'),
    "byte": ("y", True, 1, "guint8", "byte ", '0x%02x'),
    "int16": ("n", True, 2, "gint16", "int16 ", '%"G_GINT16_FORMAT"'),
    "uint16": ("q", True, 2, "guint16", "uint16 ", '%"G_GUINT16_FORMAT"'),
    "int32": ("i", True, 4, "gint32", "", '%"G_GINT32_FORMAT"'),
    "uint32": ("u", True, 4, "guint32", "uint32 ", '%"G_GUINT32_FORMAT"'),
    "int64": ("x", True, 8, "gint64", "int64 ", '%"G_GINT64_FORMAT"'),
    "uint64": ("t", True, 8, "guint64", "uint64 ", '%"G_GUINT64_FORMAT"'),
    "handle": ("h", True, 4, "guint32", "handle ", '%"G_GINT32_FORMAT"'),
    "double": ("d", True, 8, "double", "", None), # double formating is special
    "string": ("s", False, 1, "const char *", "", None), # String formating is special
    "objectpath": ("o", False, 1, "const char *", "objectpath ", '\\"%s\"'),
    "signature": ("g", False, 1, "const char *", "signature ", '\\"%s\\"'),
}

class BasicType(Type):
    def __init__(self, kind):
        super().__init__()
        assert kind in basic_types
        self.kind = kind
    def __repr__(self):
         return "BasicType(%s)" % self.kind
    def typestring(self):
         return basic_types[self.kind][0]
    def set_typename(self, name):
        pass # No names for basic types
    def is_basic(self):
        return True
    def is_fixed(self):
         return basic_types[self.kind][1]
    def get_fixed_size(self):
         return basic_types[self.kind][2]
    def alignment(self):
         return basic_types[self.kind][2]
    def get_ctype(self):
         return basic_types[self.kind][3]
    def get_read_ctype(self):
        if self.kind == "boolean":
            return "guint8"
        return self.get_ctype()
    def get_type_annotation(self):
        return basic_types[self.kind][4]
    def get_format_string(self):
        return basic_types[self.kind][5]
    def convert_value_for_format(self, value):
        if self.kind == "boolean":
            value = '(%s) ? "true" : "false"' % value
        return value
    def can_printf_format(self):
         return self.get_format_string() != None
    def generate_append_value(self, value, with_type_annotate):
        # Special case some basic types
        if self.kind == "string":
            print ('  __variant_string_append_string (s, %s);' % value)
        elif self.kind == "string":
            print ('  __variant_string_append_double (s, %s);' % value)
        else:
            value = self.convert_value_for_format(value)
            if with_type_annotate and self.get_type_annotation() != "":
                print ('  g_string_append_printf (s, "%s{format}", type_annotate ? "{annotate}" : "", {value});'
                       .format(format=self.get_format_string(),
                               annotate=self.get_type_annotation(),
                               value=value))
            else:
                print ('  g_string_append_printf (s, "{format}", {value});'
                       .format(format=self.get_format_string(),
                               value=value))

class ArrayType(Type):
    def __init__(self, element_type):
        super().__init__()
        self.element_type = element_type

        if element_type.is_basic():
            self.typename = self.element_type.kind + "array"

    def __repr__(self):
         return "ArrayType<%s>(%s)" % (self.typename, repr(self.element_type))
    def typestring(self):
         return "a" + self.element_type.typestring()
    def propagate_typename(self, name):
        self.element_type.set_typename (name + "__element")
    def alignment(self):
        return self.element_type.alignment()
    def get_children(self):
        return [self.element_type]

class DictType(Type):
    def __init__(self, key_type, element_type):
        super().__init__()
        self.key_type = key_type
        self.element_type = element_type
    def __repr__(self):
         return "DictType<%s>(%s, %s)" % (self.typename, repr(self.key_type), repr(self.element_type))
    def typestring(self):
         return "a{%s%s}" % (self.key_type.typestring(), self.element_type.typestring())
    def propagate_typename(self, name):
        self.element_type.set_typename (name + "__element")
    def alignment(self):
        return max(self.element_type.alignment(), self.key_type.alignment())
    def get_children(self):
        return [self.key_type, self.element_type]

class MaybeType(Type):
    def __init__(self, element_type):
        super().__init__()
        self.element_type = element_type
        if element_type.is_basic():
            self.typename = "maybe" + self.element_type.kind
    def __repr__(self):
         return "MaybeType<%s>(%s, %s)" % (self.typename, repr(self.element_type))
    def typestring(self):
         return "m" + self.element_type.typestring()
    def propagate_typename(self, name):
        self.element_type.set_typename (name + "__element")
    def alignment(self):
        return self.element_type.alignment()
    def get_children(self):
        return [self.element_type]

    def generate(self):
        print (
'''
typedef VariantChunk {typename};
#define {typename}_typestring "{typestring}"
static inline {typename} {typename}_from_variant(GVariant *v) {{
    {typename} val = {{ g_variant_get_data (v), g_variant_get_size (v) }};
    return val;
}}'''.format(typename=self.typename, typestring=self.typestring()))

        # has_value
        print ("static inline gboolean {typename}_has_value({typename} v) {{".format(typename=self.typename, ctype=self.get_ctype()))
        print("  return v.size != 0;")
        print("}")

        # Getter
        print ("static inline {ctype} {typename}_get_value({typename} v) {{".format(typename=self.typename, ctype=self.element_type.get_ctype()))
        print("  g_assert(v.size != 0);")

        if self.element_type.is_basic():
            if self.element_type.is_fixed():
                print ("  return (%s)*((%s *)v.base);" % (self.element_type.get_ctype(), self.element_type.get_read_ctype()))
            else: # string
                print ("  return (%s)v.base;" % (self.element_type.get_ctype()))
        else:
            if self.element_type.is_fixed():
                # Fixed means use whole size
                size = "v.size"
            else:
                # Otherwise, ignore extra zero byte
                size = "(v.size - 1)"
            print ("  %s val = { v.base, %s};" % (self.element_type.typename, size))
            print ("  return val;")
        print("}")

        print ("static inline void {typename}_format ({typename} v, GString *s, gboolean type_annotate) {{".format(typename=self.typename))
        print ("  if (type_annotate)")
        print ('    g_string_append_printf (s, "@%%s ", %s_typestring);' % (self.typename))
        print ("  if (v.size != 0)")
        print ("    {")
        if isinstance(self.element_type, MaybeType):
            print ('      g_string_append (s, "just ");')
        print ('    ', end='')
        self.element_type.generate_append_value("{typename}_get_value(v)".format(typename=self.typename), False)
        print ("    }")
        print ("  else")
        print ("    {")
        print ('      g_string_append (s, "nothing");')
        print ("    }")
        print ("}")
        print ("static inline char * {typename}_print ({typename} v, gboolean type_annotate) {{".format(typename=self.typename))
        print ('  GString *s = g_string_new ("");')
        print ("  {typename}_format (v, s, type_annotate);".format(typename=self.typename))
        print ('  return g_string_free (s, FALSE);')
        print ("}")

class VariantType(Type):
    def __init__(self):
        super().__init__()
        self.typename = "variant"
    def __repr__(self):
         return "VariantType()"
    def typestring(self):
         return "v"
    def set_typename(self, name):
        pass # No names for variant
    def alignment(self):
        return 8

class Field:
    def __init__(self, name, attributes, type):
        self.name = name
        self.attributes = attributes
        self.type = type
        self.last = False

    def __repr__(self):
         return "Field(%s, %s)" % (self.name, self.type)

    def propagate_typename(self, struct_name):
        self.type.set_typename (struct_name + "__" + self.name)

    def generate(self, struct):
        # Getter
        print ("static inline {ctype} {structname}_get_{fieldname}({structname} v) {{".format(structname=struct.typename, ctype=self.type.get_ctype(), fieldname=self.name))
        has_offset_size = False
        if self.table_i == -1:
            offset = "%d" % (self.table_c)
        else:
            has_offset_size = True
            print ("  guint offset_size = variant_chunk_get_offset_size (v.size);");
            print ("  gsize last_end = variant_chunk_read_unaligned_le ((guchar*)(v.base) + v.size - offset_size * %d, offset_size);"  % (self.table_i + 1));
            offset = "((last_end + %d) & (~(gsize)%d)) + %d" % (self.table_a + self.table_b, self.table_b, self.table_c)

        if self.type.is_basic():
            if self.type.is_fixed():
                print ("  return (%s)G_STRUCT_MEMBER(%s, v.base, %s);" % (self.type.get_ctype(), self.type.get_read_ctype(), offset))
            else: # string
                print ("  return &G_STRUCT_MEMBER(char, v.base, %s);" % (offset))
        else:
            if self.type.is_fixed():
                print ("  %s val = { G_STRUCT_MEMBER_P(v.base, %s), %d };" % (self.type.typename, offset, self.type.get_fixed_size()))
                print ("  return val;")
            else:
                if not has_offset_size:
                    has_offset_size = True
                    print ("  guint offset_size = variant_chunk_get_offset_size (v.size);");
                print ("  gsize start = %s;" % offset);
                if self.last:
                    print ("  gsize end = v.size - offset_size * %d;" % (struct.framing_offset_size))
                else:
                    print ("  gsize end = variant_chunk_read_unaligned_le ((guchar*)(v.base) + v.size - offset_size * %d, offset_size);"  % (self.table_i + 2));
                print ("  %s val = { G_STRUCT_MEMBER_P(v.base, start), end - start };" % (self.type.typename))
                print ("  return val;")
        print("}")

class StructType(Type):
    def __init__(self, fields):
        super().__init__()
        self.fields = list(fields)

        if len(self.fields) > 0:
            self.fields[len(self.fields) - 1].last = True

        framing_offset_size = 0
        fixed = True
        fixed_pos = 0
        for f in fields:
            if f.type.is_fixed():
                fixed_pos = align_up(fixed_pos, f.type.alignment()) + f.type.get_fixed_size()
            else:
                fixed = False

            if not f.last:
                framing_offset_size = framing_offset_size + 1

        self.framing_offset_size = framing_offset_size
        self._fixed = fixed
        if fixed:
            if fixed_pos == 0: # Special case unit struct
                self._fixed_size = 1;
            else:
                # Round up to alignment
                self._fixed_size = align_up(fixed_pos, self.alignment())

        def tuple_align(offset, alignment):
            return offset + ((-offset) & alignment)

        # This is code equivalend to tuple_generate_table() in gvariantinfo.c, see its docs
        i = -1
        a = 0
        b = 0
        c = 0
        for f in fields:
            d = f.type.alignment() - 1;
            e = f.type.get_fixed_size() if f.type.is_fixed() else 0

            # align to 'd'
            if d <= b: # rule 1
                c = tuple_align(c, d)
            else: # rule 2
                a = a + tuple_align(c, b)
                b = d
                c = 0

            # the start of the item is at this point (ie: right after we
            # have aligned for it).  store this information in the table.
            f.table_i = i
            f.table_a = a
            f.table_b = b
            f.table_c = c

            # "move past" the item by adding in its size.
            if e == 0:
                # variable size:
                #
                # we'll have an offset stored to mark the end of this item, so
                # just bump the offset index to give us a new starting point
                # and reset all the counters.
                i = i + 1
                a = b = c = 0
            else:
                # fixed size
                c = c + e # rule 3

    def __repr__(self):
        return "StructType<%s>(%s)" % (self.typename, ",".join(map(repr, self.fields)))

    def typestring(self):
        res = ['(']
        for f in self.fields:
            res.append(f.type.typestring())
        res.append(')')
        return "".join(res)

    def get_children(self):
        children = []
        for f in self.fields:
            children.append(f.type)
        return children

    def propagate_typename(self, name):
        for f in self.fields:
            f.propagate_typename(name)

    def alignment(self):
        alignment = 1;
        for f in self.fields:
            alignment = max(alignment, f.type.alignment())
        return alignment

    def is_fixed(self):
        return self._fixed;
    def get_fixed_size(self):
        return self._fixed_size

    def generate(self):
        print (
'''
typedef VariantChunk {typename};
#define {typename}_typestring "{typestring}"
static inline {typename} {typename}_from_variant(GVariant *v) {{
    {typename} val = {{ g_variant_get_data (v), g_variant_get_size (v) }};
    return val;
}}'''.format(typename=self.typename, typestring=self.typestring()))
        for f in self.fields:
            f.generate(self)
        print ("static inline void {typename}_format ({typename} v, GString *s, gboolean type_annotate) {{".format(typename=self.typename))

        # Create runs of things we can combine into single printf
        field_runs = []
        current_run = None
        for f in self.fields:
            if current_run and f.type.can_printf_format() == current_run[0].type.can_printf_format():
                current_run.append(f)
            else:
                current_run = [f]
                field_runs.append(current_run)

        for i, run in enumerate(field_runs):
            if run[0].type.can_printf_format():
                # A run of printf fields
                print ('  g_string_append_printf (s, "%s' % ("(" if i == 0 else ""), end = '')
                for f in run:
                    if f.type.get_type_annotation() != "":
                        print ('%s', end = '')
                    print ('%s' % (f.type.get_format_string()), end = '')
                    if not f.last:
                        print (', ', end = '')
                    elif len(self.fields) == 1:
                        print (',)', end = '')
                    else:
                        print (')', end = '')
                print ('",')
                for j, f in enumerate(run):
                    if f.type.get_type_annotation() != "":
                        print ('                   type_annotate ? "%s" : "",' % (f.type.get_type_annotation()))
                    value = f.type.convert_value_for_format("{structname}_get_{fieldname}(v)".format(structname=self.typename, fieldname=f.name))
                    print ('                   %s%s' % (value, "," if j != len(run) - 1 else ");"))
            else:
                # A run of container fields
                if i == 0:
                    print ('  g_string_append (s, "(");')
                for f in run:
                    value = "{structname}_get_{fieldname}(v)".format(structname=self.typename, fieldname=f.name)
                    f.type.generate_append_value(value, True)
                    if not f.last:
                        print ('  g_string_append (s, ", ");')
                    elif len(self.fields) == 1:
                        print ('  g_string_append (s, ",)");')
                    else:
                        print ('  g_string_append (s, ")");')
        print ("}")
        print ("static inline char * {typename}_print ({typename} v, gboolean type_annotate) {{".format(typename=self.typename))
        print ('  GString *s = g_string_new ("");')
        print ("  {typename}_format (v, s, type_annotate);".format(typename=self.typename))
        print ('  return g_string_free (s, FALSE);')
        print ("}")

typeSpec = Forward()

basicType = oneOf(basic_types.keys()).setParseAction(lambda toks: BasicType(toks[0]))

variantType = Keyword("variant").setParseAction(lambda toks: VariantType())

arrayType = (LBRACK + RBRACK + typeSpec).setParseAction(lambda toks: ArrayType(toks[0]))

dictType = (LBRACK + basicType + RBRACK + typeSpec).setParseAction(lambda toks: DictType(toks[0], toks[1]))

maybeType = (Suppress("?") + typeSpec).setParseAction(lambda toks: MaybeType(toks[0]))

fieldAttribute = oneOf("bigendian littleendian nativeendian")

field = (ident + COLON + Group(ZeroOrMore(fieldAttribute)) + typeSpec + SEMI).setParseAction(lambda toks: Field(toks[0], toks[1], toks[2]))

structType = (LBRACE + ZeroOrMore(field) + RBRACE).setParseAction(lambda toks: StructType(toks))

namedType = ident.copy().setParseAction(lambda toks: get_named_type(str(toks[0])))

typeSpec <<= basicType ^ arrayType ^ maybeType ^ variantType ^ dictType ^ structType ^ namedType

typeDef = (Suppress(Keyword("type")) + ident + typeSpec + SEMI).setParseAction(lambda toks: TypeDef(toks[0], toks[1]))

typeDefs = ZeroOrMore(typeDef).ignore(cppStyleComment)

def generate(typedefs, filename):
    generate_header(filename)
    generated = {}
    for td in typedefs:
        td.generate(generated)
    generate_footer(filename)

if __name__ == "__main__":
    file = sys.argv[1]
    with  open(file, "r") as f:
        testdata = f.read()
        try:
            typedefs = typeDefs.parseString(testdata, parseAll=True)
            generate(typedefs, os.path.basename(file))
        except ParseException as pe:
            print("Parse error:", pe)
