#!/bin/env python3

import sys
from pyparsing import *
from pyparsing import pyparsing_common as ppc

LBRACK, RBRACK, LBRACE, RBRACE, COLON, SEMI = map(Suppress, "[]{}:;")

ident = Word(alphas + "_", alphanums + "_").setName("identifier")

named_types = {}

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

basic_typestrings = {
    "boolean": "b",
    "byte": "y",
    "int16": "n",
    "uint16": "q",
    "int32": "i",
    "uint32": "u",
    "int64": "x",
    "uint64": "t",
    "handle": "h",
    "double": "d",
    "string": "s",
    "objectpath": "o",
    "signature": "g"
}

class BasicType(Type):
    def __init__(self, kind):
        super().__init__()
        assert kind in basic_typestrings
        self.kind = kind
    def __repr__(self):
         return "BasicType(%s)" % self.kind
    def typestring(self):
         return basic_typestrings[self.kind]
    def set_typename(self, name):
        pass # No names for basic types
    def is_basic(self):
        return True

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

typeSpec = Forward()

basicType = oneOf(basic_typestrings.keys()).setParseAction(lambda toks: BasicType(toks[0]))

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
