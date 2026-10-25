#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Statements: blocks, if, loops, switch, return, throw and try, break and continue.
class StmtParser {
public:
  explicit StmtParser(Parser &parser);

  // Statements. `import` and `export` are allowed only at the top level.
  Stmt *parseStatement(bool topLevel = false);
  // `namespace X { }`, `declare module "x" { }`, `declare const x: T`:
  // reports `message` and skips the declaration.
  Stmt *skipDeclaration(llvm::SMLoc loc, const llvm::Twine &message);
  BlockStmt *parseBlock();
  Stmt *parseIf();
  Stmt *parseWhile();
  Stmt *parseDoWhile();
  Stmt *parseFor();
  Stmt *parseSwitch();
  Stmt *parseReturn();
  // `throw value` and `try { } catch (e) { } finally { }` (docs/errors.md).
  Stmt *parseThrow();
  Stmt *parseTry();
  Stmt *parseBreakOrContinue();

private:
  Parser &parser_;
};

} // namespace tz
