#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GenericInstantiator {
public:
  explicit GenericInstantiator(Checker &checker);

  Expr *checkGenericValue(NameExpr *name, FuncSymbol *generic, const Checker::Signature *expected);
  bool returnsOwnClass(const FuncSymbol *generic, const Type *ret);
  FuncSymbol *instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args, llvm::SMLoc loc,
                          llvm::ArrayRef<const Type *> exact = {});
  std::vector<const Type *> exactClassArgs(CallExpr *call, const FuncSymbol *generic);
  FunctionDecl *instanceDecl(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                             llvm::ArrayRef<const Type *> exact = {});
  void checkInstance(Checker::Instance *instance);
  void reportInstance(Checker::Instance *instance, llvm::SMLoc callLoc,
                      llvm::ArrayRef<const Type *> concrete);
  void checkInstances();

private:
  Checker &checker_;
  std::map<std::pair<const FuncSymbol *, std::vector<const Type *>>, Checker::Instance *>
      instances_;
  llvm::SmallPtrSet<const FuncSymbol *, 2> ownClassReturns_;
};

}
