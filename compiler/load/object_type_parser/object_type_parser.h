#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Object types, mapped types and tuples, and the names of their fields.
class ObjectTypeParser {
public:
  explicit ObjectTypeParser(Parser &parser);

  // `[K in ...` is ahead, in an object type: a mapped type.
  bool mappedTypeAhead();
  TypeRef parseMappedType(TypeRef &object, int readonlyChange);
  TypeRef parseObjectType();
  // `[string, int]`, with the lexer at `[`.
  TypeRef parseTupleType();
  // The name of a field in an object type or literal: an identifier, a
  // keyword or a string literal.
  bool parseFieldName(llvm::StringRef &name);

private:
  Parser &parser_;
};

} // namespace tz
