TODO:
 * Inline code in lookup() to avoid recomputing. Also, maybe do special frame_size == 1 case
 * dicts: "[sorted int] value"
 * Generate structs for fixed structs, Struct_peek(), struct_peek_var()
 * Apply endianness attribute in getters
 * Add _peek_as_variant() for generated types
 * Add _peek_as_variant() variant with extra args for data + destry notify
 * Add more tests
 * More attributes: untrusted, sorted (for dict keys => bsearch)

Maybe TODO:
 * Add more optional checking (out of bounds, validity, etc)
