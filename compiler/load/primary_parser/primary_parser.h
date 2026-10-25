#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Primary expressions: names, literals of numbers, arrays and objects, parentheses.
class PrimaryParser {
public:
  explicit PrimaryParser(Parser &parser);

  Expr *parsePrimary();
  Expr *parseArrayLiteral();
  Expr *parseObjectLiteral();
  Expr *parseIntLiteral();
  Expr *parseFloatLiteral();

private:
  Parser &parser_;
};

} // namespace tz
