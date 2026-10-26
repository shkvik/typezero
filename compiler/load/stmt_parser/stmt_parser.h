#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class StmtParser {
public:
  explicit StmtParser(Parser &parser);

  Stmt *parseStatement(bool topLevel = false);
  Stmt *skipDeclaration(llvm::SMLoc loc, const llvm::Twine &message);
  BlockStmt *parseBlock();
  Stmt *parseIf();
  Stmt *parseWhile();
  Stmt *parseDoWhile();
  Stmt *parseFor();
  Stmt *parseSwitch();
  Stmt *parseReturn();
  Stmt *parseThrow();
  Stmt *parseTry();
  Stmt *parseBreakOrContinue();

private:
  Parser &parser_;
};

}
