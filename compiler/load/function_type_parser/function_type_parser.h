#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Function types and the parameters of signatures.
class FunctionTypeParser {
public:
  explicit FunctionTypeParser(Parser &parser);

  // `(a: int, b?: string) => T`, with the lexer at `(`.
  bool functionTypeAhead();
  TypeRef parseFunctionType();
  // `(a: int, b?: string)` of a function type or a method in a type, as a
  // Function TypeRef without its result yet; false after a syntax error.
  bool parseSignatureParams(TypeRef &ref);

private:
  Parser &parser_;
};

} // namespace tz
