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
  static int binaryPrec(Tok kind);
  static BinaryOp binaryOpFor(Tok kind);
  static bool assignmentOp(Tok kind, std::optional<BinaryOp> &op);
  Parser &parser;
};

}
