#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/program/ast/ast.h"

namespace tz::parallel {

class EscapeScan {
public:
  explicit EscapeScan(Analysis &analysis);

  const Escapes &escapes(const FunctionDecl *owner);
  void scanStmt(const Stmt *s, Escapes &out);
  void scanUses(const Expr *e, bool safe, Escapes &out, bool discarded = false);
  bool constructorLeaks(const ClassDecl *cls);

private:
  static bool keepsReceiver(Builtin b, bool discarded);
  Analysis &analysis;
};

}
