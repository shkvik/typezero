#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

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

}
