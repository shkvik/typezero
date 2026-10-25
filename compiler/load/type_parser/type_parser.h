#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Types: unions, functions, objects, tuples, mapped types and type arguments.
class TypeParser {
public:
  explicit TypeParser(Parser &parser);

  // `[K in ...` is ahead, in an object type: a mapped type.
  bool mappedTypeAhead();
  TypeRef parseMappedType(TypeRef &object, int readonlyChange);
  // Types
  TypeRef parseType();
  TypeRef parseUnionType();
  TypeRef parseIntersectionType();
  // A return type, which may be a type predicate: `x is T`, `asserts x is T`.
  TypeRef parseReturnType();
  TypeRef parseArrayType();
  TypeRef parsePrimaryType();
  // `(a: int, b?: string) => T`, with the lexer at `(`.
  bool functionTypeAhead();
  TypeRef parseFunctionType();
  // `(a: int, b?: string)` of a function type or a method in a type, as a
  // Function TypeRef without its result yet; false after a syntax error.
  bool parseSignatureParams(TypeRef &ref);
  TypeRef parseObjectType();
  // `[string, int]`, with the lexer at `[`.
  TypeRef parseTupleType();
  // The name of a field in an object type or literal: an identifier, a
  // keyword or a string literal.
  bool parseFieldName(llvm::StringRef &name);
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
