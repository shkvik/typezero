#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ResultTypes {
public:
  explicit ResultTypes(Checker &checker);

  llvm::StringRef neverthrowName(llvm::StringRef name) const;
  llvm::StringRef neverthrowTypeName(llvm::StringRef name) const;
  void importNeverthrow(Module *module, const ImportDecl &decl);
  const Type *resolveResultType(const TypeRef &ref, llvm::StringRef name);
  static const Type *unifyHalf(const Type *a, const Type *b);
  void neverOperands(BinaryExpr *bin);

private:
  Checker &checker_;
};

}
