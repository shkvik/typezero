#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// type, interface and enum.
class DeclParser {
public:
  explicit DeclParser(Parser &parser);

  Stmt *parseTypeAlias();
  Stmt *parseInterface();
  Stmt *parseEnum();

private:
  Parser &parser_;
};

} // namespace tz
