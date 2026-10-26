#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypeParams {
public:
  explicit TypeParams(Checker &checker);

  std::vector<const Type *> declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                              std::vector<const Type *> *defaults);
  void bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls, llvm::ArrayRef<const Type *> types);
  const Type *substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                         llvm::ArrayRef<const Type *> args);
  static const Type *representation(const Type *type);
  bool satisfies(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params = {},
                 llvm::ArrayRef<const Type *> args = {});
  bool hasMembersOf(const Type *arg, const Type *constraint);
  bool exactTypeParam(FuncSymbol *generic, size_t k);

private:
  Checker &checker_;
  llvm::SmallPtrSet<const Type *, 4> substituting_;
};

}
