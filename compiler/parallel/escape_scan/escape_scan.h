#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class EscapeScan {
public:
  explicit EscapeScan(Analysis &analysis);

  const Analysis::Escapes &escapes(const FunctionDecl *owner);
  void scanStmt(const Stmt *s, Analysis::Escapes &out);
  void scanUses(const Expr *e, bool safe, Analysis::Escapes &out, bool discarded = false);
  bool constructorLeaks(const ClassDecl *cls);

private:
  Analysis &analysis_;
};

}
