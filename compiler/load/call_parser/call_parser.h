#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Calls, members, indexing and `new`: what follows a primary expression.
class CallParser {
public:
  explicit CallParser(Parser &parser);

  Expr *parseCallOrMember();
  Expr *parseNew();

private:
  Parser &parser_;
};

} // namespace tz
