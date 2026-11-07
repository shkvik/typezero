#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class ParallelHelpers {
public:
  static bool onlyReadsArgs(Builtin b);
  static const Expr *strip(const Expr *e);
  static bool knownBuiltin(Builtin builtin);
  static bool makesNewObject(Builtin builtin);
  static bool holdsReferences(const Type *type);
  static llvm::StringRef libraryClassName(const ClassDecl *cls);
};

}
