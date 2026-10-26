#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class ExprParser {
public:
  explicit ExprParser(Parser &parser);

  Expr *parseSequence();
  Expr *parseExpr();
  Expr *parseYield();
  Expr *parseConditional();
  Expr *parseBinary(int minPrec);
  Expr *parseUnary();
  Expr *parsePostfix();

private:
  Parser &parser_;
};

}
