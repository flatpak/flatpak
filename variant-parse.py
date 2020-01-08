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

basic_types = {
    "boolean": ("b", True, 1, "gboolean"),
    "byte": ("y", True, 1, "guint8"),
    "int16": ("n", True, 2, "gint16"),
    "uint16": ("q", True, 2, "guint16"),
    "int32": ("i", True, 4, "gint32"),
    "uint32": ("u", True, 4, "guint32"),
    "int64": ("x", True, 8, "gint64"),
    "uint64": ("t", True, 8, "guint64"),
    "handle": ("h", True, 4, "guint32"),
    "double": ("d", True, 8, "double"),
    "string": ("s", False, 1, "const char *"),
    "objectpath": ("o", False, 1, "const char *"),
    "signature": ("g", False, 1, "const char *"),
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

    def __repr__(self):
         return "Field(%s, %s)" % (self.name, self.type)

    def propagate_typename(self, struct_name):
        self.type.set_typename (struct_name + "__" + self.name)

class StructType(Type):
    def __init__(self, fields):
        super().__init__()
        self.fields = list(fields)

        self._fixed = True
        pos = 0
        index = 0
        alignment = -1;
        for f in fields:
            pos = align_up(pos, f.type.alignment())
            f.offset = pos
            f.offset_index = index
            if alignment == -1:
                alignment = f.type.alignment()
            f.offset_alignment = alignment
            if f.type.is_fixed():
                pos = pos + f.type.get_fixed_size()
            else:
                pos = 0
                index = index + 1
                alignment = -1

        self._fixed = (index == 0)
        if self._fixed:
            if pos == 0:
                self._fixed_size = 1; # Special case unit struct
            else:
                # Round up to aligement
                self._fixed_size = align_up(pos, self.alignment())

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
