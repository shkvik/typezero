#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Expressions, from the comma operator down to the primary ones.
class ExprParser {
public:
  explicit ExprParser(Parser &parser);

  // Expressions, from lowest to highest precedence. parseSequence admits the
  // comma operator; parseExpr, used where a comma separates (arguments, array
  // elements, declarators), does not.
  Expr *parseSequence();
  Expr *parseExpr();
  // At `yield`, inside a generator.
  Expr *parseYield();
  Expr *parseConditional();
  Expr *parseBinary(int minPrec);
  Expr *parseUnary();
  Expr *parsePostfix();
  Expr *parseCallOrMember();
  Expr *parsePrimary();
  Expr *parseArrayLiteral();
  Expr *parseObjectLiteral();
  Expr *parseNew();
  Expr *parseIntLiteral();
  Expr *parseFloatLiteral();

private:
  Parser &parser_;
};

} // namespace tz
