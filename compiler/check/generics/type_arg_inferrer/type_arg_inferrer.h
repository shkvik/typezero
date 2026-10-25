#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The type arguments of a generic call, from its arguments (docs/generics.md).
class TypeArgInferrer {
public:
  explicit TypeArgInferrer(Checker &checker);

  // Calls of generic functions (and of the constructors of generic
  // classes): infers or resolves the type arguments into `bound` and checks
  // the arguments with them; false after an error.
  bool bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                       llvm::ArrayRef<TypeRef> typeArgs, std::vector<const Type *> &bound);
  Expr *checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                         llvm::ArrayRef<TypeRef> typeArgs);
  // The type arguments of a call (or a value with `call` unset), those
  // given or inferred, completed with the defaults. Reports those still
  // missing and those that do not satisfy their constraints; false then.
  bool finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound, llvm::SMLoc loc,
                      bool call, bool quiet = false);
  // The type arguments of a call of `generic` with these checked arguments,
  // by inference alone; false if some cannot be found or do not fit, with
  // nothing reported: a variant that does not fit is left out.
  bool inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                          std::vector<const Type *> &bound);

private:
  Checker &checker_;
};

} // namespace tz
