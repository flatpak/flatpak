#!/bin/env python3

import sys
from pyparsing import *
from pyparsing import pyparsing_common as ppc

LBRACK, RBRACK, LBRACE, RBRACE, COLON, SEMI = map(Suppress, "[]{}:;")

ident = Word(alphas + "_", alphanums + "_").setName("identifier")

named_types = {}

class TypeDef:
    def __init__(self, name, type):
        self.name = name
        self.type = type

        named_types[name] = type

class Type:
    def __init__(self):
        pass

    def resolve(self):
        return self

    def typestring(self):
        assert False

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
        assert kind in basic_typestrings
        self.kind = kind
    def __repr__(self):
         return "BasicType(%s)" % self.kind
    def typestring(self):
         return basic_typestrings[self.kind]

class ArrayType(Type):
    def __init__(self, element_type):
        self.element_type = element_type
    def __repr__(self):
         return "ArrayType(%s)" % repr(self.element_type)
    def typestring(self):
         return "a" + self.element_type.typestring()

class DictType(Type):
    def __init__(self, key_type, element_type):
        self.key_type = key_type
        self.element_type = element_type
    def __repr__(self):
         return "DictType(%s, %s)" % (repr(self.key_type), repr(self.element_type))
    def typestring(self):
         return "a{%s%s}" % (self.key_type.typestring(), self.element_type.typestring())

class MaybeType(Type):
    def __init__(self, element_type):
        self.element_type = element_type
    def __repr__(self):
         return "MaybeType(%s)" % repr(self.element_type)
    def typestring(self):
         return "m" + self.element_type.typestring()

class VariantType(Type):
    def __init__(self):
        pass
    def __repr__(self):
         return "VariantType()"
    def typestring(self):
         return "v"

class Field:
    def __init__(self, name, attributes, type):
        self.name = name
        self.attributes = attributes
        self.type = type

    def __repr__(self):
         return "Field(%s, %s)" % (self.name, self.type)

class StructType(Type):
    def __init__(self, fields):
        self.fields = list(fields)
    def __repr__(self):
        return "StructType(%s)" % ",".join(map(repr, self.fields))

    def typestring(self):
        res = ['(']
        for f in self.fields:
            res.append(f.type.typestring())
        res.append(')')
        return "".join(res)

class NamedType(Type):
    def __init__(self, name):
        self.name = name
    def __repr__(self):
        return "NamedType(%s)" % self.name

    def resolve(self):
        assert self.name in named_types
        return named_types[self.name]

    def typestring(self):
        return self.resolve().typestring()

typeSpec = Forward()

basicType = oneOf(basic_typestrings.keys()).setParseAction(lambda toks: BasicType(toks[0]))

variantType = Keyword("variant").setParseAction(lambda toks: VariantType())

arrayType = (LBRACK + RBRACK + typeSpec).setParseAction(lambda toks: ArrayType(toks[0]))

dictType = (LBRACK + basicType + RBRACK + typeSpec).setParseAction(lambda toks: DictType(toks[0], toks[1]))

maybeType = (Suppress("?") + typeSpec).setParseAction(lambda toks: MaybeType(toks[0]))

fieldAttribute = oneOf("bigendian littleendian nativeendian")

field = (ident + COLON + Group(ZeroOrMore(fieldAttribute)) + typeSpec + SEMI).setParseAction(lambda toks: Field(toks[0], toks[1], toks[2]))

structType = (LBRACE + ZeroOrMore(field) + RBRACE).setParseAction(lambda toks: StructType(toks))

namedType = ident.copy().setParseAction(lambda toks: NamedType(toks[0]))

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
    except ParseException as pe:
        print("Parse error:", pe)
