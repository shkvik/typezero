#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class FunctionTypeParser {
public:
  explicit FunctionTypeParser(Parser &parser);

  bool functionTypeAhead();
  TypeRef parseFunctionType();
  bool parseSignatureParams(TypeRef &ref);

private:
  Parser &parser_;
};

}
