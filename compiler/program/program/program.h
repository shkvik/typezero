#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>
#include <vector>

namespace tz {

class Checker;

// What goes from pass to pass: the files of the program, the trees of each round of loading
// (see EventRounds), what the passes report, the state of the check, and the program they built.
struct Program {
  Program();
  ~Program();

  llvm::SourceMgr sourceMgr;
  std::vector<std::unique_ptr<AstContext>> asts;
  std::unique_ptr<Diagnostics> diags;
  SourceFile file;
  // The symbols, scopes and types of the round being checked: from DeclarePass to
  // WholeProgramPass.
  std::unique_ptr<Checker> checker;
};

} // namespace tz
