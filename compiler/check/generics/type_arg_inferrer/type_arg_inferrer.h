#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypeArgInferrer {
public:
  explicit TypeArgInferrer(Checker &checker);

  bool bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                       llvm::ArrayRef<TypeRef> typeArgs, std::vector<const Type *> &bound);
  Expr *checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                         llvm::ArrayRef<TypeRef> typeArgs);
  bool finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound, llvm::SMLoc loc,
                      bool call, bool quiet = false);
  bool inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                          std::vector<const Type *> &bound);

private:
  Checker &checker_;
};

}
