#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class CallParser {
public:
  explicit CallParser(Parser &parser);

  Expr *parseCallOrMember();
  Expr *parseNew();

private:
  Parser &parser_;
};

}
