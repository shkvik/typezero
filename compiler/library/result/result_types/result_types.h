#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ResultTypes {
public:
  explicit ResultTypes(Checker &owner);

  llvm::StringRef resultName(llvm::StringRef name) const;
  llvm::StringRef resultTypeName(llvm::StringRef name) const;
  void rejectNeverthrowImport(const ImportDecl &decl);
  const Type *resolveResultType(const TypeRef &ref, llvm::StringRef name);
  static const Type *unifyHalf(const Type *a, const Type *b);
  void neverOperands(BinaryExpr *bin);

private:
  static llvm::StringRef findName(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef name);

  Checker &checker;
};

}
