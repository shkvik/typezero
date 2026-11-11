#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/source_file/source_file.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

class UnusedFunctions {
public:
  explicit UnusedFunctions(SourceFile &checked);

  llvm::SmallPtrSet<const FunctionDecl *, 8> findUnused() const;

private:
  const FunctionDecl *referencedFunction(const Expr *e) const;
  std::vector<const FunctionDecl *> referencedFunctions(const Stmt *stmt) const;
  std::vector<const FunctionDecl *> rootFunctions() const;
  llvm::SmallPtrSet<const FunctionDecl *, 16> findUsed() const;

  SourceFile &file;
};

}
