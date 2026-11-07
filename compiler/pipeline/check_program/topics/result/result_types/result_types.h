#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/result/result_types/result_types_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ResultTypes {
public:
  using ResultImportSource = Result<const ImportDecl *, ResultTypesImportSourceError>;

  explicit ResultTypes(CheckState &owner);

  llvm::StringRef resultName(llvm::StringRef name) const;
  llvm::StringRef resultTypeName(llvm::StringRef name) const;
  static ResultImportSource importSource(const ImportDecl &decl);
  const Type *resolveResultType(const TypeRef &ref, llvm::StringRef name);
  static const Type *unifyHalf(const Type *a, const Type *b);
  void neverOperands(BinaryExpr *bin);

private:
  using ResultTypeArgsPair = Result<const TypeRef *, ResultTypesResultTypeArgsError>;

  static ResultTypeArgsPair resultTypeArgs(const TypeRef &ref);

  static llvm::StringRef findName(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef name);

  CheckState &state;
};

}
