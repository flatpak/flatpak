#!/bin/env python3

import sys
from pyparsing import *
from pyparsing import pyparsing_common as ppc

LBRACK, RBRACK, LBRACE, RBRACE, COLON, SEMI = map(Suppress, "[]{}:;")

ident = Word(alphas + "_", alphanums + "_").setName("identifier")

named_types = {}

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

basic_types = {
    "boolean": ("b", True, 1),
    "byte": ("y", True, 1),
    "int16": ("n", True, 2),
    "uint16": ("q", True, 2),
    "int32": ("i", True, 4),
    "uint32": ("u", True, 4),
    "int64": ("x", True, 8),
    "uint64": ("t", True, 8),
    "handle": ("h", True, 4),
    "double": ("d", True, 8),
    "string": ("s", False, 1),
    "objectpath": ("o", False, 1),
    "signature": ("g", False, 1),
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
        for f in fields:
            f.offset = pos
            f.offset_index = index
            if f.type.is_fixed():
                pos = align_up(pos, f.type.alignment()) + f.type.get_fixed_size()
            else:
                pos = 0
                index = index + 1

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

if __name__ == "__main__":
    f = open(sys.argv[1], "r")
    testdata = f.read()
    try:
        results = typeDefs.parseString(testdata, parseAll=True)
        for t in results:
            print("%s - %s" % (t.name, t.type.typestring()))
            print("> ", t.type)
    except ParseException as pe:
        print("Parse error:", pe)
