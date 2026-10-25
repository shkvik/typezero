#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// Which objects a function lets go: where the references it makes may end up, and the
// constructors that let `this` out.
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

} // namespace tz::parallel
