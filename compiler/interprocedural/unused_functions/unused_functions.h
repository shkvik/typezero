#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/source_file/source_file.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

class UnusedFunctions {
public:
  explicit UnusedFunctions(SourceFile &checked);

  llvm::SmallPtrSet<const FunctionDecl *, 8> findUnused();

private:
  void reachFromRoots();
  void reachFromQueue();
  void reach(const Expr *e);

  SourceFile &file;
  llvm::SmallPtrSet<const FunctionDecl *, 16> used;
  std::vector<const FunctionDecl *> queue;
};

}
