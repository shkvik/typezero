#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ResultTypes {
public:
  explicit ResultTypes(CheckState &owner);

  llvm::StringRef resultName(llvm::StringRef name) const;
  llvm::StringRef resultTypeName(llvm::StringRef name) const;
  void rejectNeverthrowImport(const ImportDecl &decl);
  const Type *resolveResultType(const TypeRef &ref, llvm::StringRef name);
  static const Type *unifyHalf(const Type *a, const Type *b);
  void neverOperands(BinaryExpr *bin);

private:
  static llvm::StringRef findName(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef name);

  CheckState &state;
};

}
