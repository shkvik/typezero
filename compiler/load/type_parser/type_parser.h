#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Types: unions, functions, objects, tuples, mapped types and type arguments.
class TypeParser {
public:
  explicit TypeParser(Parser &parser);

  // Types
  TypeRef parseType();
  TypeRef parseUnionType();
  TypeRef parseIntersectionType();
  // A return type, which may be a type predicate: `x is T`, `asserts x is T`.
  TypeRef parseReturnType();
  TypeRef parseArrayType();
  TypeRef parsePrimaryType();
  std::vector<TypeRef> parseTypeArgs();
  // `f<int>(x)` and `f<int>` in an expression: type arguments if the `<`
  // here starts some that a call or the end of the expression follows, as
  // TypeScript decides; otherwise `<` is less-than, and nothing is consumed.
  // With `callOnly`, only a call may follow (`JSON.parse<User>(text)`).
  bool tryTypeArgs(std::vector<TypeRef> &args, bool callOnly = false);

private:
  Parser &parser_;
};

} // namespace tz
