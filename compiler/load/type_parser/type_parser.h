#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class TypeParser {
public:
  explicit TypeParser(Parser &parser);

  TypeRef parseType();
  TypeRef parseUnionType();
  TypeRef parseIntersectionType();
  TypeRef parseReturnType();
  TypeRef parseArrayType();
  TypeRef parsePrimaryType();
  std::vector<TypeRef> parseTypeArgs();
  bool tryTypeArgs(std::vector<TypeRef> &args, bool callOnly = false);

private:
  Parser &parser_;
};

}
